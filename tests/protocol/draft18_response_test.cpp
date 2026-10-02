#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/wire/draft18/messages.h"

#include <gtest/gtest.h>
#include <algorithm>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
struct Fixture {
    const char* id;
    const char* requirement;
    const char* evaluator;
    Draft18ResponseExpectation expectation;
    bool scoped;
    Bytes initial;
    Bytes ok;
};
std::vector<Fixture> fixtures() {
    // Independent literal fields from draft18 sections10.7/10.8/10.9/10.18.
    return {
        {"reject-subscription-request-update", "D18-10-9-1-MUST-001",
         "publish-done-update-failed", Draft18ResponseExpectation::FailedSubscriptionCleanup,
         false, b({3,0,5,1,0,1,'x',0}), b({4,0,4,0,0,4,1})},
        {"reject-subscribe-namespace-request-update", "D18-10-9-1-MUST-003",
         "namespace-request-stream-closed", Draft18ResponseExpectation::FailedDiscoveryCleanup,
         true, b({0x50,0,3,1,0,0}), b({7,0,1,0})},
    };
}
const Draft18ResponseProbe* find(const std::vector<Draft18ResponseProbe>& profiles, const char* id) {
    const auto it = std::find_if(profiles.begin(), profiles.end(),
        [&](const auto& p) { return p.definition.id == id; });
    return it == profiles.end() ? nullptr : &*it;
}
RawProbeTranscript stimulus(const Fixture& f) {
    RawProbeTranscript t;
    t.scenario_id = f.id;
    t.setup = {{RawProbeChannel::NewUni,b({0xaf,0,0,0}),false},3,4,false};
    t.complete = t.stimulus_delivered = t.transport_established = t.peer_setup_received = true;
    t.delivery_event_count = 3;
    t.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2,b({0xaf,0,0,0}),false},
        transport::StreamDataEvent{1,f.ok,false}};
    t.writes.push_back({{RawProbeChannel::NewBidi,f.initial,false},1,f.initial.size(),false,2});
    RawProbeWrite update{RawProbeChannel::NewBidi,b({2,0,6,3,1,3,2,2,0}),true,0,
        [](auto) { return true; }};
    t.writes.push_back({update,1,update.bytes.size(),true,3});
    return t;
}
void reply(RawProbeTranscript& t, Bytes data, bool fin = false) {
    t.events.push_back(transport::StreamDataEvent{1,std::move(data),fin});
}
Bytes joined(Bytes first, const Bytes& second) {
    first.insert(first.end(),second.begin(),second.end());
    return first;
}
Bytes response(const Fixture& f, Bytes error = b({5,0,3,1,0,0})) {
    return f.scoped ? error : joined(std::move(error),b({0x0b,0,3,8,0,0}));
}

TEST(Draft18ResponseProfiles, DefinesTwoRealLiteralStagedUpdatesWithAcceptedFin) {
    const auto profiles = draft18_response_probes(std::chrono::milliseconds{71});
    ASSERT_EQ(profiles.size(),2u);
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.id);
        const auto* p = find(profiles,f.id); ASSERT_NE(p,nullptr);
        EXPECT_EQ(p->requirement_id,f.requirement); EXPECT_EQ(p->evaluator_id,f.evaluator);
        EXPECT_EQ(p->expectation,f.expectation); EXPECT_EQ(p->namespace_scoped,f.scoped);
        EXPECT_EQ(p->definition.deadline,std::chrono::milliseconds{71});
        EXPECT_EQ(p->definition.setup_bytes,b({0xaf,0,0,0}));
        ASSERT_TRUE(p->definition.response_ready);
        ASSERT_EQ(p->definition.writes.size(),2u);
        EXPECT_EQ(p->definition.writes[0].bytes,f.initial);
        EXPECT_EQ(p->definition.writes[0].channel,RawProbeChannel::NewBidi);
        EXPECT_FALSE(p->definition.writes[0].fin);
        EXPECT_EQ(p->definition.writes[1].bytes,b({2,0,6,3,1,3,2,2,0}));
        EXPECT_EQ(p->definition.writes[1].reuse_write_stream,0u);
        EXPECT_TRUE(p->definition.writes[1].fin);
        ASSERT_TRUE(p->definition.writes[1].peer_response_ready);
        EXPECT_TRUE(p->definition.writes[1].peer_response_ready(f.ok));
        for (std::size_t n=0;n<f.ok.size();++n)
            EXPECT_FALSE(p->definition.writes[1].peer_response_ready(std::span{f.ok}.first(n)));
        EXPECT_FALSE(p->definition.writes[1].peer_response_ready(b({5,0,3,1,0,0})));
    }
}

