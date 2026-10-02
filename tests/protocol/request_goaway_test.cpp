#include "moq/interop/scenarios/request_goaway.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/wire/draft18/messages.h"
#include "moq/interop/wire/draft21/goaway.h"
#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>

using namespace moq::interop::scenarios;
namespace {
namespace transport = moq::interop::transport;
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
std::vector<RequestGoawayProbe> profiles() {
    auto result = draft18_request_goaway_probes();
    auto d21 = draft21_request_goaway_probes();
    result.insert(result.end(),d21.begin(),d21.end());
    return result;
}
RawProbeTranscript stimulus(const RequestGoawayProbe& p) {
    RawProbeTranscript t;
    t.scenario_id = p.definition.id;
    t.events = {transport::ConnectionEstablishedEvent{},
                transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    t.setup = {{RawProbeChannel::NewUni,p.definition.setup_bytes,false},3,4,false,1};
    t.transport_established = t.peer_setup_received = t.complete = t.stimulus_delivered = true;
    std::uint64_t stream = 1;
    for (std::size_t i = 0; i < p.definition.writes.size(); ++i) {
        const auto& write = p.definition.writes[i];
        const auto id = write.reuse_write_stream ? *t.writes[*write.reuse_write_stream].stream_id : stream;
        if (!write.reuse_write_stream) stream += 4;
        t.writes.push_back({write,id,write.bytes.size(),false,t.events.size()});
        if (i < (p.duplicate ? 1u : 2u))
            t.events.push_back(transport::StreamDataEvent{id,b({7,0,1,0}),false});
    }
    t.delivery_event_count = t.events.size();
    return t;
}
RawProbeTranscript positive(const RequestGoawayProbe& p) {
    auto t = stimulus(p);
    if (p.duplicate)
        t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
    else
        t.events.push_back(transport::StreamDataEvent{*t.writes.back().stream_id,b({7,0,1,0}),false});
    return t;
}
void insert_before(RawProbeTranscript& t, std::size_t index, transport::TransportEvent event) {
    t.events.insert(t.events.begin() + static_cast<std::ptrdiff_t>(index),std::move(event));
    for (auto& write : t.writes)
        if (write.delivery_event_count && *write.delivery_event_count > index) ++*write.delivery_event_count;
    if (t.delivery_event_count && *t.delivery_event_count > index) ++*t.delivery_event_count;
}
}

TEST(RequestGoaway, FactoriesProvideCompleteNamedCohort) {
    const auto d18 = draft18_request_goaway_probes();
    const auto d21 = draft21_request_goaway_probes();
    ASSERT_EQ(d18.size(),1u);
    ASSERT_EQ(d21.size(),2u);
    EXPECT_EQ(d18.front().definition.id,"receive-two-goaways-on-same-request-stream");
    EXPECT_EQ(d18.front().requirement_id,"D18-10-4-MUST-003");
    EXPECT_EQ(d18.front().evaluator_id,"session-closed-protocol-violation");
    EXPECT_EQ(d21[0].definition.id,"d21-duplicate-request-goaway");
    EXPECT_EQ(d21[1].definition.id,"d21-goaway-on-distinct-request-streams");
    for (const auto& p : d21) {
        EXPECT_EQ(p.requirement_id,"D21-9-2-MUST-328");
        EXPECT_EQ(p.evaluator_id,"d21-duplicate-request-goaway-protocol-violation");
    }
    EXPECT_THROW(draft18_request_goaway_probes(std::chrono::milliseconds{0}),std::invalid_argument);
    EXPECT_THROW(draft21_request_goaway_probes(std::chrono::milliseconds{-1}),std::invalid_argument);
}

TEST(RequestGoaway, ActualConfirmedNamespaceStreamsPassTheirOwnObservation) {
    const auto all = profiles();
    ASSERT_EQ(all.size(),3u);
    for (const auto& p : all) {
        SCOPED_TRACE(p.definition.id);
        const auto t = positive(p);
        ASSERT_TRUE(raw_probe_stimulus_valid(t,p.definition));
        EXPECT_EQ(evaluate_request_goaway_probe(t,p),true);
        EXPECT_TRUE(p.definition.response_ready(t));
    }
}

TEST(RequestGoaway, RequestGoawaysDecodeEmptyUriAndMoqTimeoutWithoutRequestId) {
    const auto all = profiles();
    ASSERT_EQ(all.size(),3u);
    for (const auto& p : all) {
        unsigned count = 0;
        for (const auto& write : p.definition.writes) {
            EXPECT_FALSE(write.fin);
            if (!write.reuse_write_stream) continue;
            ++count;
            EXPECT_EQ(write.bytes,b({0x10,0,3,0,0xa7,0x10}));
            moq::interop::wire::Cursor cursor(write.bytes);
            if (p.draft == 18) {
                namespace d18 = moq::interop::wire::draft18;
                const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
                ASSERT_TRUE(std::holds_alternative<d18::Message>(decoded));
                const auto* message = std::get_if<d18::GoawayMessage>(&std::get<d18::Message>(decoded));
                ASSERT_NE(message,nullptr);
                EXPECT_TRUE(message->new_session_uri.empty());
                EXPECT_EQ(message->timeout,10000u);
                EXPECT_FALSE(message->request_id);
            } else {
                namespace d21 = moq::interop::wire::draft21;
                const auto decoded = d21::decode_goaway(cursor,false);
                ASSERT_TRUE(std::holds_alternative<d21::GoawayMessage>(decoded));
                EXPECT_TRUE(std::get<d21::GoawayMessage>(decoded).new_session_uri.empty());
                EXPECT_EQ(std::get<d21::GoawayMessage>(decoded).timeout_ms,10000u);
            }
            EXPECT_EQ(cursor.remaining(),0u);
        }
        EXPECT_EQ(count,2u);
    }
}

TEST(RequestGoaway, DuplicateRequiresTypedApplicationProtocolViolation) {
    const auto all = profiles();
    ASSERT_EQ(all.size(),3u);
    for (const auto& p : all) {
        if (!p.duplicate) continue;
        auto t = positive(p);
        std::get<transport::PeerCloseEvent>(t.events.back()).error_code = 4;
        EXPECT_EQ(evaluate_request_goaway_probe(t,p),false);
        std::get<transport::PeerCloseEvent>(t.events.back()).error_space = transport::CloseErrorSpace::Transport;
        EXPECT_FALSE(evaluate_request_goaway_probe(t,p));
        EXPECT_FALSE(evaluate_request_goaway_probe(stimulus(p),p));
    }
}

TEST(RequestGoaway, MissingUnscopedMalformedOrPrematureAckCannotEstablishRequest) {
    const auto all = profiles();
    ASSERT_EQ(all.size(),3u);
    for (const auto& p : all) {
        const auto t = positive(p);
        for (const auto& bytes : {b({}),b({7,0}),b({5,0,3,0x10,0,0}),b({4,0,2,0,0}),
                                 b({7,0,2,0,1}),b({7,0,3,1,9,0}),b({7,0,1,0,8,0})}) {
            auto changed = t;
            std::get<transport::StreamDataEvent>(changed.events[2]).data = bytes;
            EXPECT_FALSE(evaluate_request_goaway_probe(changed,p));
        }
        auto early = t;
        early.writes.front().delivery_event_count = 3;
        EXPECT_FALSE(evaluate_request_goaway_probe(early,p));
        for (const auto& event : std::vector<transport::TransportEvent>{
             transport::PeerResetEvent{1,1},transport::PeerStopSendingEvent{1,1},
             transport::StreamDataEvent{1,{},true}}) {
            auto cancelled = t;
            insert_before(cancelled,2,event);
            EXPECT_FALSE(evaluate_request_goaway_probe(cancelled,p));
        }
        auto closed_ack = t;
        std::get<transport::StreamDataEvent>(closed_ack.events[2]).fin = true;
        EXPECT_FALSE(evaluate_request_goaway_probe(closed_ack,p));
    }
}

TEST(RequestGoaway, TypedNamespaceNotificationsMayFollowAckButInvalidTailsCannotPass) {
    const auto all = profiles();
    ASSERT_EQ(all.size(),3u);
    for (const auto& p : all) {
        auto t = positive(p);
        auto& bytes = std::get<transport::StreamDataEvent>(t.events[2]).data;
        const auto tail = b({8,0,3,1,1,'x',14,0,3,1,1,'x'});
        bytes.insert(bytes.end(),tail.begin(),tail.end());
        EXPECT_EQ(evaluate_request_goaway_probe(t,p),true);
        bytes.push_back(std::byte{8});
        EXPECT_FALSE(evaluate_request_goaway_probe(t,p));
        bytes = b({7,0,1,0,14,0,3,1,1,'x'});
        EXPECT_FALSE(evaluate_request_goaway_probe(t,p));
    }
}

TEST(RequestGoaway, DistinctFirstGoawaysNeedFreshTypedBarrierResponse) {
    const auto all = draft21_request_goaway_probes();
    ASSERT_EQ(all.size(),2u);
    const auto& p = all[1];
    ASSERT_FALSE(p.duplicate);
    auto t = positive(p);
    EXPECT_EQ(t.writes[0].write.bytes,b({0x50,0,5,1,1,1,'a',0}));
    EXPECT_EQ(t.writes[1].write.bytes,b({0x50,0,5,3,1,1,'b',0}));
    EXPECT_EQ(t.writes[4].write.bytes,b({0x50,0,5,5,1,1,'c',0}));
    ASSERT_EQ(t.writes[2].stream_id,t.writes[0].stream_id);
    ASSERT_EQ(t.writes[3].stream_id,t.writes[1].stream_id);
    EXPECT_NE(t.writes[0].stream_id,t.writes[1].stream_id);
    EXPECT_EQ(evaluate_request_goaway_probe(t,p),true);
    std::get<transport::StreamDataEvent>(t.events.back()).data = b({5,0,3,0x10,0,0});
    std::get<transport::StreamDataEvent>(t.events.back()).fin = true;
    EXPECT_EQ(evaluate_request_goaway_probe(t,p),true);
    for (const auto& response : {b({}),b({7,0}),b({5,0,3,0x10}),b({4,0,2,0,0}),b({7,0,1,0,7,0})}) {
        auto invalid = positive(p);
        std::get<transport::StreamDataEvent>(invalid.events.back()).data = response;
        EXPECT_FALSE(evaluate_request_goaway_probe(invalid,p));
    }
    auto early = positive(p);
    early.writes.back().delivery_event_count = early.events.size();
    early.delivery_event_count = early.events.size();
    EXPECT_FALSE(evaluate_request_goaway_probe(early,p));
    auto wrong_stream = positive(p);
    std::get<transport::StreamDataEvent>(wrong_stream.events.back()).stream_id = 1;
    EXPECT_FALSE(evaluate_request_goaway_probe(wrong_stream,p));
    for (const auto& event : std::vector<transport::TransportEvent>{
         transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}},
         transport::PeerCloseEvent{transport::CloseErrorSpace::Application,4,{}},
         transport::PeerCloseEvent{transport::CloseErrorSpace::Transport,3,{}}}) {
        auto closed = stimulus(p);
        closed.events.push_back(event);
        EXPECT_FALSE(evaluate_request_goaway_probe(closed,p));
    }
    EXPECT_FALSE(evaluate_request_goaway_probe(stimulus(p),p));
}

