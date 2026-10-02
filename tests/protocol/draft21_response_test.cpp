#include "moq/interop/scenarios/draft21_response.h"
#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/wire/draft21/publish_done.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <map>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
struct Fixture {
    const char* id;
    const char* requirement;
    const char* evaluator;
    Draft21ResponseExpectation expectation;
    bool scoped;
    Bytes first;
    Bytes initial_ok;
};
std::vector<Fixture> fixtures() {
    // Draft21 sections9.5,9.5.1: literal counted parameters are not KVs.
    return {
        {"d21-subscriber-update-on-publish", "D21-9-5-MUST-344",
         "d21-request-update-context-and-direction",
         Draft21ResponseExpectation::PermittedPublishUpdate, false, b({7,0,1,0}), {}},
        {"d21-failed-subscription-update-cleanup", "D21-9-5-1-MUST-346",
         "d21-failed-update-publish-done-update-failed",
         Draft21ResponseExpectation::FailedSubscriptionCleanup, false,
         b({3,0,5,1,0,1,'x',0}), b({4,0,4,0,0,4,1})},
        {"d21-failed-subscribe-namespace-update-close", "D21-9-5-1-MUST-348",
         "d21-failed-namespace-update-stream-close",
         Draft21ResponseExpectation::FailedDiscoveryCleanup, true,
         b({0x50,0,3,1,0,0}), b({7,0,1,0})},
        {"d21-failed-subscribe-tracks-update-close", "D21-9-5-1-MUST-349",
         "d21-failed-subscribe-tracks-update-stream-close",
         Draft21ResponseExpectation::FailedDiscoveryCleanup, true,
         b({0x51,0,3,1,0,0}), b({7,0,1,0})},
    };
}
Bytes publish() { return b({0x1d,0,10,0,1,1,'n',1,'x',0,0,4,1}); }
const Draft21ResponseProbe* find(const std::vector<Draft21ResponseProbe>& probes, const char* id) {
    const auto it=std::find_if(probes.begin(),probes.end(),[&](const auto& p){return p.definition.id==id;});
    return it==probes.end()?nullptr:&*it;
}
bool permitted(const Fixture& f) { return f.expectation==Draft21ResponseExpectation::PermittedPublishUpdate; }
RawProbeTranscript stimulus(const Fixture& f) {
    RawProbeTranscript t;
    t.scenario_id=f.id;
    t.setup={{RawProbeChannel::NewUni,b({0xaf,0,0,0}),false},3,4,false};
    t.complete=t.stimulus_delivered=t.transport_established=t.peer_setup_received=true;
    t.delivery_event_count=3;
    t.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false},
        transport::StreamDataEvent{permitted(f)?0u:1u,permitted(f)?publish():f.initial_ok,false}};
    const auto channel=permitted(f)?RawProbeChannel::PeerBidi:RawProbeChannel::NewBidi;
    const auto stream=permitted(f)?0u:1u;
    t.writes.push_back({{channel,f.first,false},stream,f.first.size(),false,permitted(f)?3u:2u});
    RawProbeWrite update{channel,permitted(f)?b({2,0,2,1,0}):b({2,0,6,3,1,3,2,2,0}),!permitted(f),0,{}};
    if (!permitted(f)) update.peer_response_ready=[](auto){return true;};
    t.writes.push_back({update,stream,update.bytes.size(),update.fin,3});
    return t;
}
void reply(RawProbeTranscript& t, Bytes data, bool fin=false) {
    t.events.push_back(transport::StreamDataEvent{*t.writes.back().stream_id,std::move(data),fin});
}
Bytes joined(Bytes first, const Bytes& second) { first.insert(first.end(),second.begin(),second.end());return first; }

