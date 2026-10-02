#include "moq/interop/scenarios/range_filter.h"
#include <gtest/gtest.h>
#include "../support/raw_probe_transcript.h"
#include <stdexcept>
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include <filesystem>
#include <algorithm>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes bytes(std::initializer_list<unsigned> input) {
    Bytes out;
    for (auto value : input) out.push_back(std::byte(value));
    return out;
}
RawProbeTranscript stimulus(const RangeFilterProbe& p) {
    auto t=test::raw_probe_transcript(p.definition);
    t.setup.delivery_event_count=1;
    if (t.writes.front().write.prepare_bytes) {
        const auto prepared=p.definition.writes.front().prepare_bytes({{},t.events});
        if (!prepared) throw std::logic_error("test preparation");
        t.writes.front().write.bytes=*prepared;
        t.writes.front().accepted=prepared->size();
        t.writes.front().prepared_event_count=t.events.size();
    }
    t.writes.front().delivery_event_count=t.events.size();
    if (t.writes.size()==2) {
        t.events.push_back(transport::StreamDataEvent{1,bytes({4,0,2,0,0}),false});
        t.writes.back().stream_id=1;
        t.writes.back().delivery_event_count=t.events.size();
    }
    t.delivery_event_count=t.events.size();
    return t;
}
TEST(RangeFilterProfiles, ConfiguredTrackRecoveryWorksThroughRealCatalog) {
    using namespace requirements;
    const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source=load_draft_source(21,root/"docs",root/"requirements/draft-digests.json");
    const auto catalog=RequirementCatalog::load(source,root/"requirements/draft21.json");
    const auto state=[&](const std::vector<RawProbeTranscript>& transcripts,const char* row) {
        const auto outcomes=evaluate_draft21_raw_probes(catalog,transcripts);
        const auto found=std::find_if(outcomes.begin(),outcomes.end(),[&](const auto& o){return o.requirement_id==row;});
        return found==outcomes.end()?OutcomeState::NotRun:found->state;
    };
    for (const auto& fixture : std::vector<std::pair<std::vector<Bytes>,Bytes>>{
        {{bytes({'n'})},bytes({'t'})},{{bytes({'a'}),bytes({'b'})},bytes({'v'})}}) {
        const auto configured=draft21_range_filter_probes(std::chrono::milliseconds{1000},fixture.first,fixture.second);
        std::vector<RawProbeTranscript> transcripts;
        for (const auto& p:configured) {
            auto t=stimulus(p);
            t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x36,0,0}),true});
            transcripts.push_back(t);
            std::get<transport::StreamDataEvent>(t.events.back()).data=bytes({5,0,3,0x10,0,0});
            EXPECT_EQ(state({t},p.requirement_id.c_str()),OutcomeState::Fail);
            std::get<transport::StreamDataEvent>(t.events.back()).data=p.definition.writes.size()==2?bytes({7,0,1,0}):bytes({4,0,2,0,0});
            EXPECT_EQ(state({t},p.requirement_id.c_str()),OutcomeState::Fail);
        }
        for (const auto& p:draft21_request_profiles()) {
            if (p.definition.id!="d21-duplicate-range-filter-key-in-request") continue;
            auto t=test::raw_probe_transcript(p.definition);
            t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x36,0,0}),true});
            transcripts.push_back(std::move(t));
        }
        EXPECT_EQ(state(transcripts,"D21-3-3-2-MUST-064"),OutcomeState::Pass);
        EXPECT_EQ(state(transcripts,"D21-3-3-2-MUST-065"),OutcomeState::Pass);
        EXPECT_EQ(state(transcripts,"D21-9-1-6-MUST-315"),OutcomeState::Pass);
        auto missing=transcripts;
        missing.erase(missing.begin()+3);
        EXPECT_EQ(state(missing,"D21-9-1-6-MUST-315"),OutcomeState::NotRun);
        auto duplicate=transcripts;
        duplicate.push_back(transcripts[3]);
        EXPECT_EQ(state(duplicate,"D21-9-1-6-MUST-315"),OutcomeState::NotRun);
        std::get<transport::StreamDataEvent>(missing[3].events.back()).data=bytes({5,0,3,0x10,0,0});
        EXPECT_EQ(state(missing,"D21-9-1-6-MUST-315"),OutcomeState::Fail);
    }
}
TEST(RangeFilterProfiles, RetainedLimitUpdateUsesDistinctKeyAndOmittedDefaultOnly) {
    const auto profiles=draft21_range_filter_probes();
    ASSERT_EQ(profiles.size(),6u);
    EXPECT_EQ(profiles[5].definition.writes[1].bytes,bytes({2,0,7,3,1,0x26,3,1,1,0}));
    EXPECT_TRUE(profiles[4].definition.peer_setup_ready(bytes({0xaf,0,0,0})));
    EXPECT_FALSE(profiles[4].definition.peer_setup_ready(bytes({0xaf,0,0,2,6,0})));
    for(unsigned cap:{1u,2u,16u}) {
        std::vector<transport::TransportEvent> events{transport::StreamDataEvent{2,bytes({0xaf,0,0,2,6,cap}),false}};
        const auto initial=profiles[5].definition.writes[0].prepare_bytes({{},events});
        ASSERT_TRUE(initial);
        Bytes expected=bytes({3,0,8+2*cap,1,0,1,'x',1,0x26,1+2*cap,0});
        for(unsigned i=0;i<cap;++i){expected.push_back(std::byte{1});expected.push_back(std::byte{0});}
        EXPECT_EQ(*initial,expected);
    }
}
TEST(RangeFilterProfiles, RetainedUpdateRequiresActualInitialFiltersAndPriorOk) {
    const auto p=draft21_range_filter_probes()[5];
    auto t=stimulus(p);
    t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x36,0,0,0xb,0,3,8,0,0}),true});
    ASSERT_EQ(evaluate_range_filter_probe(t,p),true);
    for(unsigned mutation=0;mutation<6;++mutation) {
        auto changed=t;
        if(mutation==0) changed.writes[1].write.bytes=bytes({2,0,4,3,1,0x26,0}); // removes initial SetID 0
        if(mutation==1) changed.writes[1].write.bytes[7]=std::byte{0}; // replaces SetID 0
        if(mutation==2) changed.writes[0].write.bytes.back()=std::byte{1}; // alters retained range
        if(mutation==3) std::get<transport::StreamDataEvent>(changed.events[1]).data=bytes({0xaf,0,0,2,6,1});
        if(mutation==4) changed.writes[1].delivery_event_count=2; // OK occurs after claimed update
        if(mutation==5) changed.writes[0].prepared_event_count.reset();
        EXPECT_FALSE(evaluate_range_filter_probe(changed,p).has_value()) << mutation;
    }
    auto explicit_zero=stimulus(draft21_range_filter_probes()[4]);
    std::get<transport::StreamDataEvent>(explicit_zero.events[1]).data=bytes({0xaf,0,0,2,6,0});
    explicit_zero.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x36,0,0}),true});
    EXPECT_FALSE(evaluate_range_filter_probe(explicit_zero,draft21_range_filter_probes()[4]).has_value());
}
TEST(RangeFilterProfiles, TerminalEventsCannotContributeLateErrorBytes) {
    const auto p=draft21_range_filter_probes()[1];
    const std::vector<transport::TransportEvent> endings{
        transport::PeerResetEvent{1,{}},transport::PeerCloseEvent{},
        transport::StreamDataEvent{1,{},true}};
    for (const auto& ending:endings) {
        for (const bool partial : {false,true}) {
            auto t=stimulus(p);
            if(partial)t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3}),false});
            t.events.push_back(ending);
            t.events.push_back(transport::StreamDataEvent{1,partial?bytes({0x36,0,0}):bytes({5,0,3,0x36,0,0}),true});
            EXPECT_FALSE(evaluate_range_filter_probe(t,p).has_value());
        }
        auto t=stimulus(p);
        t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x36,0,0}),false});
        t.events.push_back(ending);
        EXPECT_EQ(evaluate_range_filter_probe(t,p),true);
    }
}
TEST(RangeFilterProfiles, UpdateUsesAcknowledgedRequestStreamAndDuplicateKeys) {
    const auto profiles = draft21_range_filter_probes();
    ASSERT_EQ(profiles.size(), 6u);
    const auto& p = profiles[0];
    ASSERT_EQ(p.definition.writes.size(), 2u);
    EXPECT_EQ(p.definition.writes[0].bytes, bytes({3,0,5,1,0,1,'x',0}));
    EXPECT_FALSE(p.definition.writes[0].fin);
    EXPECT_EQ(p.definition.writes[1].bytes, bytes({2,0,12,3,2,0x26,3,0,1,0,0,3,0,1,0}));
    EXPECT_EQ(p.definition.writes[1].reuse_write_stream, 0u);
    EXPECT_TRUE(p.definition.writes[1].fin);
    EXPECT_FALSE(p.definition.peer_setup_ready(bytes({0xaf,0,0,2,6,1})));
    EXPECT_TRUE(p.definition.peer_setup_ready(bytes({0xaf,0,0,2,6,2})));
    EXPECT_FALSE(p.definition.writes[1].peer_response_ready(bytes({4,0,1,0})));
}
TEST(RangeFilterProfiles, LiteralRangeCountsFollowActualPeerCapacity) {
    const auto profiles = draft21_range_filter_probes();
    const auto& p = profiles[1];
    for (const unsigned cap : {1u,2u,16u}) {
        const std::vector<transport::TransportEvent> events{
            transport::StreamDataEvent{2,bytes({0xaf,0,0,2,6,cap}),false}};
        const auto actual = p.definition.writes[0].prepare_bytes({{},events});
        ASSERT_TRUE(actual);
        Bytes expected = bytes({3,0,static_cast<unsigned>(13+2*cap),1,0,1,'x',2,0x26,1+2*cap,0});
        for (unsigned i=0;i<cap;++i) { expected.push_back(std::byte{1}); expected.push_back(std::byte{0}); }
        const auto tail=bytes({0,3,1,1,0});
        expected.insert(expected.end(),tail.begin(),tail.end());
        EXPECT_EQ(*actual,expected);
    }
}
TEST(RangeFilterProfiles, MissingCapacityMeansZeroAndFragmentedSetupWorks) {
    const auto profiles = draft21_range_filter_probes();
    const auto& zero=profiles[2];
    EXPECT_TRUE(zero.definition.peer_setup_ready(bytes({0xaf,0,0,0})));
    EXPECT_TRUE(zero.definition.peer_setup_ready(bytes({0xaf,0,0,2,6,0})));
    EXPECT_FALSE(profiles[1].definition.peer_setup_ready(bytes({0xaf,0,0,0})));
    const std::vector<transport::TransportEvent> events{
        transport::StreamDataEvent{2,bytes({0xaf,0}),false},
        transport::StreamDataEvent{2,bytes({0,0}),false}};
    const auto actual=zero.definition.writes[0].prepare_bytes({{},events});
    ASSERT_TRUE(actual);
    EXPECT_EQ(*actual,bytes({3,0,10,1,0,1,'x',1,0x26,3,0,1,0}));
}
TEST(RangeFilterProfiles, RejectionRequiresPreparedActualStimulusAndTypedResponse) {
    const auto profiles=draft21_range_filter_probes();
    const auto& p=profiles[1];
    auto t=test::raw_probe_transcript(p.definition);
    t.setup.delivery_event_count=1;
    const auto prepared=p.definition.writes[0].prepare_bytes({{},t.events});
    ASSERT_TRUE(prepared);
    t.writes[0].write.bytes=*prepared;
    t.writes[0].accepted=prepared->size();
    t.writes[0].delivery_event_count=t.events.size();
    t.writes[0].prepared_event_count=t.events.size();
    t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x36,0,0}),true});
    EXPECT_EQ(evaluate_range_filter_probe(t,p),true);
    std::get<transport::StreamDataEvent>(t.events.back()).data=bytes({5,0,3,0x10,0,0});
    EXPECT_EQ(evaluate_range_filter_probe(t,p),false);
    std::get<transport::StreamDataEvent>(t.events.back()).data=bytes({5,0,3,0x36});
    EXPECT_FALSE(evaluate_range_filter_probe(t,p).has_value());
    std::get<transport::StreamDataEvent>(t.events.back()).data=bytes({5,0,3,0x36,0,0});
    t.writes[0].prepared_event_count=1;
    EXPECT_FALSE(evaluate_range_filter_probe(t,p).has_value());
    t.writes[0].prepared_event_count.reset();
    EXPECT_FALSE(evaluate_range_filter_probe(t,p).has_value());
}
TEST(RangeFilterProfiles, UpdateExcludesInitialOkAndRequiresActualStreamReuse) {
    const auto profiles=draft21_range_filter_probes();
    const auto& p=profiles[0];
    auto t=test::raw_probe_transcript(p.definition);
    t.writes[0].delivery_event_count=t.events.size();
    t.events.push_back(transport::StreamDataEvent{1,bytes({4,0,2,0,0}),false});
    t.writes[1].stream_id=1;
    t.writes[1].delivery_event_count=t.events.size();
    t.delivery_event_count=t.events.size();
    EXPECT_FALSE(evaluate_range_filter_probe(t,p).has_value());
    t.events.push_back(transport::StreamDataEvent{1,bytes({5,0,3,0x36,0,0,0xb,0,3,8,0,0}),true});
    EXPECT_EQ(evaluate_range_filter_probe(t,p),true);
    std::get<transport::StreamDataEvent>(t.events.back()).data=bytes({7,0,1,0});
    EXPECT_EQ(evaluate_range_filter_probe(t,p),false);
    t.writes[1].stream_id=5;
    EXPECT_FALSE(evaluate_range_filter_probe(t,p).has_value());
}
TEST(RangeFilterProfiles, HugeCapacityAndAmbiguousSetupAreNotRunnable) {
    const auto profiles=draft21_range_filter_probes();
    const std::vector<transport::TransportEvent> events{
        transport::StreamDataEvent{2,bytes({0xaf,0,0,2,6,2}),false},
        transport::StreamDataEvent{6,bytes({0xaf,0,0,2,6,2}),false}};
    EXPECT_FALSE(profiles[1].definition.writes[0].prepare_bytes({{},events}));
    wire::ByteWriter writer(64);
    ASSERT_FALSE(wire::draft21::encode_setup({{{6,std::uint64_t{32761}}}},writer));
    EXPECT_FALSE(profiles[1].definition.peer_setup_ready(writer.bytes()));
    EXPECT_THROW(draft21_range_filter_probes(std::chrono::milliseconds{1},{},{ }), std::invalid_argument);
}
} // namespace
} // namespace moq::interop::scenarios