TEST(RequestGoaway, DistinctControlAllowsMigratedFirstRequestToCloseBeforeSecondGoaway) {
    const auto all = draft21_request_goaway_probes();
    ASSERT_EQ(all.size(),2u);
    const auto& p = all[1];
    for (const auto& event : std::vector<transport::TransportEvent>{
         transport::PeerResetEvent{1,0x12},transport::StreamDataEvent{1,{},true}}) {
        auto t = positive(p);
        const auto index = *t.writes[2].delivery_event_count;
        insert_before(t,index,event);
        ++*t.writes[3].delivery_event_count;
        ++*t.writes[4].delivery_event_count;
        ++*t.delivery_event_count;
        EXPECT_TRUE(p.definition.writes[3].evidence_ready({std::span(t.writes).first(3),std::span(t.events).first(*t.writes[3].delivery_event_count)}));
        EXPECT_EQ(evaluate_request_goaway_probe(t,p),true);
    }
}

TEST(RequestGoaway, IncompleteAcceptanceChangedStimulusAndStreamAliasesRemainNotRun) {
    const auto all = profiles();
    ASSERT_EQ(all.size(),3u);
    for (const auto& p : all) {
        const auto t = positive(p);
        for (auto mutate : std::vector<std::function<void(RawProbeTranscript&)>>{
             [](auto& x){--x.writes.back().accepted;},
             [](auto& x){x.writes.back().delivery_event_count.reset();},
             [](auto& x){x.writes.front().stream_id = 3;},
             [](auto& x){x.writes.front().write.bytes[3] = std::byte{3};},
             [](auto& x){x.writes.back().write.bytes.front() = std::byte{0};},
             [](auto& x){x.writes.back().stream_id = 3;},
             [](auto& x){x.stimulus_delivered = false;},
             [](auto& x){x.timed_out = true;},
             [](auto& x){x.harness_failed = true;}}) {
            auto changed = t;
            mutate(changed);
            EXPECT_FALSE(evaluate_request_goaway_probe(changed,p));
        }
    }
}