TEST(Draft21ResponseProfiles, DefinesFourLiteralWireOperationsWithRealEstablishmentGates) {
    const auto profiles=draft21_response_probes(std::chrono::milliseconds{71});
    ASSERT_EQ(profiles.size(),4u);
    for (const auto& f:fixtures()) {
        SCOPED_TRACE(f.id);
        const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
        EXPECT_EQ(p->requirement_id,f.requirement);EXPECT_EQ(p->evaluator_id,f.evaluator);
        EXPECT_EQ(p->expectation,f.expectation);EXPECT_EQ(p->namespace_scoped,f.scoped);
        EXPECT_EQ(p->definition.deadline,std::chrono::milliseconds{71});
        ASSERT_TRUE(p->definition.response_ready);
        ASSERT_EQ(p->definition.writes.size(),2u);
        EXPECT_EQ(p->definition.setup_bytes,b({0xaf,0,0,0}));
        EXPECT_EQ(p->definition.writes[0].bytes,f.first);
        EXPECT_EQ(p->definition.writes[1].bytes,permitted(f)?b({2,0,2,1,0}):b({2,0,6,3,1,3,2,2,0}));
        EXPECT_EQ(p->definition.writes[1].fin,!permitted(f));
        EXPECT_EQ(p->definition.writes[1].reuse_write_stream,0u);
        if(permitted(f)) {
            ASSERT_TRUE(p->definition.peer_request_ready);
            EXPECT_TRUE(p->definition.peer_request_ready(publish()));
            const auto opener=publish();
            for(std::size_t n=0;n<opener.size();++n)
                EXPECT_FALSE(p->definition.peer_request_ready(std::span{opener}.first(n)));
            auto wrong=publish();wrong[3]=std::byte{1};EXPECT_FALSE(p->definition.peer_request_ready(wrong));
        } else {
            ASSERT_TRUE(p->definition.writes[1].peer_response_ready);
            EXPECT_TRUE(p->definition.writes[1].peer_response_ready(f.initial_ok));
            EXPECT_FALSE(p->definition.writes[1].peer_response_ready(b({5,0,3,1,0,0})));
        }
    }
}

TEST(Draft21ResponseObserver, PermittedPublishUpdateAcceptsTypedSuccessOrErrorAndProtocolCloseFails) {
    const auto profiles=draft21_response_probes();const auto f=fixtures().front();
    const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
    const Bytes valid[]{b({7,0,1,0}), b({7,0,6,2,8,10,1,0,0}), b({5,0,3,1,0,0}),
        joined(b({5,0,3,1,0,0}),b({0x0b,0,3,8,0,0}))};
    for(const auto& data:valid) {
        auto t=stimulus(f);reply(t,data);
        EXPECT_TRUE(p->definition.response_ready(t));
        EXPECT_EQ(evaluate_draft21_response_probe(t,*p),true);
        t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
        EXPECT_EQ(evaluate_draft21_response_probe(t,*p),false);
    }
    for(const auto code:{3u,4u}) {
        auto t=stimulus(f);t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,code,{}});
        const auto result=evaluate_draft21_response_probe(t,*p);
        if(code==3) EXPECT_EQ(result,false);else EXPECT_FALSE(result.has_value());
    }
    const Bytes malformed[]{b({7,0,3,0,4,1}),b({5,0,4,1,0,1,0xff}),b({5,0,3,0x34,0,0}),b({7,0,2,1,8})};
    for(const auto& data:malformed) {auto t=stimulus(f);reply(t,data,true);EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());}
}

TEST(Draft21ResponseObserver, FailedCleanupUsesAnyValidRejectionAndDirectionalFinOrReset) {
    const auto profiles=draft21_response_probes();
    // Section12.3 codes are advisory. UINT64_MAX is an opaque error code,
    // not a guessed UNKNOWN_AUTH_TOKEN_ALIAS assignment.
    const Bytes errors[]{b({5,0,3,1,0,0}),b({5,0,3,0x17,0,0}),
        b({5,0,11,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0,0}),
        b({5,0,6,1,0,3,0xe2,0x82,0xac}),b({5,0,6,0x34,0,0,0,0,0})};
    for(const auto& f:fixtures()) {
        if(permitted(f))continue;
        const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
        for(const auto& error:errors) for(bool reset:{false,true}) {
            auto t=stimulus(f);auto data=error;
            if(f.expectation==Draft21ResponseExpectation::FailedSubscriptionCleanup)
                data=joined(data,b({0x0b,0,3,8,0,0}));
            reply(t,data,!reset);
            if(reset)t.events.push_back(transport::PeerResetEvent{1,1});
            EXPECT_TRUE(p->definition.response_ready(t));
            EXPECT_EQ(evaluate_draft21_response_probe(t,*p),true);
        }
        auto pending=stimulus(f);reply(pending,b({5,0,3,1,0,0}));
        pending.events.push_back(transport::PeerStopSendingEvent{1,1});
        EXPECT_FALSE(evaluate_draft21_response_probe(pending,*p).has_value());
        EXPECT_FALSE(p->definition.response_ready(pending));
        pending=stimulus(f);reply(pending,b({7,0,1,0}),true);
        EXPECT_FALSE(evaluate_draft21_response_probe(pending,*p).has_value());
        pending=stimulus(f);reply(pending,b({5,0,3,1,0}),true);
        EXPECT_FALSE(evaluate_draft21_response_probe(pending,*p).has_value());
    }
}

