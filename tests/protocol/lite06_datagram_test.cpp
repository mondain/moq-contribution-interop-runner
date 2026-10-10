// The moq-lite-06 datagram scenario (L2b Task B2a): the evaluator of L06-6-4-MUST-NOT-105 against the conforming
// scripted publisher with and without datagrams and against publishers that violate exactly one rule, plus the NotRun
// conditions of the catalog row.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_datagram.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_datagram_size_limit;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
namespace scen = moq::interop::scenarios;
namespace lite06 = moq::interop::scenarios::lite06;

using Verdict = std::optional<bool>;

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 20000ms;
const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";

const Verdict kPass{true};
const Verdict kFail{false};
const Verdict kNotRun{};

ConformingLitePublisherConfig with_datagrams() {
    ConformingLitePublisherConfig config;
    config.datagrams = true;
    config.frames_per_group = 1;
    return config;
}

LiteTranscript run(ConformingLitePublisherConfig config) {
    ConformingLitePublisher publisher(std::move(config));
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    return run_lite_probe(peer, scen::l06_datagram_size_probe(kDeadline, kBroadcast, kTrack), clock, kTick);
}

TEST(Lite06Datagram, APublisherThatKeepsToTheLimitPasses) {
    const auto t = run(with_datagrams());
    ASSERT_TRUE(scen::judgeable_with_stimulus(t)) << t.harness_failure_reason;
    EXPECT_FALSE(t.datagrams.empty());
    EXPECT_EQ(evaluate_l06_datagram_size_limit(t), kPass);
}

TEST(Lite06Datagram, ADatagramAboveTheLimitFails) {
    auto config = with_datagrams();
    config.defect = LiteDefect::DatagramOversize;
    const auto t = run(config);
    ASSERT_FALSE(t.datagrams.empty());
    EXPECT_EQ(evaluate_l06_datagram_size_limit(t), kFail);
}

// The row is about size, not routing: a datagram for another Subscribe ID or one that differs from the stream copy is
// not this row's subject.
TEST(Lite06Datagram, OtherDatagramFaultsAreNotThisRow) {
    for (const auto defect : {LiteDefect::DatagramUnknownSubscribeId, LiteDefect::DatagramDiffersFromStream,
                              LiteDefect::DatagramOnly}) {
        auto config = with_datagrams();
        config.defect = defect;
        EXPECT_EQ(evaluate_l06_datagram_size_limit(run(config)), kPass) << static_cast<int>(defect);
    }
}

TEST(Lite06Datagram, NoDatagramAtAllIsNotRunNeverAPass) {
    ConformingLitePublisherConfig config;  // datagrams off: the moq CLI's behavior
    const auto t = run(config);
    EXPECT_TRUE(t.datagrams.empty());
    EXPECT_EQ(evaluate_l06_datagram_size_limit(t), kNotRun);
}

TEST(Lite06Datagram, OnlyMalformedDatagramsAreNotRun) {
    auto config = with_datagrams();
    config.hooks.on_poll = [](ConformingLitePublisher&, ScriptedLitePeer& peer) {
        if (peer.polls() == 3) peer.datagram(Bytes{std::byte{0x01}, std::byte{0x41}});  // a cut header
    };
    config.groups_per_subscription = 0;
    config.keep_live_group_open = false;
    const auto t = run(config);
    ASSERT_EQ(t.datagrams.size(), 1u);
    EXPECT_EQ(t.datagrams[0].issue, moq::interop::session::kIssueDatagramMalformed);
    EXPECT_EQ(evaluate_l06_datagram_size_limit(t), kNotRun);
}

TEST(Lite06Datagram, AWrongScenarioOrAnUnjudgeableTranscriptIsNotRun) {
    const auto t = run(with_datagrams());
    ASSERT_EQ(evaluate_l06_datagram_size_limit(t), kPass);
    auto other = t;
    other.scenario_id = "l06-subscribe-latest";
    EXPECT_EQ(evaluate_l06_datagram_size_limit(other), kNotRun);
    auto limited = t;
    limited.event_limit_reached = true;
    EXPECT_EQ(evaluate_l06_datagram_size_limit(limited), kNotRun);
    auto no_fixture = t;
    no_fixture.track_name.clear();
    EXPECT_EQ(evaluate_l06_datagram_size_limit(no_fixture), kNotRun);
    auto altered = t;
    for (auto& step : altered.steps) {
        if (step.label == scen::kL06DatagramSubscribeLabel && step.bytes.size() > 3) step.bytes[3] = std::byte{0x7f};
    }
    EXPECT_EQ(evaluate_l06_datagram_size_limit(altered), kNotRun) << "a stimulus that is not the one the scenario builds";
}

TEST(Lite06Datagram, TheBuilderValidatesItsInputs) {
    EXPECT_THROW(scen::l06_datagram_size_probe(kDeadline, "", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_datagram_size_probe(kDeadline, kBroadcast, ""), std::invalid_argument);
    EXPECT_THROW(scen::l06_datagram_size_probe(5000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_datagram_size_probe(kDeadline, kBroadcast, kTrack, 6000ms, 0ms), std::invalid_argument);
    const auto definition = scen::l06_datagram_size_probe(kDeadline, kBroadcast, kTrack);
    EXPECT_TRUE(definition.requires_track);
    EXPECT_EQ(definition.id, scen::kL06DatagramSize);
    EXPECT_EQ(definition.broadcast_path, kBroadcast);
    EXPECT_EQ(definition.track_name, kTrack);
    EXPECT_EQ(definition.steps.back().label, lite06::kAllowanceLabel);
}

}  // namespace