TEST(RequestGoaway, DistinctBarrierGateRequiresBothActualFirstGoawayAcceptances) {
    const auto all = draft21_request_goaway_probes();
    ASSERT_EQ(all.size(),2u);
    const auto& p = all[1];
    const auto t = positive(p);
    for (std::size_t i : {0u,1u,2u,3u}) {
        auto changed = t;
        --changed.writes[i].accepted;
        EXPECT_FALSE(p.definition.writes.back().evidence_ready(
            {std::span(changed.writes).first(4),std::span(changed.events).first(*changed.delivery_event_count)}));
        EXPECT_FALSE(evaluate_request_goaway_probe(changed,p));
    }
    auto alias = t;
    alias.writes[1].stream_id = alias.writes[0].stream_id;
    alias.writes[3].stream_id = alias.writes[0].stream_id;
    EXPECT_FALSE(evaluate_request_goaway_probe(alias,p));
    for (std::size_t initial : {0u,1u}) {
        auto missing_ack = t;
        std::get<transport::StreamDataEvent>(missing_ack.events[2 + initial]).data.clear();
        EXPECT_FALSE(p.definition.writes[2].evidence_ready(
            {std::span(missing_ack.writes).first(2),std::span(missing_ack.events).first(*missing_ack.writes[2].delivery_event_count)}));
        EXPECT_FALSE(evaluate_request_goaway_probe(missing_ack,p));
    }
}