TEST(Draft21ResponseObserver, SubscriptionRequiresAdditionalValidUpdateFailedBeforeTermination) {
    const auto profiles=draft21_response_probes();const auto f=fixtures()[1];
    const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
    const Bytes bad_done[]{ {},b({0x0b,0,3,1,0,0}),b({0x0b,0,2,8,0}),
        b({0x0b,0,3,8,0}),b({0x0b,0,4,8,0,1,0xff}),b({0x0b,0,3,8,0,0,0})};
    for(const auto& done:bad_done) {
        auto t=stimulus(f);reply(t,joined(b({5,0,3,1,0,0}),done),true);
        EXPECT_TRUE(p->definition.response_ready(t));
        EXPECT_EQ(evaluate_draft21_response_probe(t,*p),false);
    }
    auto t=stimulus(f);reply(t,b({0x0b,0,3,8,0,0}),true);
    EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());
    t=stimulus(f);reply(t,joined(b({5,0,3,1,0,0}),b({0x0b,0,3,8,0,0})));
    EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());
    // PUBLISH_DONE stream accounting is separate: structurally valid unknown
    // StreamCount is not fabricated as a measured number of data streams.
    t=stimulus(f);reply(t,joined(b({5,0,3,1,0,0}),
        b({0x0b,0,11,8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0})),true);
    EXPECT_EQ(evaluate_draft21_response_probe(t,*p),true);
}

TEST(Draft21ResponseObserver, FragmentationWrongStreamsAndFinalDeliveryChronologyAreProven) {
    const auto profiles=draft21_response_probes();
    for(const auto& f:fixtures()) {
        const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
        auto final=permitted(f)?b({7,0,3,1,8,10}):b({5,0,3,1,0,0});
        if(f.expectation==Draft21ResponseExpectation::FailedSubscriptionCleanup)
            final=joined(final,b({0x0b,0,3,8,0,0}));
        const auto valid=[&] {auto t=stimulus(f);reply(t,final,!permitted(f));return t;};
        EXPECT_EQ(evaluate_draft21_response_probe(valid(),*p),true);
        for(std::size_t split=1;split<final.size();++split) {
            auto t=stimulus(f);reply(t,{final.begin(),final.begin()+static_cast<std::ptrdiff_t>(split)});
            reply(t,{final.begin()+static_cast<std::ptrdiff_t>(split),final.end()},!permitted(f));
            EXPECT_EQ(evaluate_draft21_response_probe(t,*p),true);
        }
        for(unsigned mutation=0;mutation<8;++mutation) {
            auto t=valid();
            switch(mutation) {
                case 0: std::get<transport::StreamDataEvent>(t.events.back()).stream_id+=4;break;
                case 1: t.writes.back().delivery_event_count.reset();break;
                case 2: --t.writes.back().accepted;break;
                case 3: t.harness_failed=true;break;
                case 4: t.timed_out=true;break;
                case 5: ++*t.delivery_event_count;++*t.writes.back().delivery_event_count;break;
                case 6: if(permitted(f))std::get<transport::StreamDataEvent>(t.events[2]).data.pop_back();
                        else std::get<transport::StreamDataEvent>(t.events[2]).stream_id+=4;break;
                case 7: if(permitted(f))t.writes.back().stream_id=4;else t.writes.back().fin_accepted=false;break;
            }
            EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());
        }
    }
}

TEST(Draft21SuccessfulResponse, NamespaceContextForbidsPropertiesAndLargestObject) {
    using namespace wire::draft21;
    const auto legal=b({7,0,3,1,8,10});wire::Cursor cursor(legal,20);
    EXPECT_TRUE(std::holds_alternative<SuccessfulResponse>(decode_successful_response(cursor,ResponseContext::SubscribeNamespace)));
    EXPECT_EQ(cursor.offset(),26u);
    for(const auto& input:{b({7,0,3,0,4,1}),b({7,0,4,1,9,0,0})}) {
        wire::Cursor invalid(input,20);const auto result=decode_successful_response(invalid,ResponseContext::SubscribeNamespace);
        EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(result));EXPECT_EQ(invalid.offset(),20u);
    }
}

