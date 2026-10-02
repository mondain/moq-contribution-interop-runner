#include "moq/interop/scenarios/discovery_overlap.h"
#include <gtest/gtest.h>
using namespace moq::interop::scenarios;
TEST(DiscoveryOverlap, FactoriesImplementEightRequiredRowsWithBothIndependentContexts) {
    EXPECT_EQ(draft18_discovery_overlap_probes().size(),4u);
    EXPECT_EQ(draft21_discovery_overlap_probes().size(),8u);
}

#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include <algorithm>
#include <filesystem>
#include <set>
namespace {
using moq::interop::transport::StreamDataEvent;
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> values) {
    Bytes result; for(auto value:values)result.push_back(static_cast<std::byte>(value));return result;
}
std::vector<DiscoveryOverlapProbe> all_profiles(std::vector<Bytes> fields = {}) {
    auto result = draft18_discovery_overlap_probes(std::chrono::milliseconds{1000},fields);
    auto d21 = draft21_discovery_overlap_probes(std::chrono::milliseconds{1000},fields);
    result.insert(result.end(),d21.begin(),d21.end());return result;
}
RawProbeTranscript transcript(const DiscoveryOverlapProbe& p, Bytes response = b({5,0,3,0x30,0,0})) {
    RawProbeTranscript t;
    t.scenario_id=p.definition.id;
    t.events={moq::interop::transport::ConnectionEstablishedEvent{},StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    t.setup={{RawProbeChannel::NewUni,p.definition.setup_bytes,false},3,4,false,1};
    t.transport_established=t.peer_setup_received=t.complete=t.stimulus_delivered=true;
    std::uint64_t stream=1;
    auto initial=std::find_if(p.definition.writes.begin(),p.definition.writes.end(),[](const auto& w){return bool(w.evidence_ready);})-p.definition.writes.begin();
    for(std::size_t i=0;i<p.definition.writes.size();++i) {
        const auto& w=p.definition.writes[i];
        const auto id=w.reuse_write_stream?*t.writes[*w.reuse_write_stream].stream_id:stream;
        if(!w.reuse_write_stream)stream+=4;
        t.writes.push_back({w,id,w.bytes.size(),false,t.events.size()});
        if(i<static_cast<std::size_t>(initial))t.events.push_back(StreamDataEvent{id,b({7,0,1,0}),false});
    }
    t.delivery_event_count=t.events.size();
    for(std::size_t i=static_cast<std::size_t>(initial);i<t.writes.size();++i)
        t.events.push_back(StreamDataEvent{*t.writes[i].stream_id,response,true});
    return t;
}
}
TEST(DiscoveryOverlap, OnlyTypedPrefixOverlapPassesAndWrongErrorOrOkFailsEveryContext) {
    for(const auto& p:all_profiles()) {
        SCOPED_TRACE(p.definition.id);
        auto t=transcript(p);
        ASSERT_TRUE(raw_probe_stimulus_valid(t,p.definition));
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),true);
        EXPECT_TRUE(p.definition.response_ready(t));
        EXPECT_EQ(evaluate_discovery_overlap_probe(transcript(p,b({5,0,3,0x10,0,0})),p),false);
        EXPECT_EQ(evaluate_discovery_overlap_probe(transcript(p,b({7,0,1,0})),p),false);
    }
}
TEST(DiscoveryOverlap, TargetTypedErrorWithUnresolvedTrailingBytesCannotPass) {
    for(const auto& p:all_profiles()) {
        SCOPED_TRACE(p.definition.id);
        auto t=transcript(p,b({5,0,3,0x30,0,0,7,0}));
        EXPECT_FALSE(evaluate_discovery_overlap_probe(t,p).has_value());
    }
}
TEST(DiscoveryOverlap, IndependentContextScoresOnlyOwnType) {
    for(const auto& p:draft21_discovery_overlap_probes()) {
        if(p.definition.id.find("independent")==std::string::npos)continue;
        auto t=transcript(p);
        const bool namespace_row=p.requirement_id=="D21-9-15-MUST-385" || p.requirement_id=="D21-9-20-21-MUST-470";
        std::get<StreamDataEvent>(t.events.back()).data=b({5,0,3,0x10,0,0});
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),namespace_row);
        std::get<StreamDataEvent>(t.events.back()).data=b({5,0,3,0x30,0,0});
        std::get<StreamDataEvent>(t.events[t.events.size()-2]).data=b({5,0,3,0x10,0,0});
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),!namespace_row);
    }
}
TEST(DiscoveryOverlap, ExactAncestorDescendantAndDisjointFixturesAreActualCanonicalWrites) {
    for(const auto& p:all_profiles({b({'m','e','d','i','a'}),b({'r','o','o','m'})})) {
        auto t=transcript(p);
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),true);
        const bool updating=p.definition.writes.back().reuse_write_stream.has_value();
        if(updating) {
            const auto& original=p.definition.writes[1].bytes;
            EXPECT_EQ(original[5],std::byte{6}); // B's first field media+b; a distinct first field.
            for(const auto& w:p.definition.writes)EXPECT_FALSE(w.fin);
        } else if(p.definition.id.find("independent")==std::string::npos) {
            ASSERT_EQ(p.definition.writes.size(),4u);
            EXPECT_EQ(p.definition.writes[1].bytes[4],std::byte{2});
            EXPECT_EQ(p.definition.writes[2].bytes[4],std::byte{1});
            EXPECT_EQ(p.definition.writes[3].bytes[4],std::byte{3});
        }
        auto changed=t;changed.writes.front().write.bytes.back()=std::byte{1};
        EXPECT_FALSE(evaluate_discovery_overlap_probe(changed,p).has_value());
        changed=t;changed.writes.back().write.bytes[3]=std::byte{1};
        EXPECT_FALSE(evaluate_discovery_overlap_probe(changed,p).has_value());
    }
    EXPECT_THROW(draft18_discovery_overlap_probes(std::chrono::milliseconds{0}),std::invalid_argument);
    EXPECT_THROW(draft21_discovery_overlap_probes(std::chrono::milliseconds{1},{Bytes{}}),std::invalid_argument);
    EXPECT_THROW(draft21_discovery_overlap_probes(std::chrono::milliseconds{1},std::vector<Bytes>(32,b({'a'}))),std::invalid_argument);
    EXPECT_THROW(draft21_discovery_overlap_probes(std::chrono::milliseconds{1},{Bytes(4095,std::byte{'a'})}),std::invalid_argument);
}
TEST(DiscoveryOverlap, UnprovedStimulusAndLateOrUntypedResponsesRemainNotRun) {
    for(const auto& p:all_profiles()) {
        SCOPED_TRACE(p.definition.id);
        const auto t=transcript(p);
        const bool namespace_row=p.requirement_id=="D21-9-15-MUST-385" || p.requirement_id=="D21-9-20-21-MUST-470";
        const bool independent=p.definition.id.find("independent")!=std::string::npos;
        const auto target=t.writes.size()-(independent && namespace_row?2u:1u);
        const auto response_index=t.events.size()-(independent && namespace_row?2u:1u);
        for(auto mutate:std::vector<std::function<void(RawProbeTranscript&)>>{
            [&](auto& x){x.writes.back().accepted--;},
            [&](auto& x){x.writes.back().delivery_event_count.reset();},
            [&](auto& x){x.writes.back().stream_id=3;},
            [&](auto& x){x.writes.front().delivery_event_count=x.events.size();},
            [&](auto& x){std::get<StreamDataEvent>(x.events[2]).fin=true;},
            [&](auto& x){std::get<StreamDataEvent>(x.events[2]).data=b({7,0});},
            [&](auto& x){x.events.insert(x.events.begin()+2,moq::interop::transport::PeerResetEvent{1,1});},
            [&](auto& x){x.events.insert(x.events.begin()+2,moq::interop::transport::PeerCloseEvent{});},
            [&](auto& x){std::get<StreamDataEvent>(x.events[response_index]).data=b({5,0,3,0x30});},
            [&](auto& x){std::get<StreamDataEvent>(x.events[response_index]).data=b({4,0,2,0,0});},
            [&](auto& x){x.events.insert(x.events.begin()+static_cast<std::ptrdiff_t>(response_index),moq::interop::transport::PeerResetEvent{*x.writes[target].stream_id,1});},
            [&](auto& x){x.events.insert(x.events.begin()+static_cast<std::ptrdiff_t>(response_index),StreamDataEvent{*x.writes[target].stream_id,{},true});},
            [&](auto& x){x.events.insert(x.events.begin()+static_cast<std::ptrdiff_t>(response_index),moq::interop::transport::PeerCloseEvent{});},
            [&](auto& x){x.events[response_index]=StreamDataEvent{999,b({5,0,3,0x30,0,0}),true};}
        }) {
            auto changed=t;mutate(changed);
            EXPECT_FALSE(evaluate_discovery_overlap_probe(changed,p).has_value());
        }
    }
}
TEST(DiscoveryOverlap, IndependentUpdateSecondChallengeSurvivesFirstFailedStreamFin) {
    for(const auto& p:draft21_discovery_overlap_probes()) {
        if(p.definition.id!="d21-discovery-update-independent-overlap-spaces")continue;
        auto t=transcript(p);
        const auto first=t.events[t.events.size()-2];
        t.events.erase(t.events.end()-2);
        const auto marker=*t.writes.back().delivery_event_count;
        t.events.insert(t.events.begin()+static_cast<std::ptrdiff_t>(marker),first);
        ++*t.writes.back().delivery_event_count;
        ++*t.delivery_event_count;
        EXPECT_TRUE(p.definition.writes.back().evidence_ready({std::span(t.writes).first(5),std::span(t.events).first(*t.writes.back().delivery_event_count)}));
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),true);
    }
}
TEST(DiscoveryOverlap, InitialNamespaceOkMayCarryValidNotificationsButNotMalformedTail) {
    for(const auto& p:all_profiles()) {
        if(p.definition.writes.front().bytes.front()!=std::byte{0x50})continue;
        auto t=transcript(p);
        auto& ack=std::get<StreamDataEvent>(t.events[2]).data;
        const auto suffix=b({8,0,3,1,1,'z',14,0,3,1,1,'z'});
        ack.insert(ack.end(),suffix.begin(),suffix.end());
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),true);
        ack.push_back(std::byte{8});
        EXPECT_FALSE(evaluate_discovery_overlap_probe(t,p).has_value());
    }
}
TEST(DiscoveryOverlap, RealCatalogRequiresAllNamedContextsAndRejectsDuplicatesWithFailDominance) {
    namespace req=moq::interop::requirements;
    const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    for(unsigned draft:{18u,21u})for(const auto fields:std::vector<std::vector<Bytes>>{{b({'a'})},{b({'m'}),b({'r'})}}) {
        const auto source=req::load_draft_source(draft,root/"docs",root/"requirements/draft-digests.json");
        const auto catalog=req::RequirementCatalog::load(source,root/"requirements"/(draft==18?"draft18.json":"draft21.json"));
        const auto profiles=draft==18?draft18_discovery_overlap_probes(std::chrono::milliseconds{1000},fields):draft21_discovery_overlap_probes(std::chrono::milliseconds{1000},fields);
        std::vector<RawProbeTranscript> ts;std::set<std::string> seen;
        for(const auto& p:profiles)if(seen.insert(p.definition.id).second)ts.push_back(transcript(p));
        const auto evaluate=[&](const auto& input){
            if(draft==21)return req::evaluate_draft21_raw_probes(catalog,input);
            std::vector<req::ScenarioContext> contexts;
            for(const auto& t:input){req::ScenarioContext c;c.scenario_id=t.scenario_id;c.complete=true;c.stimulus_delivered=true;c.raw_probe=t;contexts.push_back(c);}
            return req::evaluate_draft18(catalog,contexts);
        };
        const auto state=[&](const auto& input,const auto& id){auto outcomes=evaluate(input);return std::find_if(outcomes.begin(),outcomes.end(),[&](const auto& o){return o.requirement_id==id;})->state;};
        for(const auto& p:profiles) {
            EXPECT_EQ(state(ts,p.requirement_id),req::OutcomeState::Pass);
            auto missing=ts;std::erase_if(missing,[&](const auto& t){return t.scenario_id==p.definition.id;});
            EXPECT_EQ(state(missing,p.requirement_id),req::OutcomeState::NotRun);
            auto duplicate=ts;const auto found=std::find_if(ts.begin(),ts.end(),[&](const auto& t){return t.scenario_id==p.definition.id;});duplicate.push_back(*found);
            EXPECT_EQ(state(duplicate,p.requirement_id),req::OutcomeState::NotRun);
            auto wrong=missing;wrong.push_back(transcript(p,b({5,0,3,0x10,0,0})));
            EXPECT_EQ(state(wrong,p.requirement_id),req::OutcomeState::Fail);
        }
    }
}