TEST(RequestGoaway, StreamEvidenceSupportsFragmentationAndEnforcesBoundsAndTerminalChronology) {
    const auto all = profiles();
    ASSERT_EQ(all.size(),3u);
    for (const auto& p : all) {
        auto fragmented = positive(p);
        std::get<transport::StreamDataEvent>(fragmented.events[2]).data = b({7,0});
        insert_before(fragmented,3,transport::StreamDataEvent{1,b({1,0}),false});
        if (!p.duplicate) ++*fragmented.writes[1].delivery_event_count;
        else {
            ++*fragmented.writes[1].delivery_event_count;
            ++*fragmented.writes[2].delivery_event_count;
            ++*fragmented.delivery_event_count;
        }
        EXPECT_EQ(evaluate_request_goaway_probe(fragmented,p),true);
        auto limit = positive(p);
        std::get<transport::StreamDataEvent>(limit.events[2]).data.resize(65547);
        EXPECT_FALSE(evaluate_request_goaway_probe(limit,p));
        limit = positive(p);
        limit.events.resize(4097,transport::DatagramEvent{});
        EXPECT_FALSE(evaluate_request_goaway_probe(limit,p));
        auto expired = positive(p);
        insert_before(expired,2,transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
        EXPECT_FALSE(evaluate_request_goaway_probe(expired,p));
        auto bad_profile = p;
        bad_profile.duplicate = !bad_profile.duplicate;
        EXPECT_FALSE(evaluate_request_goaway_probe(positive(p),bad_profile));
    }
    const auto& p = all.back();
    auto fragmented_barrier = positive(p);
    std::get<transport::StreamDataEvent>(fragmented_barrier.events.back()).data = b({7,0});
    fragmented_barrier.events.push_back(transport::StreamDataEvent{*fragmented_barrier.writes.back().stream_id,b({1,0}),true});
    EXPECT_EQ(evaluate_request_goaway_probe(fragmented_barrier,p),true);
    for (const auto& event : std::vector<transport::TransportEvent>{
         transport::PeerResetEvent{9,1},transport::PeerStopSendingEvent{9,1},
         transport::StreamDataEvent{9,{},true}}) {
        auto expired = positive(p);
        expired.events.insert(expired.events.end() - 1,event);
        EXPECT_FALSE(evaluate_request_goaway_probe(expired,p));
    }
    auto closed = positive(p);
    closed.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
    EXPECT_FALSE(evaluate_request_goaway_probe(closed,p));
}

namespace {
namespace requirements = moq::interop::requirements;
requirements::RequirementCatalog goaway_catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(
        draft,root / "docs",root / "requirements/draft-digests.json");
    return requirements::RequirementCatalog::load(source,
        root / "requirements" / ("draft" + std::to_string(draft) + ".json"));
}
std::vector<requirements::Outcome> catalog_outcomes(
    const requirements::RequirementCatalog& catalog,
    const std::vector<RawProbeTranscript>& transcripts) {
    if (catalog.draft == 21) return requirements::evaluate_draft21_raw_probes(catalog,transcripts);
    std::vector<requirements::ScenarioContext> contexts;
    for (const auto& transcript : transcripts) {
        requirements::ScenarioContext context;
        context.scenario_id = transcript.scenario_id;
        context.complete = transcript.complete;
        context.stimulus_delivered = transcript.stimulus_delivered;
        context.raw_probe = transcript;
        contexts.push_back(std::move(context));
    }
    return requirements::evaluate_draft18(catalog,contexts);
}
requirements::OutcomeState catalog_state(
    const requirements::RequirementCatalog& catalog,
    const std::vector<RawProbeTranscript>& transcripts, const std::string& requirement_id) {
    const auto outcomes = catalog_outcomes(catalog,transcripts);
    const auto found = std::find_if(outcomes.begin(),outcomes.end(),[&](const auto& outcome) {
        return outcome.requirement_id == requirement_id;
    });
    EXPECT_NE(found,outcomes.end());
    return found == outcomes.end() ? requirements::OutcomeState::NotRun : found->state;
}
RawProbeTranscript wrong_application_close(const RequestGoawayProbe& profile) {
    auto transcript = positive(profile);
    std::get<transport::PeerCloseEvent>(transcript.events.back()).error_code = 4;
    return transcript;
}
}

