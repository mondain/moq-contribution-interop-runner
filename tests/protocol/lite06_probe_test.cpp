// The moq-lite-06 probe scenario (L2a Task B3): both evaluators against the conforming scripted publisher at each Probe
// level and against publishers that violate exactly one rule, plus the NotRun conditions of the catalog rows
// (L06-5-1-5-MUST-072, L06-5-1-5-MUST-075).

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_probe.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_probe_none_reset;
using moq::interop::scenarios::evaluate_l06_probe_target_continues;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
namespace scen = moq::interop::scenarios;
namespace lite06 = moq::interop::scenarios::lite06;
namespace wire = moq::interop::wire;
namespace session = moq::interop::session;

using Verdict = std::optional<bool>;

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 20000ms;

const Verdict kPass{true};
const Verdict kFail{false};
const Verdict kNotRun{};

struct Verdicts {
    Verdict continues;  // row 072
    Verdict none_reset; // row 075
    bool operator==(const Verdicts&) const = default;
};

Verdicts judge(const LiteTranscript& t) {
    return {evaluate_l06_probe_target_continues(t), evaluate_l06_probe_none_reset(t)};
}

// A conforming publisher advertising `level` (nullopt: no Probe parameter at all).
ConformingLitePublisherConfig config_at(std::optional<std::uint64_t> level) {
    ConformingLitePublisherConfig config;
    if (level) {
        wire::ByteWriter value(8);
        EXPECT_TRUE(l06::write_varint(*level, value));
        config.setup_parameters.push_back(
            l06::SetupParameter{l06::kParamProbe, Bytes(value.bytes().begin(), value.bytes().end())});
    }
    return config;
}

LiteTranscript run_probe(ConformingLitePublisherConfig config) {
    ConformingLitePublisher publisher(std::move(config));
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    return run_lite_probe(peer, scen::l06_probe_report_probe(kDeadline), clock, kTick);
}

const session::LiteStreamRecord* probe_stream(const LiteTranscript& t) {
    for (const auto& record : t.streams) {
        if (record.kind == session::LiteStreamKind::Probe) return &record;
    }
    return nullptr;
}

TEST(Lite06Probe, AReportLevelPublisherKeepsReportingAfterBothTargets) {
    for (const std::uint64_t level : {1, 2}) {
        const auto t = run_probe(config_at(level));
        ASSERT_TRUE(scen::judgeable_with_stimulus(t)) << level;
        EXPECT_EQ(judge(t), (Verdicts{kPass, kNotRun})) << level;
        ASSERT_NE(lite06::step_labelled(t, scen::kL06ProbeSecondLabel), nullptr) << "the second target was sent";
        const auto* record = probe_stream(t);
        ASSERT_NE(record, nullptr);
        EXPECT_GE(session::peer_probe_reports(*record).size(), 2u);
        EXPECT_FALSE(record->reset_seen);
    }
}

TEST(Lite06Probe, ALevelNonePublisherResetsTheStreamAndPassesRow075) {
    for (const auto level : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0}}) {
        const auto t = run_probe(config_at(level));
        ASSERT_TRUE(scen::judgeable_with_stimulus(t));
        EXPECT_EQ(judge(t), (Verdicts{kNotRun, kPass}));
        const auto* record = probe_stream(t);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(record->reset_seen);
        EXPECT_TRUE(session::peer_probe_reports(*record).empty());
        // The reset ended the stream before the second target: the runner never wrote to a reset stream.
        EXPECT_EQ(lite06::step_labelled(t, scen::kL06ProbeSecondLabel), nullptr);
    }
}

TEST(Lite06Probe, AResetOnATargetAtReportLevelFailsRow072) {
    auto config = config_at(1);
    config.defect = LiteDefect::ProbeResetsOnTarget;
    const auto t = run_probe(config);
    EXPECT_EQ(judge(t), (Verdicts{kFail, kNotRun}));
    const auto* record = probe_stream(t);
    ASSERT_NE(record, nullptr);
    ASSERT_TRUE(record->reset_code.has_value());
    EXPECT_EQ(*record->reset_code, 0x12u);
}

TEST(Lite06Probe, ReportsAtLevelNoneFailRow075) {
    auto config = config_at(0);
    config.defect = LiteDefect::ProbeNoneNotReset;
    EXPECT_EQ(judge(run_probe(config)), (Verdicts{kNotRun, kFail}));
}

TEST(Lite06Probe, AResetAfterReportsAtLevelNoneStillFailsRow075) {
    auto config = config_at(0);
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (request.stream_type != 0x4 || !request.bidirectional) return false;
        peer.data(request.stream, probe_message({5000000, 25}));
        peer.peer_reset(request.stream, 0x0);
        return true;
    };
    EXPECT_EQ(judge(run_probe(config)), (Verdicts{kNotRun, kFail}));
}

TEST(Lite06Probe, AFinInsteadOfAResetAtLevelNoneFailsRow075) {
    auto config = config_at(0);
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (request.stream_type != 0x4 || !request.bidirectional) return false;
        peer.fin(request.stream);
        return true;
    };
    EXPECT_EQ(judge(run_probe(config)), (Verdicts{kNotRun, kFail}));
}