TEST(Draft21PublishDone, DecodesStatusCountReasonWithAtomicFramingAndAbsoluteOffsets) {
    const auto input=b({0x0b,0,6,8,0,3,0xe2,0x82,0xac});
    wire::Cursor cursor(input,20);const auto decoded=wire::draft21::decode_publish_done(cursor);
    ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishDoneMessage>(decoded));
    const auto& message=std::get<wire::draft21::PublishDoneMessage>(decoded);
    EXPECT_EQ(message.status_code,8u);EXPECT_EQ(message.stream_count,0u);
    EXPECT_EQ(message.reason,b({0xe2,0x82,0xac}));EXPECT_EQ(cursor.offset(),29u);
    for(std::size_t n=0;n<input.size();++n) {
        wire::Cursor incomplete(std::span{input}.first(n),20);
        EXPECT_TRUE(std::holds_alternative<wire::NeedMore>(wire::draft21::decode_publish_done(incomplete)));
        EXPECT_EQ(incomplete.offset(),20u);
    }
    for(const auto& malformed:{b({0x0b,0,2,8,0}),b({0x0b,0,4,8,0,1,0xff})}) {
        wire::Cursor invalid(malformed,20);const auto result=wire::draft21::decode_publish_done(invalid);
        ASSERT_TRUE(std::holds_alternative<wire::DecodeError>(result));
        EXPECT_GE(std::get<wire::DecodeError>(result).offset,23u);EXPECT_EQ(invalid.offset(),20u);
    }
    auto oversized=b({0x0b,4,5,8,0,0x84,1});oversized.insert(oversized.end(),1025,std::byte{'x'});
    wire::Cursor invalid(oversized);EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(wire::draft21::decode_publish_done(invalid)));
    EXPECT_TRUE(wire::draft21::valid_reason_phrase({}));
    EXPECT_TRUE(wire::draft21::valid_reason_phrase(b({0xf0,0x9f,0x8c,0x8d})));
    for(const auto& utf8:{b({0xc0,0x80}),b({0xed,0xa0,0x80}),b({0xf4,0x90,0x80,0x80}),b({0xe2,0x82})})
        EXPECT_FALSE(wire::draft21::valid_reason_phrase(utf8));
}

TEST(Draft21PeerClose, ResponderUpdateOnPublishNamespaceUsesSameActualPeerStream) {
    const auto profiles=draft21_peer_close_probes();
    const auto it=std::find_if(profiles.begin(),profiles.end(),[](const auto& p){return p.definition.id=="d21-responder-update-on-publish-namespace";});
    ASSERT_NE(it,profiles.end());EXPECT_EQ(it->requirement_id,"D21-9-5-MUST-344");
    EXPECT_EQ(it->evaluator_id,"d21-request-update-context-and-direction");EXPECT_EQ(it->expected_close,3u);
    ASSERT_EQ(it->definition.writes.size(),2u);EXPECT_EQ(it->definition.writes[0].bytes,b({7,0,1,0}));
    EXPECT_EQ(it->definition.writes[1].bytes,b({2,0,2,1,0}));EXPECT_EQ(it->definition.writes[1].reuse_write_stream,0u);
    EXPECT_FALSE(it->definition.writes[1].fin);EXPECT_FALSE(it->definition.writes[1].peer_response_ready);
    EXPECT_TRUE(it->definition.peer_request_ready(b({6,0,5,0,1,1,'n',0})));
}
TEST(Draft21ResponseObserver, SessionCloseCannotBeReplacedByALaterResetAsCleanupProof) {
    const auto profiles=draft21_response_probes();
    for(const auto& f:fixtures()) {
        if(permitted(f))continue;
        const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
        auto t=stimulus(f);auto data=b({5,0,3,1,0,0});
        if(f.expectation==Draft21ResponseExpectation::FailedSubscriptionCleanup)
            data=joined(data,b({0x0b,0,3,8,0,0}));
        reply(t,data);
        t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,0,{}});
        t.events.push_back(transport::PeerResetEvent{1,1});
        EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());
    }
}