TEST(RequestGoawayCatalog, Draft18ScoresOnlyItsSingleCompleteUniqueActualContext) {
    const auto catalog = goaway_catalog(18);
    const auto all = draft18_request_goaway_probes();
    ASSERT_EQ(all.size(),1u);
    const auto& p = all.front();
    const auto t = positive(p);
    EXPECT_EQ(catalog_state(catalog,{t},p.requirement_id),requirements::OutcomeState::Pass);
    EXPECT_EQ(catalog_state(catalog,{},p.requirement_id),requirements::OutcomeState::NotRun);
    EXPECT_EQ(catalog_state(catalog,{t,t},p.requirement_id),requirements::OutcomeState::NotRun);
    EXPECT_EQ(catalog_state(catalog,{stimulus(p)},p.requirement_id),requirements::OutcomeState::NotRun);
    const auto baseline = catalog_outcomes(catalog,{});
    const auto outcomes = catalog_outcomes(catalog,{t});
    ASSERT_EQ(outcomes.size(),baseline.size());
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        ASSERT_EQ(outcomes[i].requirement_id,baseline[i].requirement_id);
        if (outcomes[i].requirement_id != p.requirement_id) {
            EXPECT_EQ(outcomes[i].state,baseline[i].state) << outcomes[i].requirement_id;
        }
    }
    auto invalid_ack = t;
    std::get<transport::StreamDataEvent>(invalid_ack.events[2]).data = b({7,0});
    EXPECT_EQ(catalog_state(catalog,{invalid_ack},p.requirement_id),requirements::OutcomeState::NotRun);
    auto wrong_scenario = t;
    wrong_scenario.scenario_id = "d21-duplicate-request-goaway";
    EXPECT_EQ(catalog_state(catalog,{wrong_scenario},p.requirement_id),requirements::OutcomeState::NotRun);
    auto incomplete = t;
    incomplete.complete = false;
    EXPECT_EQ(catalog_state(catalog,{incomplete},p.requirement_id),requirements::OutcomeState::NotRun);
    auto undelivered = t;
    undelivered.stimulus_delivered = false;
    EXPECT_EQ(catalog_state(catalog,{undelivered},p.requirement_id),requirements::OutcomeState::NotRun);
    const auto wrong = wrong_application_close(p);
    EXPECT_EQ(catalog_state(catalog,{wrong},p.requirement_id),requirements::OutcomeState::Fail);
    EXPECT_EQ(catalog_state(catalog,{t,wrong},p.requirement_id),requirements::OutcomeState::Fail);
    EXPECT_EQ(catalog_state(catalog,{wrong,wrong},p.requirement_id),requirements::OutcomeState::Fail);
}