TEST(Draft18ResponseProfiles, InitialSuccessUsesDraft18ScopesAndLegalTrackProperties) {
    const auto profiles = draft18_response_probes();
    const auto* subscription = find(profiles,fixtures()[0].id); ASSERT_NE(subscription,nullptr);
    const auto* ns = find(profiles,fixtures()[1].id); ASSERT_NE(ns,nullptr);
    const auto& sub_ready = subscription->definition.writes[1].peer_response_ready;
    const auto& ns_ready = ns->definition.writes[1].peer_response_ready;
    // Properties optional. EXPIRES8 and LARGEST9 are counted parameters.
    for (const auto& input : {b({4,0,2,0,0}),b({4,0,9,0,2,8,10,1,0,0,4,1}),
                            b({4,0,4,0,0,0x22,2}),b({4,0,4,0,0,0x30,1}),
                            b({4,0,6,0,0,0x0b,2,0x22,1}),
                            b({4,0,8,0,0,0x0b,4,0x0b,2,0x30,0}),
                            b({4,0,4,0,0,0x38,7})})
        EXPECT_TRUE(sub_ready(input));
    for (const auto& input : {b({4,0,4,0,0,0x22,0}),b({4,0,4,0,0,0x30,2}),
                            b({4,0,5,0,0,0x0e,0x81,0}),
                            b({4,0,6,0,0,0xc0,0x40,0,0}),
                            b({4,0,4,0,0,0x3c,0}),b({4,0,5,0,0,0x0b,1,0x22}),
                            b({4,0,6,0,0,0x0b,2,0x30,2}),
                            b({4,0,7,0,2,8,10,0,11}),b({4,0,4,0,1,0x10,1})})
        EXPECT_FALSE(sub_ready(input));
    EXPECT_TRUE(ns_ready(b({7,0,1,0})));
    // D18 section10.2.10 excludes namespace OK; D21 permits EXPIRES there.
    for (const auto& input : {b({7,0,3,1,8,10}),b({7,0,4,1,9,0,0}),
                            b({7,0,3,0,4,1}),b({4,0,2,0,0})})
        EXPECT_FALSE(ns_ready(input));
}

TEST(Draft18ResponseProfiles, NamespaceSuccessMayCoalesceCompleteValidNotifications) {
    const auto profiles=draft18_response_probes(); const auto f=fixtures()[1];
    const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
    const auto& ready=p->definition.writes[1].peer_response_ready;
    const auto announced=joined(f.ok,b({8,0,3,1,1,'n'}));
    const auto removed=joined(announced,b({0x0e,0,3,1,1,'n'}));
    EXPECT_TRUE(ready(announced));EXPECT_TRUE(ready(removed));
    for (std::size_t n=f.ok.size()+1;n<announced.size();++n)
        EXPECT_FALSE(ready(std::span{announced}.first(n)));
    for (const auto& invalid : {joined(f.ok,b({8,0,2,1,0})),
                               joined(f.ok,b({0x0e,0,3,1,1,'n'})),
                               joined(f.ok,b({7,0,1,0})),
                               joined(f.ok,b({5,0,3,1,0,0})),
                               joined(f.ok,b({0x0b,0,3,8,0,0}))})
        EXPECT_FALSE(ready(invalid));
    for (const auto& initial : {announced,removed}) {
        auto t=stimulus(f);std::get<transport::StreamDataEvent>(t.events[2]).data=initial;
        reply(t,response(f),true);
        EXPECT_EQ(evaluate_draft18_response_probe(t,*p),true);
    }
}

