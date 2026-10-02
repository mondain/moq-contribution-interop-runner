#include "moq/interop/scenarios/fetch_group_order.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
std::vector<FetchGroupOrderProbe> all() {
    auto profiles = draft18_fetch_group_order_probes();
    auto d21 = draft21_fetch_group_order_probes();
    profiles.insert(profiles.end(), d21.begin(), d21.end());
    return profiles;
}
RawProbeTranscript stimulus(const FetchGroupOrderProbe& p) {
    RawProbeTranscript t;
    t.scenario_id = p.definition.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.events = {transport::ConnectionEstablishedEvent{},
                transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    for (std::size_t i = 0; i < p.definition.writes.size(); ++i) {
        const auto& w = p.definition.writes[i];
        t.writes.push_back({w, 1 + 4 * i, w.bytes.size(), true, 2});
    }
    t.complete = t.stimulus_delivered = t.transport_established = t.peer_setup_received = true;
    t.delivery_event_count = 2;
    return t;
}
bool descending(const FetchGroupOrderProbe& p, std::size_t index) {
    return p.order == FetchGroupOrderRequest::Descending ||
           (p.order == FetchGroupOrderRequest::BothExplicit && index == 1);
}
Bytes objects(unsigned id, bool desc) {
    // Absolute first Group/Object, explicit priority. A zero Group delta
    // moves one group in the request's order, and resets Object ID to zero.
    return b({5, id, 0x1c, desc ? 9u : 7u, 0, 99, 1, 42,
              0x0c, 0, 0, 1, 43});
}
void ack(RawProbeTranscript& t, std::size_t index) {
    t.events.push_back(transport::StreamDataEvent{1 + 4 * index,
        b({0x18, 0, 4, 0, 9, 9, 0}), true});
}
void complete_pair(RawProbeTranscript& t, const FetchGroupOrderProbe& p,
                   std::size_t index, bool ack_first = true) {
    if (ack_first) ack(t, index);
    t.events.push_back(transport::StreamDataEvent{6 + 4 * index,
        objects(1 + 2 * index, descending(p, index)), true});
    if (!ack_first) ack(t, index);
}
RawProbeTranscript complete(const FetchGroupOrderProbe& p) {
    auto t = stimulus(p);
    for (std::size_t i = 0; i < p.definition.writes.size(); ++i) complete_pair(t, p, i);
    return t;
}
TEST(FetchGroupOrder, ActualTwoGroupsPassAfterFinAndAckInEitherArrivalOrder) {
    for (const auto& p : all()) {
        for (const auto ack_first : {false, true}) {
            auto t = stimulus(p);
            for (std::size_t i = 0; i < p.definition.writes.size(); ++i)
                complete_pair(t, p, i, ack_first);
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), true) << p.definition.id;
            t.complete = false;
            EXPECT_TRUE(p.definition.response_ready(t));
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
        }
    }
}
TEST(FetchGroupOrder, Draft18RequiresBothExplicitRequestsAndAssociatedStreams) {
    const auto p = draft18_fetch_group_order_probes().front();
    ASSERT_EQ(p.definition.writes.size(), 2u);
    auto t = stimulus(p);
    complete_pair(t, p, 0);
    EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
    EXPECT_FALSE(p.definition.response_ready(t));
    complete_pair(t, p, 1);
    EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), true);
    t.writes[1].stream_id = t.writes[0].stream_id;
    EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
}
TEST(FetchGroupOrder, SingleGroupRangesAndHeaderDoNotProveOrdering) {
    for (const auto& p : all()) {
        for (const auto& body : {b({5, 1}), b({5, 1, 0x1c, 7, 0, 99, 1, 42}),
                                b({5, 1, 0x81, 0x0c, 9, 9})}) {
            auto t = stimulus(p);
            ack(t, 0);
            t.events.push_back(transport::StreamDataEvent{6, body, true});
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
        }
    }
}
TEST(FetchGroupOrder, CompleteOrdinaryGroupsAcrossAbsoluteRangeExposeReversal) {
    for (const auto& p : all()) {
        auto t = complete(p);
        const bool desc = descending(p, 0);
        // Range endpoints are absolute and replace the delta baseline. A
        // subsequent complete ordinary object therefore can expose reversal.
        const auto reversed = b({5, 1, 0x1c, desc ? 7u : 9u, 0, 99, 1, 42,
                                  0x81, 0x0c, desc ? 9u : 7u, 0,
                                  0x04, 1, 1, 43});
        for (auto& event : t.events)
            if (auto* data = std::get_if<transport::StreamDataEvent>(&event);
                data && data->stream_id == 6) data->data = reversed;
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), false);
        EXPECT_TRUE(p.definition.response_ready(t));
        for (auto& event : t.events)
            if (auto* data = std::get_if<transport::StreamDataEvent>(&event);
                data && data->stream_id == 6) data->fin = false;
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), false);
        t.events.push_back(transport::StreamDataEvent{6, b({0x80}), false});
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), false);
        t.events.push_back(transport::PeerResetEvent{6, 0});
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), false);
    }
}
TEST(FetchGroupOrder, PartialMalformedOrResetBeforeProofAreNotRun) {
    for (const auto& p : all()) {
        for (unsigned kind = 0; kind < 6; ++kind) {
            auto t = complete(p);
            for (auto& event : t.events) {
                auto* data = std::get_if<transport::StreamDataEvent>(&event);
                if (!data || data->stream_id != 6) continue;
                if (kind == 0) data->fin = false;
                if (kind == 1) data->data.pop_back();
                if (kind == 2) data->data.push_back(std::byte{0x80});
                if (kind == 3) data->data[1] = std::byte{11};
                if (kind == 4) data->stream_id = 4;
                if (kind == 5) data->fin = false;
            }
            if (kind == 5) t.events.push_back(transport::PeerResetEvent{6, 0});
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt) << kind;
        }
    }
}
TEST(FetchGroupOrder, FinCannotHideMalformedTailAndMissingAck) {
    for (const auto& p : all()) {
        auto t = stimulus(p);
        t.events.push_back(transport::StreamDataEvent{6, objects(1, descending(p, 0)), true});
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
        ack(t, 0);
        if (p.draft == 18) complete_pair(t, p, 1);
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), true);
        t.events.push_back(transport::StreamDataEvent{6, b({0}), false});
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
    }
}
TEST(FetchGroupOrder, DuplicateAssociatedStreamsAndPredeliveryBytesAreNotRun) {
    for (const auto& p : all()) {
        auto t = complete(p);
        t.events.push_back(transport::StreamDataEvent{18, objects(1, descending(p, 0)), true});
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
        t = complete(p);
        t.events.insert(t.events.begin() + 1,
            transport::StreamDataEvent{6, b({5, 1}), false});
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
    }
}
TEST(FetchGroupOrder, ConfiguredFixtureRegeneratedFromActualCanonicalBytes) {
    const std::vector<Bytes> ns{b({'i', 'n', 't', 'e', 'r', 'o', 'p'}), b({'p'})};
    const auto name = b({'v', 'i', 'd', 'e', 'o'});
    for (const auto draft : {18u, 21u}) {
        const auto profiles = draft == 18 ? draft18_fetch_group_order_probes(std::chrono::milliseconds{1000}, ns, name)
                                           : draft21_fetch_group_order_probes(std::chrono::milliseconds{1000}, ns, name);
        for (const auto& p : profiles) {
            auto t = complete(p);
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), true);
            t.writes.front().write.bytes.back() ^= std::byte{1};
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
        }
    }
}
TEST(FetchGroupOrder, FragmentedActualObjectsAndAckUseEachRequestsOwnMarker) {
    for (const auto& p : all()) {
        auto t = stimulus(p);
        for (std::size_t i = 0; i < p.definition.writes.size(); ++i) {
            if (i) {
                t.writes[i].delivery_event_count = t.events.size();
                t.delivery_event_count = t.events.size();
            }
            const auto payload = objects(1 + 2 * i, descending(p, i));
            for (std::size_t j = 0; j < payload.size(); ++j)
                t.events.push_back(transport::StreamDataEvent{6 + 4 * i,
                    Bytes{payload[j]}, j + 1 == payload.size()});
            for (const auto byte : b({0x18, 0, 4, 0, 9, 9, 0}))
                t.events.push_back(transport::StreamDataEvent{1 + 4 * i, Bytes{byte}, false});
        }
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), true);
    }
}
TEST(FetchGroupOrder, RangesDoNotCountAsGroupsOrOverrideObjectOrderingKey) {
    for (const auto& p : all()) {
        auto t = complete(p);
        const auto first_group = descending(p, 0) ? 9u : 7u;
        const auto stream = b({5, 1, 0x1f, first_group, 9, 0, 3, 1, 42,
                               0x81, 0x0c, first_group, 1,
                               0x0f, 0, 0, 0, 1, 43});
        for (auto& event : t.events)
            if (auto* data = std::get_if<transport::StreamDataEvent>(&event);
                data && data->stream_id == 6) data->data = stream;
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), true);
    }
}
TEST(FetchGroupOrder, AckBoundsAndMalformedPropertyScopeCannotAnchorResults) {
    for (const auto& p : all()) {
        for (const auto& reply : {b({0x18, 0, 4, 0, 6, 0, 0}),
                                  b({0x18, 0, 4, 0, 10, 0, 0}),
                                  b({0x18, 0, 3, 0, 9, 9}),
                                  b({0x18, 0, 4, 2, 9, 9, 0})}) {
            auto t = complete(p);
            for (auto& event : t.events)
                if (auto* data = std::get_if<transport::StreamDataEvent>(&event);
                    data && data->stream_id == 1) data->data = reply;
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
        }
    }
}
TEST(FetchGroupOrder, Draft18ExclusiveEndAndWholeGroupEndDifferFromDraft21InclusiveEnd) {
    for (const auto& p : all()) {
        if (p.order == FetchGroupOrderRequest::Descending) continue;
        for (const auto end_object : {0u, 1u}) {
            auto t = complete(p);
            for (auto& event : t.events) {
                auto* data = std::get_if<transport::StreamDataEvent>(&event);
                if (!data) continue;
                if (data->stream_id == 1) data->data = b({0x18, 0, 4, 0, 8, end_object, 0});
                if (data->stream_id == 6) data->data[10] = std::byte{1};
            }
            const bool in_range = p.draft == 18 ? end_object == 0 : end_object == 1;
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p),
                      in_range ? std::optional{true} : std::nullopt);
        }
    }
}
TEST(FetchGroupOrder, TerminalEventsBoundChronologicalProof) {
    for (const auto& p : all()) {
        auto t = complete(p);
        const transport::PeerCloseEvent close{transport::CloseErrorSpace::Application, 0, {}};
        t.events.push_back(close);
        t.events.push_back(transport::StreamDataEvent{6, Bytes(65547), false});
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), true);
        t = stimulus(p);
        t.events.push_back(close);
        for (std::size_t i = 0; i < p.definition.writes.size(); ++i) complete_pair(t, p, i);
        EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt);
    }
}
TEST(FetchGroupOrder, AcceptedStimulusAndBoundedMetadataAreRequired) {
    for (const auto& p : all()) {
        for (unsigned mode = 0; mode < 14; ++mode) {
            auto t = complete(p);
            if (mode == 0) t.writes[0].accepted--;
            if (mode == 1) t.writes[0].fin_accepted = false;
            if (mode == 2) t.writes[0].write.fin = false;
            if (mode == 3) t.writes[0].delivery_event_count.reset();
            if (mode == 4) t.delivery_event_count.reset();
            if (mode == 5) t.writes[0].delivery_event_count = 3;
            if (mode == 6) t.scenario_id = "claimed";
            if (mode == 7) t.harness_failed = true;
            if (mode == 8) t.timed_out = true;
            if (mode == 9) t.events.resize(4097);
            if (mode == 10) t.events.push_back(transport::StreamDataEvent{18, Bytes(65547), false});
            if (mode == 11) t.writes[0].write.bytes.push_back(std::byte{0});
            if (mode == 12) t.setup.accepted--;
            if (mode == 13)
                for (unsigned i = 0; i < 65; ++i)
                    t.events.push_back(transport::PeerResetEvent{18 + 4 * i, 0});
            EXPECT_EQ(evaluate_fetch_group_order_probe(t, p), std::nullopt) << mode;
        }
    }
    EXPECT_THROW(draft18_fetch_group_order_probes(std::chrono::milliseconds{0}), std::invalid_argument);
    EXPECT_THROW(draft21_fetch_group_order_probes(std::chrono::milliseconds{10}, {b({'.'})}), std::invalid_argument);
}
TEST(FetchGroupOrderCatalog, RequiredActualContextsMustAllBePresentAndUnique) {
    using namespace requirements;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const std::vector<Bytes> ns{b({'i', 'n', 't', 'e', 'r', 'o', 'p'})};
    for (const auto draft : {18u, 21u}) {
        const auto source = load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        const auto catalog = RequirementCatalog::load(source,
            root / "requirements" / ("draft" + std::to_string(draft) + ".json"));
        const auto profiles = draft == 18
            ? draft18_fetch_group_order_probes(std::chrono::milliseconds{1000}, ns, b({'v'}))
            : draft21_fetch_group_order_probes(std::chrono::milliseconds{1000}, ns, b({'v'}));
        const auto bindings = draft == 18 ? draft18_executable_bindings() : draft21_executable_bindings();
        for (const auto& p : profiles)
            EXPECT_TRUE(std::ranges::any_of(bindings, [&](const auto& binding) {
                return binding.requirement_id == p.requirement_id &&
                       binding.scenario_id == p.definition.id && binding.evaluator_id == p.evaluator_id;
            }));
        const auto state = [&](std::vector<RawProbeTranscript> transcripts) {
            std::vector<Outcome> outcomes;
            if (draft == 21) outcomes = evaluate_draft21_raw_probes(catalog, transcripts);
            else {
                std::vector<ScenarioContext> contexts;
                for (auto& transcript : transcripts) {
                    ScenarioContext c;
                    c.scenario_id = transcript.scenario_id;
                    c.complete = transcript.complete;
                    c.stimulus_delivered = transcript.stimulus_delivered;
                    c.raw_probe = std::move(transcript);
                    contexts.push_back(std::move(c));
                }
                outcomes = evaluate_draft18(catalog, contexts);
            }
            const auto found = std::ranges::find_if(outcomes, [&](const auto& outcome) {
                return outcome.requirement_id == profiles.front().requirement_id;
            });
            EXPECT_NE(found, outcomes.end());
            return found == outcomes.end() ? OutcomeState::NotRun : found->state;
        };
        std::vector<RawProbeTranscript> transcripts;
        for (const auto& p : profiles) transcripts.push_back(complete(p));
        EXPECT_EQ(state(transcripts), OutcomeState::Pass);
        EXPECT_EQ(state({}), OutcomeState::NotRun);
        auto duplicate = transcripts;
        duplicate.push_back(transcripts.front());
        EXPECT_EQ(state(duplicate), OutcomeState::NotRun);
        if (draft == 21)
            for (std::size_t i = 0; i < profiles.size(); ++i) {
                auto missing = transcripts;
                missing.erase(missing.begin() + i);
                EXPECT_EQ(state(missing), OutcomeState::NotRun);
            }
        auto missing_ack = transcripts;
        auto& events = missing_ack.front().events;
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event) {
            const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            return data && data->stream_id == 1;
        }), events.end());
        EXPECT_EQ(state(missing_ack), OutcomeState::NotRun);
    }
}
} // namespace
} // namespace moq::interop::scenarios