TEST(RequestGoawayCatalog, Draft21NeedsDuplicateAndDistinctPositiveContextsExactlyOnce) {
    const auto catalog = goaway_catalog(21);
    const auto all = draft21_request_goaway_probes();
    ASSERT_EQ(all.size(),2u);
    const auto& duplicate = all[0];
    const auto& distinct = all[1];
    const auto duplicate_pass = positive(duplicate);
    const auto distinct_pass = positive(distinct);
    const auto& id = duplicate.requirement_id;
    EXPECT_EQ(catalog_state(catalog,{duplicate_pass,distinct_pass},id),requirements::OutcomeState::Pass);
    EXPECT_EQ(catalog_state(catalog,{},id),requirements::OutcomeState::NotRun);
    EXPECT_EQ(catalog_state(catalog,{duplicate_pass},id),requirements::OutcomeState::NotRun);
    EXPECT_EQ(catalog_state(catalog,{distinct_pass},id),requirements::OutcomeState::NotRun);
    EXPECT_EQ(catalog_state(catalog,{duplicate_pass,duplicate_pass,distinct_pass},id),requirements::OutcomeState::NotRun);
    EXPECT_EQ(catalog_state(catalog,{duplicate_pass,distinct_pass,distinct_pass},id),requirements::OutcomeState::NotRun);
    EXPECT_EQ(catalog_state(catalog,{duplicate_pass,stimulus(distinct)},id),requirements::OutcomeState::NotRun);
    for (const auto& response : {b({}),b({7,0}),b({5,0,3,0x10}),b({7,0,1,0,8,0})}) {
        auto invalid = distinct_pass;
        std::get<transport::StreamDataEvent>(invalid.events.back()).data = response;
        EXPECT_EQ(catalog_state(catalog,{duplicate_pass,invalid},id),requirements::OutcomeState::NotRun);
    }
    auto early = distinct_pass;
    early.writes.back().delivery_event_count = early.events.size();
    early.delivery_event_count = early.events.size();
    EXPECT_EQ(catalog_state(catalog,{duplicate_pass,early},id),requirements::OutcomeState::NotRun);
    auto alias = distinct_pass;
    alias.writes.back().stream_id = alias.writes.front().stream_id;
    EXPECT_EQ(catalog_state(catalog,{duplicate_pass,alias},id),requirements::OutcomeState::NotRun);
    for (const auto& close : std::vector<transport::PeerCloseEvent>{
         {transport::CloseErrorSpace::Application,3,{}},
         {transport::CloseErrorSpace::Application,4,{}},
         {transport::CloseErrorSpace::Transport,3,{}}}) {
        auto closed = stimulus(distinct);
        closed.events.push_back(close);
        EXPECT_EQ(catalog_state(catalog,{closed},id),requirements::OutcomeState::NotRun);
        EXPECT_EQ(catalog_state(catalog,{duplicate_pass,closed},id),requirements::OutcomeState::NotRun);
    }
}