TEST(Draft18ResponseObserver, AnyActualValidErrorFollowedByFinOrResetProvesCleanup) {
    const auto profiles = draft18_response_probes();
    const Bytes errors[] = {b({5,0,3,1,0,0}),b({5,0,3,0x17,0,0}),
        b({5,0,11,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0,0}),
        b({5,0,6,1,0,3,0xe2,0x82,0xac}),b({5,0,6,0x34,0,0,0,0,0})};
    for (const auto& f : fixtures()) {
        const auto* p=find(profiles,f.id); ASSERT_NE(p,nullptr);
        for (const auto& error : errors) for (const bool reset : {false,true}) {
            auto t=stimulus(f); reply(t,response(f,error),!reset);
            if (reset) t.events.push_back(transport::PeerResetEvent{1,1});
            EXPECT_TRUE(p->definition.response_ready(t));
            EXPECT_EQ(evaluate_draft18_response_probe(t,*p),true);
            // This observation never assigns UNKNOWN_AUTH_TOKEN_ALIAS a code.
            EXPECT_FALSE(t.unknown_auth_token_alias_compatibility_code.has_value());
        }
    }
}

TEST(Draft18ResponseObserver, SubscriptionRequiresAdditionalTypedUpdateFailedDone) {
    const auto profiles=draft18_response_probes(); const auto f=fixtures()[0];
    const auto* p=find(profiles,f.id); ASSERT_NE(p,nullptr);
    for (const auto& done : {Bytes{},b({0x0b,0,3,1,0,0}),b({0x0b,0,2,8,0}),
                           b({0x0b,0,3,8,0}),b({0x0b,0,4,8,0,1,0xff}),
                           b({0x0b,0,3,8,0,0,0})}) {
        auto t=stimulus(f);reply(t,joined(b({5,0,3,1,0,0}),done),true);
        EXPECT_EQ(evaluate_draft18_response_probe(t,*p),false);
    }
    auto t=stimulus(f);reply(t,b({0x0b,0,3,8,0,0}),true);
    EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
    t=stimulus(f);reply(t,response(f));
    EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
    EXPECT_FALSE(p->definition.response_ready(t));
    // D18 unknown stream count is 2^62-1: nine-byte vi64 ff 3f ff... .
    t=stimulus(f);reply(t,joined(b({5,0,3,1,0,0}),
        b({0x0b,0,11,8,0xff,0x3f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0})),true);
    EXPECT_EQ(evaluate_draft18_response_probe(t,*p),true);
}

TEST(Draft18ResponseObserver, FragmentationWrongStreamsAndActualAcceptedFinAreRequired) {
    const auto profiles=draft18_response_probes();
    for (const auto& f : fixtures()) {
        const auto* p=find(profiles,f.id); ASSERT_NE(p,nullptr); const auto data=response(f);
        const auto good=[&] {auto t=stimulus(f);reply(t,data,true);return t;};
        for (std::size_t split=1;split<data.size();++split) {
            auto t=stimulus(f);
            reply(t,{data.begin(),data.begin()+static_cast<std::ptrdiff_t>(split)});
            reply(t,{data.begin()+static_cast<std::ptrdiff_t>(split),data.end()},true);
            EXPECT_EQ(evaluate_draft18_response_probe(t,*p),true);
        }
        for (unsigned mutation=0;mutation<11;++mutation) {
            auto t=good();
            switch (mutation) {
                case 0: std::get<transport::StreamDataEvent>(t.events.back()).stream_id=5;break;
                case 1: t.writes.back().delivery_event_count.reset();break;
                case 2: --t.writes.back().accepted;break;
                case 3: t.writes.back().fin_accepted=false;break;
                case 4: t.harness_failed=true;break;
                case 5: t.timed_out=true;break;
                case 6: ++*t.delivery_event_count;++*t.writes.back().delivery_event_count;break;
                case 7: std::get<transport::StreamDataEvent>(t.events[2]).stream_id=5;break;
                case 8: std::get<transport::StreamDataEvent>(t.events[2]).data.pop_back();break;
                case 9: t.writes.back().stream_id=5;break;
                case 10: t.writes.back().write.peer_response_ready={};break;
            }
            EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
        }
        auto t=good(); auto ok=std::get<transport::StreamDataEvent>(t.events[2]).data;
        t.events[2]=transport::StreamDataEvent{1,{ok.begin(),ok.begin()+2},false};
        t.events.insert(t.events.begin()+3,transport::StreamDataEvent{1,{ok.begin()+2,ok.end()},false});
        t.delivery_event_count=t.writes.back().delivery_event_count=4;
        EXPECT_EQ(evaluate_draft18_response_probe(t,*p),true);
    }
}