TEST(Draft21ResponseProfiles, PublishOpenerRequiresLegalPropertiesAndLocationFilter) {
    const auto profiles=draft21_response_probes();const auto* p=find(profiles,fixtures().front().id);
    ASSERT_NE(p,nullptr);const auto& ready=p->definition.peer_request_ready;
    // Sections9.8/9.20 and10: properties are optional, and legal FORWARD
    // and NextObject LocationFilter must not narrow the context to one fixture.
    EXPECT_TRUE(ready(b({0x1d,0,8,0,1,1,'n',1,'x',0,0})));
    EXPECT_TRUE(ready(b({0x1d,0,12,0,1,1,'n',1,'x',0,1,0x10,1,4,1})));
    EXPECT_TRUE(ready(b({0x1d,0,14,0,1,1,'n',1,'x',0,1,0x21,2,0,0,4,1})));
    for(const auto& invalid:{b({0x1d,0,10,0,1,1,'n',1,'x',0,0,0x22,0}),
                            b({0x1d,0,10,0,1,1,'n',1,'x',0,0,0x30,2}),
                            b({0x1d,0,12,0,1,1,'n',1,'x',0,0,0xc0,0x40,0,0}),
                            b({0x1d,0,23,0,1,1,'n',1,'x',0,1,0x21,11,
                               0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0,1,4,1})})
        EXPECT_FALSE(ready(invalid));
}

TEST(Draft21ResponseObserver, MalformedUtf8AndWrongDirectionCannotEstablishFailedCleanup) {
    const auto profiles=draft21_response_probes();
    for(const auto& f:fixtures()) {
        if(permitted(f))continue;
        const auto* p=find(profiles,f.id);ASSERT_NE(p,nullptr);
        auto t=stimulus(f);reply(t,joined(b({5,0,4,1,0,1,0xff}),b({0x0b,0,3,8,0,0})),true);
        EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());
        t=stimulus(f);auto error=b({5,0,3,1,0,0});
        if(f.expectation==Draft21ResponseExpectation::FailedSubscriptionCleanup)
            error=joined(error,b({0x0b,0,3,8,0,0}));
        reply(t,error);t.events.push_back(transport::PeerResetEvent{5,1});
        EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());
        if(f.scoped) {
            t=stimulus(f);reply(t,b({5,0,7,0x34,0,0,0,0,1,'x'}),true);
            EXPECT_FALSE(evaluate_draft21_response_probe(t,*p).has_value());
        }
    }
}

TEST(Draft21ResponseProfiles, NamespaceGateAcceptsCoalescedValidNotifications) {
    const auto profiles = draft21_response_probes();
    const auto fixture = fixtures()[2];
    const auto* profile = find(profiles, fixture.id);
    ASSERT_NE(profile, nullptr);
    const auto& ready = profile->definition.writes[1].peer_response_ready;
    const auto ok = b({7, 0, 1, 0});
    const auto announcement = b({8, 0, 3, 1, 1, 'n'});
    const auto withdrawal = b({0x0e, 0, 3, 1, 1, 'n'});
    const auto coalesced = joined(ok, announcement);
    EXPECT_TRUE(ready(coalesced));
    EXPECT_TRUE(ready(joined(coalesced, withdrawal)));
    auto transcript = stimulus(fixture);
    std::get<transport::StreamDataEvent>(transcript.events[2]).data = coalesced;
    reply(transcript, b({5, 0, 3, 1, 0, 0}), true);
    EXPECT_EQ(evaluate_draft21_response_probe(transcript, *profile), true);
    for (const auto& invalid : {joined(ok, withdrawal), joined(ok, b({8, 0, 2, 1, 0})),
                               joined(ok, b({8, 0, 1, 33})), joined(ok, ok),
                               joined(ok, b({5, 0, 3, 1, 0, 0})),
                               joined(ok, b({8, 0, 3, 1, 1}))}) {
        EXPECT_FALSE(ready(invalid));
    }
    // Compare suffix fields, so equivalent legal varint encodings match.
    EXPECT_TRUE(ready(joined(coalesced, b({0x0e, 0, 4, 0x80, 1, 1, 'n'}))));
    EXPECT_FALSE(ready(joined(coalesced, b({0x0e, 0, 3, 1, 1, 'x'}))));
}

}  // namespace
}  // namespace moq::interop::scenarios
