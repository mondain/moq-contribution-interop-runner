#include "moq/interop/wire/draft21/successful_response.h"
#include "moq/interop/scenarios/fetch_probe.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>

namespace moq::interop::scenarios {
namespace {
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
TEST(FetchWire, Draft21FetchOkHasTypedEndLocationAndScopedProperties) {
    const auto frame = bytes({0x18,0,7,1,3,4,0,0x0e,0x80,255});
    wire::Cursor cursor(frame);
    const auto decoded = wire::draft21::decode_successful_response(cursor,wire::draft21::ResponseContext::Fetch);
    ASSERT_TRUE(std::holds_alternative<wire::draft21::SuccessfulResponse>(decoded));
    const auto& ok = std::get<wire::draft21::SuccessfulResponse>(decoded);
    EXPECT_EQ(ok.end_of_track, 1U);
    ASSERT_TRUE(ok.end_location);
    EXPECT_EQ(ok.end_location->group,3U);
    EXPECT_EQ(ok.end_location->object,4U);
    EXPECT_FALSE(ok.track_alias);
    EXPECT_EQ(ok.track_properties.size(),1U);
    EXPECT_EQ(cursor.remaining(),0U);
}
TEST(FetchWire, Draft21FetchOkRejectsWrongTypeScopesAndInvalidProperties) {
    for (const auto& frame : {bytes({7,0,1,0}), bytes({0x18,0,4,2,0,0,0}),
        bytes({0x18,0,6,0,0,0,1,8,0}), bytes({0x18,0,7,0,0,0,1,9,0,0}),
        bytes({0x18,0,6,0,0,0,0,0x30,2}), bytes({0x18,0,6,0,0,0,0,0x3c,0})}) {
        wire::Cursor cursor(frame);
        EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(
            wire::draft21::decode_successful_response(cursor,wire::draft21::ResponseContext::Fetch)));
        EXPECT_EQ(cursor.offset(),0U);
    }
}
TEST(FetchWire, Draft21FetchOkWaitsForCompleteFrameTransactionally) {
    const auto frame = bytes({0x18,0,4,0,0,0,0});
    for (std::size_t length=0;length<frame.size();++length) {
        wire::Cursor cursor{std::span(frame).first(length)};
        EXPECT_TRUE(std::holds_alternative<wire::NeedMore>(
            wire::draft21::decode_successful_response(cursor,wire::draft21::ResponseContext::Fetch)));
        EXPECT_EQ(cursor.offset(),0U);
    }
}

std::vector<std::byte> literal_fetch(unsigned draft) {
    if (draft == 18) return bytes({0x16,0,18,1,1,0,1,'x',0,0,
        255,255,255,255,255,255,255,255,255,0,0});
    return bytes({0x16,0,18,1,0,1,'x',1,0x21,11,0,0,
        255,255,255,255,255,255,255,255,255});
}
RawProbeTranscript fetch_stimulus(const FetchProbe& p) {
    RawProbeTranscript t;
    t.scenario_id=p.definition.id;
    t.setup={{RawProbeChannel::NewUni,bytes({0xaf,0,0,0}),false},3,4,false,1};
    t.events={transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2,bytes({0xaf,0,0,0}),false},
        transport::StreamDataEvent{1,bytes({0x18,0,4,0,0,1,0}),false},
        transport::StreamDataEvent{6,bytes({5,1}),false}};
    const bool cancellation=p.expectation!=FetchProbeExpectation::FailedUpdateDataReset;
    t.writes.push_back({{RawProbeChannel::NewBidi,literal_fetch(p.draft),cancellation},1,21,cancellation,2});
    auto final=p.definition.writes[1];
    t.writes.push_back({final,1,final.bytes.size(),final.fin,4,final.operation==RawProbeOperation::StopSending});
    t.complete=t.stimulus_delivered=t.transport_established=t.peer_setup_received=true;
    t.delivery_event_count=4;
    return t;
}
void successful_cleanup(RawProbeTranscript& t,const FetchProbe& p,bool reversed=false) {
    if (p.expectation==FetchProbeExpectation::FailedUpdateDataReset) {
        if (reversed) t.events.push_back(transport::PeerResetEvent{6,1});
        t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x17,0,0}),true});
        if (!reversed) t.events.push_back(transport::PeerResetEvent{6,99});
    } else {
        t.events.push_back(transport::PeerResetEvent{1,99});
        t.events.push_back(transport::PeerResetEvent{6,77});
    }
}
std::vector<FetchProbe> all_fetch_profiles() {
    auto result=draft18_fetch_probes();auto d21=draft21_fetch_probes();
    result.insert(result.end(),d21.begin(),d21.end());return result;
}
TEST(FetchProfiles, DefinesSixRequiredFamiliesWithIndependentLiteralStimuli) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    const char* requirements[]={"D18-5-2-MUST-003","D18-5-2-MUST-004","D18-10-9-1-MUST-002",
        "D21-3-2-1-MUST-055","D21-3-2-1-MUST-056","D21-9-5-1-MUST-347"};
    const char* evaluators[]={"fetch-request-stream-reset","fetch-data-stream-reset","fetch-data-stream-reset",
        "d21-fetch-cancel-resets-bidi-request-stream","d21-fetch-cancel-resets-unidirectional-data-stream",
        "d21-failed-fetch-update-resets-data-stream"};
    for (std::size_t i=0;i<profiles.size();++i) {
        const auto& p=profiles[i];SCOPED_TRACE(p.requirement_id);
        EXPECT_EQ(p.requirement_id,requirements[i]);EXPECT_EQ(p.evaluator_id,evaluators[i]);
        EXPECT_EQ(p.definition.setup_bytes,bytes({0xaf,0,0,0}));
        ASSERT_EQ(p.definition.writes.size(),2u);
        EXPECT_EQ(p.definition.writes[0].bytes,literal_fetch(p.draft));
        EXPECT_EQ(p.definition.writes[0].fin,p.expectation!=FetchProbeExpectation::FailedUpdateDataReset);
        const auto& final=p.definition.writes[1];ASSERT_TRUE(final.evidence_ready);
        EXPECT_EQ(final.reuse_write_stream,0u);
        if (p.expectation==FetchProbeExpectation::FailedUpdateDataReset) {
            EXPECT_EQ(final.bytes,bytes({2,0,6,3,1,3,2,2,0}));EXPECT_TRUE(final.fin);
            EXPECT_EQ(final.operation,RawProbeOperation::Write);
        } else {
            EXPECT_TRUE(final.bytes.empty());EXPECT_FALSE(final.fin);
            EXPECT_EQ(final.operation,RawProbeOperation::StopSending);EXPECT_EQ(final.application_error,1u);
        }
    }
}
TEST(FetchProfiles, ActualOpenResponseAndHeaderGateCompleteAndFragmentedEvidence) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for (const auto& p:profiles) {
        auto t=fetch_stimulus(p);const auto& gate=p.definition.writes[1].evidence_ready;
        ASSERT_TRUE(gate);EXPECT_TRUE(gate({std::span(t.writes).first(1),t.events}));
        auto fragmented=t.events;fragmented.pop_back();fragmented.pop_back();
        fragmented.push_back(transport::StreamDataEvent{1,bytes({0x18,0,4,0}),false});
        EXPECT_FALSE(gate({std::span(t.writes).first(1),fragmented}));
        fragmented.push_back(transport::StreamDataEvent{1,bytes({0,1,0}),false});
        fragmented.push_back(transport::StreamDataEvent{6,bytes({5}),false});
        EXPECT_FALSE(gate({std::span(t.writes).first(1),fragmented}));
        fragmented.push_back(transport::StreamDataEvent{6,bytes({1}),false});
        EXPECT_TRUE(gate({std::span(t.writes).first(1),fragmented}));
        for (unsigned variant=0;variant<9;++variant) {
            auto bad=t.events;
            if(variant==0) std::get<transport::StreamDataEvent>(bad[2]).data=bytes({7,0,1,0});
            if(variant==1) std::get<transport::StreamDataEvent>(bad[2]).data=bytes({0x18,0,4,2,0,1,0});
            if(variant==2) std::get<transport::StreamDataEvent>(bad[2]).stream_id=5;
            if(variant==3) std::get<transport::StreamDataEvent>(bad[3]).data=bytes({5,3});
            if(variant==4) std::get<transport::StreamDataEvent>(bad[3]).stream_id=7;
            if(variant==5) std::get<transport::StreamDataEvent>(bad[3]).fin=true;
            if(variant==6) bad.push_back(transport::PeerResetEvent{1,1});
            if(variant==7) bad.push_back(transport::StreamDataEvent{10,bytes({5,1}),false});
            if(variant==8) bad.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
            EXPECT_FALSE(gate({std::span(t.writes).first(1),bad}))<<variant;
        }
    }
}
TEST(FetchObservers, ActualAssociatedResetsPassWithoutInventedErrorCodeOrCrossStreamOrder) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles)for(bool reversed:{false,true}) {
        auto t=fetch_stimulus(p);successful_cleanup(t,p,reversed);
        EXPECT_EQ(evaluate_fetch_probe(t,p),true)<<p.requirement_id;
        EXPECT_TRUE(p.definition.response_ready(t));
    }
}
TEST(FetchProfiles, CancellationAcceptsLocalFinBeforeStopButUpdateRequiresOpenWriteDirection) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles) {
        auto t=fetch_stimulus(p);
        const bool cancellation=p.expectation!=FetchProbeExpectation::FailedUpdateDataReset;
        EXPECT_EQ(t.writes.front().write.fin,cancellation);
        EXPECT_EQ(t.writes.front().fin_accepted,cancellation);
        const auto& gate=p.definition.writes[1].evidence_ready;
        EXPECT_TRUE(gate({std::span(t.writes).first(1),t.events}));
        successful_cleanup(t,p);EXPECT_EQ(evaluate_fetch_probe(t,p),true);
        auto wrong=t;wrong.writes.front().write.fin=!cancellation;
        wrong.writes.front().fin_accepted=!cancellation;
        EXPECT_FALSE(gate({std::span(wrong.writes).first(1),wrong.events}));
        EXPECT_FALSE(evaluate_fetch_probe(wrong,p).has_value());
        wrong=t;wrong.writes.front().fin_accepted=!cancellation;
        EXPECT_FALSE(gate({std::span(wrong.writes).first(1),wrong.events}));
        EXPECT_FALSE(evaluate_fetch_probe(wrong,p).has_value());
    }
}
TEST(FetchObservers, FinCannotEstablishMissingResetOrReplaceAnObservedReset) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles) {
        auto fin=fetch_stimulus(p);successful_cleanup(fin,p);
        const auto target=p.expectation==FetchProbeExpectation::CancelRequestReset?1u:6u;
        for(auto& e:fin.events)if(auto* reset=std::get_if<transport::PeerResetEvent>(&e);reset&&reset->stream_id==target)
            e=transport::StreamDataEvent{target,{},true};
        EXPECT_EQ(evaluate_fetch_probe(fin,p),std::nullopt)<<p.requirement_id;
        for(unsigned variant=0;variant<8;++variant) {
            auto t=fetch_stimulus(p);successful_cleanup(t,p);
            if(variant==0)t.writes[1].operation_accepted=!t.writes[1].operation_accepted;
            if(variant==1)t.writes[1].stream_id=5;
            if(variant==2)t.writes[1].delivery_event_count=3;
            if(variant==3)t.events.insert(t.events.begin()+3,transport::PeerResetEvent{6,1});
            if(variant==4)t.events[3]=transport::StreamDataEvent{6,bytes({5,3}),false};
            if(variant==5)for(auto& e:t.events)if(auto* reset=std::get_if<transport::PeerResetEvent>(&e);reset&&reset->stream_id==target)reset->stream_id=10;
            if(variant==6)t.events.insert(t.events.begin()+4,transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
            if(variant==7)t.events[2]=transport::StreamDataEvent{1,bytes({0x18,0,6,0,0,1,0,0x30,2}),false};
            EXPECT_NE(evaluate_fetch_probe(t,p),std::optional<bool>{true})<<p.requirement_id<<" variant "<<variant;
        }
    }
}
TEST(FetchObservers, FailedUpdateRequiresActualTypedErrorAndRejectsMalformedOrPartialRejections) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles)if(p.expectation==FetchProbeExpectation::FailedUpdateDataReset) {
        for(const auto& error:{bytes({5,0,3,1,0,0}),bytes({5,0,3,0x17,0,0}),
            bytes({5,0,6,1,0,3,0xe2,0x82,0xac})}) {
            auto t=fetch_stimulus(p);t.events.push_back(transport::PeerResetEvent{6,7});
            t.events.push_back(transport::StreamDataEvent{1,error,true});
            EXPECT_EQ(evaluate_fetch_probe(t,p),true);
        }
        for(const auto& invalid:{bytes({7,0,1,0}),bytes({5,0,4,1,0,1,0x80}),
            bytes({5,0,4,1,0,1}),bytes({5,0,3,0x34,0,0})}) {
            auto t=fetch_stimulus(p);t.events.push_back(transport::PeerResetEvent{6,7});
            t.events.push_back(transport::StreamDataEvent{1,invalid,true});
            EXPECT_NE(evaluate_fetch_probe(t,p),std::optional<bool>{true});
        }
    }
}
TEST(FetchProfiles, EarlierResetAndIncompleteDuplicateHeaderCannotEstablishAnOpenStream) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles) {
        auto t=fetch_stimulus(p);const auto& gate=p.definition.writes[1].evidence_ready;
        t.events.insert(t.events.begin()+3,transport::PeerResetEvent{6,1});
        EXPECT_FALSE(gate({std::span(t.writes).first(1),t.events}));
        t=fetch_stimulus(p);
        t.events.push_back(transport::StreamDataEvent{10,bytes({5}),false});
        EXPECT_FALSE(gate({std::span(t.writes).first(1),t.events}));
        t=fetch_stimulus(p);t.writes.front().delivery_event_count=4;
        EXPECT_FALSE(gate({std::span(t.writes).first(1),t.events}));
        t=fetch_stimulus(p);std::swap(t.events[2],t.events[3]);
        t.writes.front().delivery_event_count=3;
        EXPECT_FALSE(gate({std::span(t.writes).first(1),t.events}));
    }
    EXPECT_NO_THROW(draft18_fetch_probes(std::chrono::milliseconds{1000},{},{}));
    EXPECT_NO_THROW(draft21_fetch_probes(std::chrono::milliseconds{1000},{},{}));
}
TEST(FetchObservers, CancellationWaitsForBothTerminalsAndSessionClosureNeverSuppliesMissingReset) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles)if(p.expectation!=FetchProbeExpectation::FailedUpdateDataReset) {
        auto t=fetch_stimulus(p);EXPECT_FALSE(p.definition.response_ready(t));
        t.events.push_back(transport::PeerResetEvent{1,1});
        EXPECT_FALSE(p.definition.response_ready(t));
        t.events.push_back(transport::PeerResetEvent{6,1});
        EXPECT_TRUE(p.definition.response_ready(t));
        for(const auto fin_stream:{1u,6u}) {
            t=fetch_stimulus(p);t.events.push_back(transport::StreamDataEvent{fin_stream,{},true});
            EXPECT_TRUE(p.definition.response_ready(t));
            EXPECT_FALSE(evaluate_fetch_probe(t,p).has_value());
        }
        t=fetch_stimulus(p);t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,0,{}});
        EXPECT_FALSE(p.definition.response_ready(t));
        EXPECT_FALSE(evaluate_fetch_probe(t,p).has_value());
        t.events.push_back(transport::PeerResetEvent{1,1});t.events.push_back(transport::PeerResetEvent{6,1});
        EXPECT_FALSE(evaluate_fetch_probe(t,p).has_value());
    }
}
TEST(FetchObservers, ConfiguredTrackIsParsedThenEveryOtherOpeningByteIsReconstructedExactly) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles) {
        auto t=fetch_stimulus(p);
        const auto opener=p.draft==18?bytes({0x16,0,20,1,1,1,1,'n',1,'t',0,0,
            255,255,255,255,255,255,255,255,255,0,0}):
            bytes({0x16,0,20,1,1,1,'n',1,'t',1,0x21,11,0,0,255,255,255,255,255,255,255,255,255});
        const auto configured=p.draft==18?draft18_fetch_probes(std::chrono::milliseconds{1000},{bytes({'n'})},bytes({'t'})):
            draft21_fetch_probes(std::chrono::milliseconds{1000},{bytes({'n'})},bytes({'t'}));
        EXPECT_EQ(configured[0].definition.writes.front().bytes,opener);
        t.writes.front().write.bytes=opener;t.writes.front().accepted=opener.size();
        successful_cleanup(t,p);EXPECT_EQ(evaluate_fetch_probe(t,p),true);
        for(unsigned variant=0;variant<4;++variant) {
            auto bad=t;
            if(variant==0)bad.writes.front().write.bytes[3]=std::byte{3};
            if(variant==1)bad.writes.front().write.bytes.back()=std::byte{1};
            if(variant==2)bad.writes.front().write.bytes.push_back(std::byte{0});
            if(variant==3)bad.writes.front().write.fin=!bad.writes.front().write.fin;
            bad.writes.front().accepted=bad.writes.front().write.bytes.size();
            EXPECT_FALSE(evaluate_fetch_probe(bad,p).has_value())<<variant;
        }
    }
}
TEST(FetchProfiles, PropertiesAndResponseScopeMustBeValidBeforeAnyStimulus) {
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles) {
        for(const auto& invalid:{bytes({0x18,0,6,0,0,1,1,8,0}),
            bytes({0x18,0,6,0,0,1,0,0x30,2}),bytes({0x18,0,6,0,0,1,0,0x3c,0}),
            bytes({0x18,0,8,0,0,1,0,0x0b,2,0x30,2}),
            bytes({0x18,0,8,0,0,1,0,0xc0,0x40,0,0})}) {
            auto t=fetch_stimulus(p);std::get<transport::StreamDataEvent>(t.events[2]).data=invalid;
            EXPECT_FALSE(p.definition.writes[1].evidence_ready({std::span(t.writes).first(1),t.events}));
        }
        auto t=fetch_stimulus(p);auto& ok=std::get<transport::StreamDataEvent>(t.events[2]).data;
        const auto duplicate=ok;ok.insert(ok.end(),duplicate.begin(),duplicate.end());
        EXPECT_FALSE(p.definition.writes[1].evidence_ready({std::span(t.writes).first(1),t.events}));
    }
}
TEST(FetchProfiles, EmptySessionTrackIsRejectedWithoutInventingAnEmptyNameRestriction) {
    const auto ns=bytes({'.','s','e','s','s','i','o','n'});
    EXPECT_THROW(draft18_fetch_probes(std::chrono::milliseconds{1000},{ns},{}),std::invalid_argument);
    EXPECT_THROW(draft21_fetch_probes(std::chrono::milliseconds{1000},{ns},{}),std::invalid_argument);
    EXPECT_NO_THROW(draft18_fetch_probes(std::chrono::milliseconds{1000},{},{}));
    EXPECT_NO_THROW(draft21_fetch_probes(std::chrono::milliseconds{1000},{},{}));
    EXPECT_NO_THROW(draft18_fetch_probes(std::chrono::milliseconds{1000},{ns},bytes({'x'})));
    EXPECT_NO_THROW(draft21_fetch_probes(std::chrono::milliseconds{1000},{ns},bytes({'x'})));
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles) {
        auto t=fetch_stimulus(p);
        t.writes.front().write.bytes=p.draft==18?
            bytes({0x16,0,26,1,1,1,8,'.','s','e','s','s','i','o','n',0,0,0,
                255,255,255,255,255,255,255,255,255,0,0}):
            bytes({0x16,0,26,1,1,8,'.','s','e','s','s','i','o','n',0,1,0x21,11,0,0,
                255,255,255,255,255,255,255,255,255});
        t.writes.front().accepted=t.writes.front().write.bytes.size();successful_cleanup(t,p);
        EXPECT_FALSE(evaluate_fetch_probe(t,p).has_value());
    }
}
TEST(FetchCatalog, RealCatalogBindingsPassAllSixRowsAndMissingDuplicateContextsRemainNotRun) {
    using namespace requirements;
    const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto profiles=all_fetch_profiles();ASSERT_EQ(profiles.size(),6u);
    for(const auto& p:profiles) {
        const auto source=load_draft_source(p.draft,root/"docs",root/"requirements/draft-digests.json");
        const auto catalog=RequirementCatalog::load(source,root/"requirements"/("draft"+std::to_string(p.draft)+".json"));
        const auto bindings=p.draft==18?draft18_executable_bindings():draft21_executable_bindings();
        EXPECT_TRUE(std::ranges::any_of(bindings,[&](const auto& binding){
            return binding.requirement_id==p.requirement_id&&binding.scenario_id==p.definition.id&&binding.evaluator_id==p.evaluator_id;
        }));
        auto t=fetch_stimulus(p);successful_cleanup(t,p);
        const auto evaluate=[&](std::vector<RawProbeTranscript> transcripts) {
            if(p.draft==21)return evaluate_draft21_raw_probes(catalog,transcripts);
            std::vector<ScenarioContext> contexts;
            for(auto& transcript:transcripts) {
                ScenarioContext context;context.scenario_id=transcript.scenario_id;
                context.complete=context.stimulus_delivered=true;context.raw_probe=std::move(transcript);
                contexts.push_back(std::move(context));
            }
            return evaluate_draft18(catalog,contexts);
        };
        const auto state=[&](const auto& outcomes) {
            const auto found=std::ranges::find_if(outcomes,[&](const auto& outcome){return outcome.requirement_id==p.requirement_id;});
            return found==outcomes.end()?OutcomeState::NotRun:found->state;
        };
        EXPECT_EQ(state(evaluate({t})),OutcomeState::Pass)<<p.requirement_id;
        EXPECT_EQ(state(evaluate({})),OutcomeState::NotRun);
        EXPECT_EQ(state(evaluate({t,t})),OutcomeState::NotRun);
        const auto target=p.expectation==FetchProbeExpectation::CancelRequestReset?1u:6u;
        for(auto& e:t.events)if(auto* reset=std::get_if<transport::PeerResetEvent>(&e);reset&&reset->stream_id==target)
            e=transport::StreamDataEvent{target,{},true};
        const auto outcomes=evaluate({t});
        EXPECT_EQ(state(outcomes),OutcomeState::NotRun);
        EXPECT_EQ(score(catalog,outcomes).verdict,RunVerdict::Incomplete);
    }
}
}  // namespace
}  // namespace moq::interop::scenarios