TEST(Lite06Probe, SilenceAtLevelNoneIsNotRun) {
    auto config = config_at(0);
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer&, const LiteRunnerRequest& request) {
        return request.stream_type == 0x4 && request.bidirectional;
    };
    EXPECT_EQ(judge(run_probe(config)), (Verdicts{kNotRun, kNotRun}));
}

TEST(Lite06Probe, NoReportAfterTheSecondTargetIsNotRun) {
    auto config = config_at(1);
    config.probe_report_interval_polls = 1'000'000;  // the immediate report only
    EXPECT_EQ(judge(run_probe(config)), (Verdicts{kNotRun, kNotRun}));
}

TEST(Lite06Probe, ASessionCloseAfterTheTargetsFailsRow072) {
    auto config = config_at(1);
    config.hooks.on_poll = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        if (!peer.peer_closed()) {
            const auto* stream = peer.runner_stream(1);
            // The second target is on the stream once its bytes exceed the first message.
            if (stream != nullptr && stream->bytes.size() > scen::l06_probe_open_bytes(scen::kL06ProbeFirstTarget).size()) {
                publisher.close(peer, 0x1);
            }
        }
    };
    EXPECT_EQ(evaluate_l06_probe_target_continues(run_probe(config)), kFail);
}

TEST(Lite06Probe, ANonSetupOrMalformedProbeParameterIsNotRun) {
    // No SETUP at all: the level is unknown, never read as None.
    auto no_setup = config_at(1);
    no_setup.defect = LiteDefect::NoSetupStream;
    EXPECT_EQ(judge(run_probe(no_setup)), (Verdicts{kNotRun, kNotRun}));
    // A Probe parameter that is not one varint.
    auto malformed = config_at(std::nullopt);
    malformed.setup_parameters.push_back(l06::SetupParameter{l06::kParamProbe, Bytes{std::byte{1}, std::byte{1}}});
    EXPECT_EQ(judge(run_probe(malformed)), (Verdicts{kNotRun, kNotRun}));
}

TEST(Lite06Probe, TheLevelIsReadFromTheDecodedSetup) {
    EXPECT_EQ(scen::l06_publisher_probe_level(run_probe(config_at(std::nullopt))), std::optional<std::uint64_t>(0));
    EXPECT_EQ(scen::l06_publisher_probe_level(run_probe(config_at(0))), std::optional<std::uint64_t>(0));
    EXPECT_EQ(scen::l06_publisher_probe_level(run_probe(config_at(1))), std::optional<std::uint64_t>(1));
    EXPECT_EQ(scen::l06_publisher_probe_level(run_probe(config_at(2))), std::optional<std::uint64_t>(2));
}

TEST(Lite06Probe, AWrongScenarioOrAnUnjudgeableTranscriptIsNotRun) {
    const auto t = run_probe(config_at(1));
    ASSERT_EQ(judge(t), (Verdicts{kPass, kNotRun}));
    auto other = t;
    other.scenario_id = "l06-subscribe-latest";
    EXPECT_EQ(judge(other), (Verdicts{kNotRun, kNotRun}));
    auto limited = t;
    limited.event_limit_reached = true;
    EXPECT_EQ(judge(limited), (Verdicts{kNotRun, kNotRun}));
    auto altered = t;
    for (auto& step : altered.steps) {
        if (step.label == scen::kL06ProbeOpenLabel && step.bytes.size() > 3) step.bytes[3] = std::byte{0x7f};
    }
    EXPECT_EQ(judge(altered), (Verdicts{kNotRun, kNotRun})) << "a stimulus that is not the one the scenario builds";
}

TEST(Lite06Probe, TheStimuliAreExactlyWhatTheBuilderSent) {
    const auto t = run_probe(config_at(1));
    const auto* open = lite06::step_labelled(t, scen::kL06ProbeOpenLabel);
    const auto* second = lite06::step_labelled(t, scen::kL06ProbeSecondLabel);
    ASSERT_TRUE(open != nullptr && second != nullptr);
    EXPECT_EQ(open->bytes, scen::l06_probe_open_bytes(scen::kL06ProbeFirstTarget));
    EXPECT_EQ(second->bytes, scen::l06_probe_message_bytes(scen::kL06ProbeSecondTarget));
    // STREAM_TYPE 0x4, then PROBE{4 000 000 bps = 0x3d0900 -> 4-byte form, rtt 0}: 04 | 05 | 80 3d 09 00 | 00.
    EXPECT_EQ(scen::l06_probe_open_bytes(4'000'000),
              (Bytes{std::byte{0x04}, std::byte{0x05}, std::byte{0x80}, std::byte{0x3d}, std::byte{0x09},
                     std::byte{0x00}, std::byte{0x00}}));
}

TEST(Lite06Probe, TheBuilderValidatesItsInputs) {
    EXPECT_THROW(scen::l06_probe_report_probe(1000ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_probe_report_probe(kDeadline, 3000ms, 0ms), std::invalid_argument);
    const auto definition = scen::l06_probe_report_probe(kDeadline);
    EXPECT_FALSE(definition.requires_track);
    EXPECT_EQ(definition.id, scen::kL06ProbeReport);
    EXPECT_TRUE(static_cast<bool>(definition.next_steps));
}

}  // namespace