TEST(Draft18ResponseObserver, MissingMalformedAndInvalidUtf8ErrorsRemainNotRun) {
    const auto profiles=draft18_response_probes();
    for (const auto& f : fixtures()) {
        const auto* p=find(profiles,f.id); ASSERT_NE(p,nullptr);
        for (const auto& error : {Bytes{},b({5,0,3,1,0}),b({5,0,4,1,0,1,0xff}),
                                b({5,0,5,1,0,2,0xc0,0x80}),b({5,0,6,1,0,3,0xed,0xa0,0x80}),
                                b({5,0,7,1,0,4,0xf4,0x90,0x80,0x80}),
                                b({5,0,3,0x34,0,0}),b({5,0,7,0x34,0,0,1,'u',0,0})}) {
            auto t=stimulus(f);reply(t,response(f,error),true);
            EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
        }
        if (f.scoped) {
            auto t=stimulus(f);reply(t,b({5,0,7,0x34,0,0,0,0,1,'x'}),true);
            EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
        }
    }
}

TEST(Draft18ResponseObserver, StopSendingAndWrongDirectionResetDoNotProveClosure) {
    const auto profiles=draft18_response_probes();
    for (const auto& f : fixtures()) {
        const auto* p=find(profiles,f.id); ASSERT_NE(p,nullptr);
        auto t=stimulus(f);reply(t,response(f));
        t.events.push_back(transport::PeerStopSendingEvent{1,1});
        t.events.push_back(transport::PeerResetEvent{5,1});
        EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
        EXPECT_FALSE(p->definition.response_ready(t));
        t.events.push_back(transport::PeerResetEvent{1,1});
        EXPECT_EQ(evaluate_draft18_response_probe(t,*p),true);
    }
}

TEST(Draft18ResponseObserver, SessionCloseBeforeDirectionalTerminationCannotProveCleanup) {
    const auto profiles=draft18_response_probes();
    for (const auto& f : fixtures()) {
        const auto* p=find(profiles,f.id); ASSERT_NE(p,nullptr);
        for (bool reset : {false,true}) {
            auto t=stimulus(f);reply(t,response(f));
            t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,0,{}});
            if (reset) t.events.push_back(transport::PeerResetEvent{1,1});
            else reply(t,{},true);
            EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
        }
        auto t=stimulus(f);reply(t,response(f),true);reply(t,b({0}),false);
        EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
    }
}

TEST(Draft18ResponseObserver, EventAndResponseLimitsCannotFabricateSuccessfulCleanup) {
    const auto profiles=draft18_response_probes(); const auto f=fixtures()[0];
    const auto* p=find(profiles,f.id); ASSERT_NE(p,nullptr);
    auto t=stimulus(f); reply(t,response(f),true);
    t.events.resize(4097,transport::ConnectionEstablishedEvent{});
    EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
    t=stimulus(f); auto excessive=response(f);excessive.resize(2*65546+1,std::byte{0});
    reply(t,std::move(excessive),true);
    EXPECT_FALSE(evaluate_draft18_response_probe(t,*p).has_value());
}
}  // namespace
}  // namespace moq::interop::scenarios