TEST(DiscoveryOverlap, InitialFinOrResetBeforeAcceptedWriteCannotBeRevivedByLateAck) {
    for(const auto& p:all_profiles())for(bool reset:{false,true}) {
        auto t=transcript(p);
        const moq::interop::transport::TransportEvent close=reset?moq::interop::transport::TransportEvent{moq::interop::transport::PeerResetEvent{1,1}}:
            moq::interop::transport::TransportEvent{StreamDataEvent{1,{},true}};
        t.events.insert(t.events.begin()+2,close);
        for(auto& write:t.writes)++*write.delivery_event_count;
        ++*t.delivery_event_count;
        EXPECT_FALSE(evaluate_discovery_overlap_probe(t,p).has_value());
    }
}
TEST(DiscoveryOverlap, WrongAcceptedNamespaceChallengeWithLegalNotificationsStillFails) {
    for(const auto& p:all_profiles()) {
        if(p.definition.writes.back().reuse_write_stream ||
            (p.requirement_id!="D18-10-18-MUST-003" && p.requirement_id!="D21-9-15-MUST-385"))continue;
        auto t=transcript(p,b({7,0,1,0,8,0,3,1,1,'z'}));
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),false);
    }
}

TEST(DiscoveryOverlap, WrongAcceptedNamespaceUpdateWithLegalNotificationsStillFails) {
    for(const auto& p:all_profiles()) {
        if(!p.definition.writes.back().reuse_write_stream ||
            (p.requirement_id!="D18-10-2-14-MUST-001" && p.requirement_id!="D21-9-20-21-MUST-470"))continue;
        SCOPED_TRACE(p.definition.id);
        auto t=transcript(p,b({7,0,1,0,8,0,3,1,1,'z',14,0,3,1,1,'z'}));
        if(p.definition.id=="d21-discovery-update-independent-overlap-spaces")
            std::get<StreamDataEvent>(t.events.back()).data=b({7,0,1,0});
        EXPECT_EQ(evaluate_discovery_overlap_probe(t,p),false);
        EXPECT_TRUE(p.definition.response_ready(t));
    }
}