TEST(RequestGoawayCatalog, DuplicateWrongApplicationCloseDominatesMissingSuccessfulAndDuplicatedControl) {
    const auto catalog = goaway_catalog(21);
    const auto all = draft21_request_goaway_probes();
    ASSERT_EQ(all.size(),2u);
    const auto wrong = wrong_application_close(all[0]);
    const auto duplicate_pass = positive(all[0]);
    const auto distinct_pass = positive(all[1]);
    const auto& id = all[0].requirement_id;
    for (const auto& transcripts : std::vector<std::vector<RawProbeTranscript>>{
         {wrong}, {wrong,stimulus(all[1])}, {wrong,distinct_pass},
         {wrong,distinct_pass,distinct_pass}, {duplicate_pass,wrong,distinct_pass},
         {distinct_pass,duplicate_pass,wrong}, {wrong,wrong}})
        EXPECT_EQ(catalog_state(catalog,transcripts,id),requirements::OutcomeState::Fail);
}

TEST(RequestGoawayCatalog, BindingsRequireTransportEvidenceAndOnlyDuplicateContextsNeedPeerClose) {
    for (const auto& p : profiles()) {
        const auto catalog = goaway_catalog(p.draft);
        const auto row = std::find_if(catalog.requirements.begin(),catalog.requirements.end(),[&](const auto& requirement) {
            return requirement.id == p.requirement_id;
        });
        ASSERT_NE(row,catalog.requirements.end());
        EXPECT_NE(std::find(row->scenarios.begin(),row->scenarios.end(),p.definition.id),row->scenarios.end());
        EXPECT_NE(std::find(row->evaluators.begin(),row->evaluators.end(),p.evaluator_id),row->evaluators.end());
        const auto bindings = p.draft == 18 ? requirements::draft18_executable_bindings()
                                           : requirements::draft21_executable_bindings();
        const auto binding = std::find_if(bindings.begin(),bindings.end(),[&](const auto& candidate) {
            return candidate.requirement_id == p.requirement_id &&
                candidate.scenario_id == p.definition.id && candidate.evaluator_id == p.evaluator_id;
        });
        ASSERT_NE(binding,bindings.end());
        const auto contains = [&](const std::string& kind) {
            return std::find(binding->evidence_kinds.begin(),binding->evidence_kinds.end(),kind) != binding->evidence_kinds.end();
        };
        EXPECT_TRUE(contains("raw_probe_stimulus"));
        EXPECT_TRUE(contains("raw_probe_transport_event"));
        EXPECT_EQ(contains("peer_close"),p.duplicate);
    }
}
