#include "moq/interop/scenarios/subscription_cancel.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for(const auto value:values) result.push_back(static_cast<std::byte>(value));
    return result;
}
std::vector<SubscriptionCancelProbe> profiles() {
    auto result=draft18_subscription_cancel_probes();auto d21=draft21_subscription_cancel_probes();
    result.insert(result.end(),d21.begin(),d21.end());return result;
}
RawProbeTranscript stimulus(const SubscriptionCancelProbe& p,unsigned count=2) {
    RawProbeTranscript t;t.scenario_id=p.definition.id;
    t.setup={{RawProbeChannel::NewUni,bytes({0xaf,0,0,0}),false},3,4,false,1};
    t.events={transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2,bytes({0xaf,0,0,0}),false},
        transport::StreamDataEvent{1,bytes({4,0,2,7,0}),false}};
    for(unsigned i=0;i<count;++i)
        t.events.push_back(transport::StreamDataEvent{6+4*i,bytes({0x34,7,i,0}),false});
    t.writes.push_back({{RawProbeChannel::NewBidi,bytes({3,0,5,1,0,1,'x',0}),true},1,8,true,2});
    t.writes.push_back({p.definition.writes[1],1,0,false,t.events.size(),true});
    t.delivery_event_count=t.events.size();
    t.complete=t.stimulus_delivered=t.transport_established=t.peer_setup_received=true;
    return t;
}
void resets(RawProbeTranscript& t,unsigned count=2) {
    t.events.push_back(transport::PeerResetEvent{1,99});
    for(unsigned i=0;i<count;++i)t.events.push_back(transport::PeerResetEvent{6+4*i,77+i});
}
bool gate(const SubscriptionCancelProbe& p,const RawProbeTranscript& t) {
    return p.definition.writes[1].evidence_ready({std::span(t.writes).first(1),t.events});
}
TEST(SubscriptionCancelProfiles, TwoRequiredProfilesHaveIndependentLiteralStimuli) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    const char* ids[]={"D18-5-1-1-MUST-001","D21-3-1-1-MUST-045"};
    const char* evaluators[]={"all-open-subscription-streams-reset","d21-subscribe-stop-sending-resets-open-streams"};
    const char* scenarios[]={"cancel-subscribe-with-multiple-open-subgroups","d21-cancel-subscribe-with-open-streams"};
    for(std::size_t i=0;i<all.size();++i) {
        const auto& p=all[i];EXPECT_EQ(p.requirement_id,ids[i]);EXPECT_EQ(p.evaluator_id,evaluators[i]);
        EXPECT_EQ(p.definition.id,scenarios[i]);EXPECT_EQ(p.definition.setup_bytes,bytes({0xaf,0,0,0}));
        ASSERT_EQ(p.definition.writes.size(),2u);
        EXPECT_EQ(p.definition.writes[0].bytes,bytes({3,0,5,1,0,1,'x',0}));
        EXPECT_TRUE(p.definition.writes[0].fin);
        const auto& stop=p.definition.writes[1];EXPECT_TRUE(stop.bytes.empty());EXPECT_FALSE(stop.fin);
        EXPECT_EQ(stop.operation,RawProbeOperation::StopSending);EXPECT_EQ(stop.application_error,1u);
        EXPECT_EQ(stop.reuse_write_stream,0u);ASSERT_TRUE(stop.evidence_ready);
    }
}
TEST(SubscriptionCancelProfiles, SnapshotIncludesEveryMatchingOpenStreamAndRequest) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all)for(unsigned count:{2u,3u,4u}) {
        auto t=stimulus(p,count);ASSERT_TRUE(gate(p,t));EXPECT_FALSE(p.definition.response_ready(t));
        resets(t,count);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        EXPECT_TRUE(p.definition.response_ready(t));
        for(unsigned missing=0;missing<=count;++missing) {
            auto incomplete=t;incomplete.events.erase(incomplete.events.begin()+*t.delivery_event_count+missing);
            EXPECT_FALSE(evaluate_subscription_cancel_probe(incomplete,p).has_value());
            EXPECT_FALSE(p.definition.response_ready(incomplete));
        }
        auto fin=stimulus(p,count);fin.events.push_back(transport::StreamDataEvent{6+4*(count-1),{},true});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(fin,p).has_value());
        EXPECT_TRUE(p.definition.response_ready(fin));
    }
}
TEST(SubscriptionCancelProfiles, ClosedAndUnrelatedStreamsAreExcludedWhileMatchingStreamsBeforeOkAreIncluded) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        auto t=stimulus(p);t.events.push_back(transport::StreamDataEvent{14,bytes({0x30,7,2}),true});
        t.events.push_back(transport::StreamDataEvent{18,bytes({0x30,8,3}),false});
        t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,4,0}),false});
        t.events.push_back(transport::PeerResetEvent{22,1});
        t.writes.back().delivery_event_count=t.delivery_event_count=t.events.size();
        EXPECT_TRUE(gate(p,t));resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        t=stimulus(p);std::swap(t.events[2],t.events[3]);
        EXPECT_TRUE(gate(p,t));resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
    }
}
TEST(SubscriptionCancelProfiles, FragmentedAndNonminimalLegalHeadersGateOnlyWhenComplete) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        auto t=stimulus(p);t.events.pop_back();t.events.pop_back();
        t.events.push_back(transport::StreamDataEvent{6,bytes({0x80,0x10}),false}); // vi64 0x10
        EXPECT_FALSE(gate(p,t));
        t.events.push_back(transport::StreamDataEvent{6,bytes({0x80,7,0x80,0}),false}); // alias7/group0
        EXPECT_FALSE(gate(p,t)); // priority still missing
        t.events.push_back(transport::StreamDataEvent{6,bytes({0xff}),false});
        t.events.push_back(transport::StreamDataEvent{10,bytes({0x34,7}),false});
        EXPECT_FALSE(gate(p,t));
        t.events.push_back(transport::StreamDataEvent{10,bytes({1,0}),false});
        EXPECT_TRUE(gate(p,t));
        t.writes.back().delivery_event_count=t.delivery_event_count=t.events.size();
        resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
    }
}
TEST(SubscriptionCancelProfiles, InvalidResponseOrHeaderCannotEstablishCancellationPrerequisites) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        for(const auto& response:{bytes({7,0,1,0}),bytes({4,0,3,7,1,8}),
            bytes({4,0,4,7,0,0x30,2}),bytes({4,0,4,7,0,0x3c,0}),
            bytes({4,0,4,7,1,0x10,2}),bytes({4,0,4,7,1,6,0}),
            bytes({4,0,6,7,0,0x0b,2,0x30,2})}) {
            auto t=stimulus(p);std::get<transport::StreamDataEvent>(t.events[2]).data=response;
            EXPECT_FALSE(gate(p,t));
        }
        for(const auto& header:{bytes({0x36,7,1,0}),bytes({0x34,7,1}),bytes({0x10,7,1}),
            bytes({0x34}),bytes({0x34,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1,0})}) {
            auto t=stimulus(p);std::get<transport::StreamDataEvent>(t.events[4]).data=header;
            EXPECT_FALSE(gate(p,t));
        }
        auto t=stimulus(p);std::get<transport::StreamDataEvent>(t.events[4]).data=bytes({0x34,8,1,0});
        EXPECT_FALSE(gate(p,t)); // only one matching stream
        t=stimulus(p);auto& ok=std::get<transport::StreamDataEvent>(t.events[2]).data;
        const auto duplicate=ok;ok.insert(ok.end(),duplicate.begin(),duplicate.end());EXPECT_FALSE(gate(p,t));
    }
}
TEST(SubscriptionCancelProfiles, PreterminalOrUnacceptedOpenerAndAmbiguousPartialHeaderAreRejected) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all)for(unsigned variant=0;variant<10;++variant) {
        auto t=stimulus(p);
        if(variant==0)t.events.insert(t.events.begin()+3,transport::PeerResetEvent{6,1});
        if(variant==1)t.events.push_back(transport::PeerResetEvent{1,1});
        if(variant==2)t.events.push_back(transport::StreamDataEvent{14,bytes({0x34}),false});
        if(variant==3)t.writes.front().delivery_event_count=4;
        if(variant==4)t.writes.front().accepted=7;
        if(variant==5)t.writes.front().fin_accepted=false;
        if(variant==6)t.writes.front().write.fin=false;
        if(variant==7)std::get<transport::StreamDataEvent>(t.events[4]).stream_id=11;
        if(variant==8)t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,0,{}});
        if(variant==9)t.events.push_back(transport::StreamDataEvent{1,{},true});
        EXPECT_FALSE(gate(p,t))<<variant;
    }
}
TEST(SubscriptionCancelProfiles, ConcurrentPublishWithSameAliasMakesDataAssociationAmbiguous) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        const auto publish=bytes({0x1d,0,6,0,0,1,'x',7,0});
        auto t=stimulus(p);t.events.push_back(transport::StreamDataEvent{0,publish,false});
        EXPECT_FALSE(gate(p,t));
        t.writes.back().delivery_event_count=t.delivery_event_count=t.events.size();
        resets(t);EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        for(std::size_t length=0;length<publish.size();++length) {
            t=stimulus(p);t.events.push_back(transport::StreamDataEvent{0,Bytes(publish.begin(),publish.begin()+length),false});
            EXPECT_FALSE(gate(p,t))<<length;
        }
        t=stimulus(p);auto other=publish;other[7]=std::byte{8};
        t.events.push_back(transport::StreamDataEvent{0,other,false});EXPECT_TRUE(gate(p,t));
        t.writes.back().delivery_event_count=t.delivery_event_count=t.events.size();
        resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        t=stimulus(p);t.events.push_back(transport::StreamDataEvent{0,bytes({6,0,3,0,0,0}),false});
        EXPECT_TRUE(gate(p,t)); // independent PUBLISH_NAMESPACE, not a track alias
    }
}
TEST(SubscriptionCancelObservers, WrongEarlyFutureOrSessionCloseTerminalsNeverSupplyMissingResets) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all)for(unsigned variant=0;variant<9;++variant) {
        auto t=stimulus(p);resets(t);
        if(variant==0)t.writes.back().operation_accepted=false;
        if(variant==1)t.writes.back().stream_id=5;
        if(variant==2)t.writes.back().delivery_event_count=4;
        if(variant==3)t.events.insert(t.events.begin()+3,transport::PeerResetEvent{6,1});
        if(variant==4)std::get<transport::PeerResetEvent>(t.events.back()).stream_id=14;
        if(variant==5)t.events.insert(t.events.begin()+5,transport::PeerCloseEvent{transport::CloseErrorSpace::Application,0,{}});
        if(variant==6)t.events.push_back(transport::StreamDataEvent{6,bytes({0}),false});
        if(variant==7)t.delivery_event_count=t.events.size()+1;
        if(variant==8)t.events.push_back(transport::PeerResetEvent{6,1});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value())<<variant;
    }
}
TEST(SubscriptionCancelObservers, SplitResetBatchesWaitAndFinEndsWithoutInventingPeerReceiptEvidence) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        auto t=stimulus(p);t.events.push_back(transport::PeerResetEvent{6,1});
        EXPECT_FALSE(p.definition.response_ready(t));EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        t.events.push_back(transport::PeerResetEvent{1,1});EXPECT_FALSE(p.definition.response_ready(t));
        t.events.push_back(transport::PeerResetEvent{10,1});EXPECT_TRUE(p.definition.response_ready(t));
        EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        for(const auto id:{1u,6u,10u}) {
            t=stimulus(p);t.events.push_back(transport::StreamDataEvent{id,{},true});
            EXPECT_TRUE(p.definition.response_ready(t));EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        }
    }
}
TEST(SubscriptionCancelObservers, LaterPublishControlCannotRetroactivelyMakeSnapshotAttributionCertain) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        auto t=stimulus(p);resets(t);
        t.events.push_back(transport::StreamDataEvent{0,bytes({0x1d,0,6,0,0,1,'x',7,0}),false});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        EXPECT_FALSE(p.definition.response_ready(t));
        std::get<transport::StreamDataEvent>(t.events.back()).data[7]=std::byte{8};
        EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        std::get<transport::StreamDataEvent>(t.events.back()).data.resize(8);
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        t=stimulus(p);resets(t);
        t.events.push_back(transport::StreamDataEvent{0,Bytes(65547,std::byte{0}),false});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        t=stimulus(p);t.writes.back().delivery_event_count=t.delivery_event_count=4;
        resets(t);EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        // The second matching header exists only after the accepted STOP marker.
    }
}
TEST(SubscriptionCancelObservers, ReorderedLateSubgroupsNeedTheirOwnResetWithoutProvingInitialGate) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        auto t=stimulus(p);resets(t);
        t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,2,0}),false});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());EXPECT_FALSE(p.definition.response_ready(t));
        t.events.push_back(transport::PeerResetEvent{22,9});
        EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);EXPECT_TRUE(p.definition.response_ready(t));
        t=stimulus(p);t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,2,0}),true});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());EXPECT_TRUE(p.definition.response_ready(t));
        t=stimulus(p);resets(t);t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7}),false});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());EXPECT_FALSE(p.definition.response_ready(t));
        t.events.push_back(transport::StreamDataEvent{22,bytes({2,0}),false});
        t.events.push_back(transport::PeerResetEvent{22,9});EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        t=stimulus(p);resets(t);t.events.push_back(transport::PeerResetEvent{22,9});
        t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,2,0}),false});
        EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true); // reset tombstone stays closed
        t.events.push_back(transport::StreamDataEvent{22,bytes({0}),false});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value()); // no post-reset objects
        t=stimulus(p);resets(t);t.events.push_back(transport::PeerResetEvent{22,9});
        t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,2,0,0}),false});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        t=stimulus(p);resets(t);t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7}),false});
        t.events.push_back(transport::PeerResetEvent{22,9});
        t.events.push_back(transport::StreamDataEvent{22,bytes({2,0}),false});
        EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        t=stimulus(p);resets(t);t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,0,{}});
        t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,2,0}),false});
        t.events.push_back(transport::PeerResetEvent{22,9});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value());
        t=stimulus(p);resets(t);t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,8,2,0}),false});
        EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        t=stimulus(p);t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,2,0}),true});
        t.writes.back().delivery_event_count=t.delivery_event_count=t.events.size();
        resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true); // already closed before STOP
        t=stimulus(p);t.writes.back().delivery_event_count=t.delivery_event_count=4;
        t.events.push_back(transport::StreamDataEvent{22,bytes({0x34,7,2,0}),false});resets(t);
        t.events.push_back(transport::PeerResetEvent{22,9});
        EXPECT_FALSE(evaluate_subscription_cancel_probe(t,p).has_value()); // initial minimum still missing
    }
}
TEST(SubscriptionCancelProfiles, HeaderBoundsAndEveryDefinedFlagModeStayUsable) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        for(const auto& header:{bytes({0x10,7,1,128}),bytes({0x12,7,1,255}),
            bytes({0x14,7,1,3,0}),bytes({0x30,7,1}),bytes({0x32,7,1}),bytes({0x7d,7,1,3})}) {
            auto t=stimulus(p);std::get<transport::StreamDataEvent>(t.events[4]).data=header;
            EXPECT_TRUE(gate(p,t));resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        }
        auto t=stimulus(p);
        auto& header=std::get<transport::StreamDataEvent>(t.events[4]).data;
        header=bytes({255,0,0,0,0,0,0,0,0x14,255,0,0,0,0,0,0,0,7,
            255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,0});
        ASSERT_EQ(header.size(),37u);EXPECT_TRUE(gate(p,t));
        for(unsigned i=0;i<63;++i)t.events.push_back(transport::StreamDataEvent{14+4*i,bytes({0x30,8,i}),false});
        EXPECT_FALSE(gate(p,t));
        t=stimulus(p);t.events.resize(4097,transport::ConnectionEstablishedEvent{});EXPECT_FALSE(gate(p,t));
    }
}
TEST(SubscriptionCancelProfiles, FragmentedOkAndLegalExpiresAndLargestObjectUseTypedResponseScope) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        auto t=stimulus(p);std::get<transport::StreamDataEvent>(t.events[2]).data=bytes({4,0,2,7});
        EXPECT_FALSE(gate(p,t));t.events.push_back(transport::StreamDataEvent{1,bytes({0}),false});
        EXPECT_TRUE(gate(p,t));
        t.writes.back().delivery_event_count=t.delivery_event_count=t.events.size();
        resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        t=stimulus(p);
        std::get<transport::StreamDataEvent>(t.events[2]).data=bytes({4,0,7,7,2,8,3,1,1,2});
        EXPECT_TRUE(gate(p,t));resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        EXPECT_THROW((p.draft==18?draft18_subscription_cancel_probes(std::chrono::milliseconds{0}):
            draft21_subscription_cancel_probes(std::chrono::milliseconds{0})),std::invalid_argument);
        EXPECT_THROW((p.draft==18?draft18_subscription_cancel_probes(std::chrono::milliseconds{1000},{bytes({'.'})}):
            draft21_subscription_cancel_probes(std::chrono::milliseconds{1000},{bytes({'.'})})),std::invalid_argument);
    }
}
TEST(SubscriptionCancelObservers, ConfiguredFixtureAndDynamicAliasRebuildStrictly) {
    const auto all=profiles();ASSERT_EQ(all.size(),2u);
    for(const auto& p:all) {
        const auto configured=p.draft==18?draft18_subscription_cancel_probes(std::chrono::milliseconds{1000},{bytes({'n'})},bytes({'t'})):
            draft21_subscription_cancel_probes(std::chrono::milliseconds{1000},{bytes({'n'})},bytes({'t'}));
        const auto opener=bytes({3,0,7,1,1,1,'n',1,'t',0});
        ASSERT_EQ(configured.size(),1u);EXPECT_EQ(configured[0].definition.writes[0].bytes,opener);
        auto t=stimulus(p);t.writes.front().write.bytes=opener;t.writes.front().accepted=opener.size();
        std::get<transport::StreamDataEvent>(t.events[2]).data=bytes({4,0,2,9,0});
        for(unsigned i:{3u,4u})std::get<transport::StreamDataEvent>(t.events[i]).data[1]=std::byte{9};
        resets(t);EXPECT_EQ(evaluate_subscription_cancel_probe(t,p),true);
        for(unsigned variant=0;variant<5;++variant) {
            auto bad=t;
            if(variant==0)bad.writes.front().write.bytes[3]=std::byte{3};
            if(variant==1)bad.writes.front().write.bytes.back()=std::byte{1};
            if(variant==2)bad.writes.front().write.bytes.push_back(std::byte{0});
            if(variant==3)bad.writes.front().write.fin=false;
            if(variant==4)std::get<transport::StreamDataEvent>(bad.events[4]).data[1]=std::byte{7};
            bad.writes.front().accepted=bad.writes.front().write.bytes.size();
            EXPECT_FALSE(evaluate_subscription_cancel_probe(bad,p).has_value())<<variant;
        }
    }
}
TEST(SubscriptionCancelCatalog, BothActualRowsPassAndMissingDuplicateOrFinContextsAreNotRun) {
    using namespace requirements;
    const auto all=profiles();ASSERT_EQ(all.size(),2u);const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    for(const auto& p:all) {
        const auto source=load_draft_source(p.draft,root/"docs",root/"requirements/draft-digests.json");
        const auto catalog=RequirementCatalog::load(source,root/"requirements"/("draft"+std::to_string(p.draft)+".json"));
        const auto bindings=p.draft==18?draft18_executable_bindings():draft21_executable_bindings();
        EXPECT_TRUE(std::ranges::any_of(bindings,[&](const auto& b){return b.requirement_id==p.requirement_id&&
            b.scenario_id==p.definition.id&&b.evaluator_id==p.evaluator_id;}));
        const auto evaluate=[&](std::vector<RawProbeTranscript> transcripts) {
            if(p.draft==21)return evaluate_draft21_raw_probes(catalog,transcripts);
            std::vector<ScenarioContext> contexts;
            for(auto& transcript:transcripts){ScenarioContext c;c.scenario_id=transcript.scenario_id;
                c.complete=c.stimulus_delivered=true;c.raw_probe=std::move(transcript);contexts.push_back(std::move(c));}
            return evaluate_draft18(catalog,contexts);
        };
        const auto state=[&](const auto& outcomes){const auto found=std::ranges::find_if(outcomes,[&](const auto& o){
            return o.requirement_id==p.requirement_id;});return found==outcomes.end()?OutcomeState::NotRun:found->state;};
        auto t=stimulus(p);resets(t);EXPECT_EQ(state(evaluate({t})),OutcomeState::Pass);
        EXPECT_EQ(state(evaluate({})),OutcomeState::NotRun);EXPECT_EQ(state(evaluate({t,t})),OutcomeState::NotRun);
        t=stimulus(p);t.events.push_back(transport::StreamDataEvent{10,{},true});
        const auto outcomes=evaluate({t});EXPECT_EQ(state(outcomes),OutcomeState::NotRun);
        EXPECT_EQ(score(catalog,outcomes).verdict,RunVerdict::Incomplete);
    }
}
}  // namespace
}  // namespace moq::interop::scenarios
