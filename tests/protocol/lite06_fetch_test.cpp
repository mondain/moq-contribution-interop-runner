// The moq-lite-06 fetch scenarios (L2a Task B2): both evaluators against the conforming scripted publisher and against
// publishers that violate exactly one rule, plus the NotRun conditions of the catalog rows (L06-7-16-MUST-177,
// L06-5-1-3-MUST-066).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_fetch.h"
#include "moq/interop/scenarios/lite06_track.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_fetch_short_run;
using moq::interop::scenarios::evaluate_l06_fetch_unknown_group_reset;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
namespace scen = moq::interop::scenarios;
namespace lite06 = moq::interop::scenarios::lite06;
namespace session = moq::interop::session;

using Verdict = std::optional<bool>;

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 40000ms;
const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";

const Verdict kPass{true};
const Verdict kFail{false};
const Verdict kNotRun{};

// Four frames per group everywhere, so a learned group has the frames the three ranges need, and every group the
// subscription can deliver is one FETCH holds.
ConformingLitePublisherConfig base_config() {
    ConformingLitePublisherConfig config;
    config.frames_per_group = 4;
    config.fetch_frames_per_group = 4;
    config.fetch_last_group = 100;
    return config;
}

LiteTranscript run(ConformingLitePublisher& publisher, LiteProbeDefinition definition) {
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    return run_lite_probe(peer, std::move(definition), clock, kTick);
}

LiteTranscript run_group(ConformingLitePublisherConfig config = base_config()) {
    ConformingLitePublisher publisher(std::move(config));
    return run(publisher, scen::l06_fetch_group_probe(kDeadline, kBroadcast, kTrack));
}

LiteTranscript run_unknown(ConformingLitePublisherConfig config = base_config()) {
    ConformingLitePublisher publisher(std::move(config));
    return run(publisher, scen::l06_fetch_unknown_group_probe(kDeadline, kBroadcast, kTrack));
}

bool is_fetch(const LiteRunnerRequest& request) { return request.stream_type == 0x3 && request.bidirectional; }

const session::LiteStreamRecord* stream_of(const LiteTranscript& t, std::string_view label) {
    const auto* step = lite06::step_labelled(t, label);
    if (!step || !step->stream_id) return nullptr;
    return lite06::find_stream(t, *step->stream_id);
}

// --- l06-fetch-group (L06-7-16-MUST-177) ------------------------------------------------------------------------

TEST(Lite06FetchGroup, AConformingPublisherPassesAndTheThreeRangesReturnTheirFrames) {
    const auto t = run_group();
    ASSERT_TRUE(scen::judgeable_with_stimulus(t)) << t.harness_failure_reason;
    EXPECT_EQ(evaluate_l06_fetch_short_run(t), kPass);
    const auto* whole = stream_of(t, scen::kL06FetchWholeLabel);
    const auto* leading = stream_of(t, scen::kL06FetchLeadingLabel);
    const auto* trailing = stream_of(t, scen::kL06FetchTrailingLabel);
    ASSERT_TRUE(whole != nullptr && leading != nullptr && trailing != nullptr);
    EXPECT_EQ(session::peer_fetch_frames(*whole).size(), 4u);
    EXPECT_EQ(session::peer_fetch_frames(*leading).size(), 2u);
    EXPECT_EQ(session::peer_fetch_frames(*trailing).size(), 3u);
    EXPECT_TRUE(whole->fin_seen && leading->fin_seen && trailing->fin_seen);
}

TEST(Lite06FetchGroup, TheRangesAskForTheLearnedGroupAndAreIndexPlusOne) {
    const auto t = run_group();
    const auto* whole_step = lite06::step_labelled(t, scen::kL06FetchWholeLabel);
    ASSERT_NE(whole_step, nullptr);
    const auto whole = scen::l06_decode_fetch_stimulus(whole_step->bytes);
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->broadcast_path, kBroadcast);
    EXPECT_EQ(whole->track_name, kTrack);
    EXPECT_EQ(whole->frame_start, 0u);
    EXPECT_EQ(whole->frame_end, 0u);
    const auto leading = scen::l06_decode_fetch_stimulus(lite06::step_labelled(t, scen::kL06FetchLeadingLabel)->bytes);
    const auto trailing = scen::l06_decode_fetch_stimulus(lite06::step_labelled(t, scen::kL06FetchTrailingLabel)->bytes);
    ASSERT_TRUE(leading.has_value() && trailing.has_value());
    EXPECT_EQ(leading->group_sequence, whole->group_sequence);
    EXPECT_EQ(trailing->group_sequence, whole->group_sequence);
    EXPECT_EQ(leading->frame_end, 2u);    // frames 0 and 1
    EXPECT_EQ(trailing->frame_start, 1u);
    EXPECT_EQ(trailing->frame_end, 0u);
}

TEST(Lite06FetchGroup, AFinAfterAShorterRunFailsEachOfTheThreeRanges) {
    for (const auto which : {0, 1, 2}) {
        auto config = base_config();
        std::size_t seen = 0;
        config.hooks.on_request = [&, which](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                             const LiteRunnerRequest& request) {
            if (!is_fetch(request)) return false;
            if (static_cast<int>(seen++) != which) return false;
            // Answer this one fetch one frame short, with FIN.
            const auto* fetch = std::get_if<l06::FetchRequest>(&request.message);
            if (!fetch) return false;
            const std::uint64_t end = fetch->frame_end == 0 ? 4 : fetch->frame_end;
            Bytes bytes;
            for (std::uint64_t i = fetch->frame_start; i + 1 < end; ++i) {
                l06::Frame value;
                value.timestamp_delta = 33;
                value.payload = bytes_of("short");
                bytes = join({bytes, frame(value)});
            }
            peer.data(request.stream, bytes, true);
            (void)publisher;
            return true;
        };
        EXPECT_EQ(evaluate_l06_fetch_short_run(run_group(config)), kFail) << which;
    }
}

TEST(Lite06FetchGroup, ThePublishersTruncationDefectFails) {
    auto config = base_config();
    config.defect = LiteDefect::FetchTruncatesOnShortRange;
    EXPECT_EQ(evaluate_l06_fetch_short_run(run_group(config)), kFail);
}

TEST(Lite06FetchGroup, AResetForTheGroupIsConformingButGivesNoVerdict) {
    auto config = base_config();
    config.fetch_first_retained_frame = 100;  // every fetch asks for evicted frames and is reset TOO_FAR_BEHIND
    const auto t = run_group(config);
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    EXPECT_EQ(evaluate_l06_fetch_short_run(t), kNotRun);
}

TEST(Lite06FetchGroup, AResetOfOneRangeDoesNotHideAFailOfAnother) {
    auto config = base_config();
    config.defect = LiteDefect::FetchTruncatesOnShortRange;
    config.hooks.on_request = [seen = std::size_t{0}](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                                      const LiteRunnerRequest& request) mutable {
        if (!is_fetch(request) || seen++ != 0) return false;
        publisher.refuse(peer, request.stream, 0x33);  // the whole-group fetch is reset: conforming
        return true;
    };
    EXPECT_EQ(evaluate_l06_fetch_short_run(run_group(config)), kFail);
}

TEST(Lite06FetchGroup, MoreFramesThanAskedIsNotThisRow) {
    auto config = base_config();
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!is_fetch(request)) return false;
        Bytes bytes;
        for (int i = 0; i < 9; ++i) {
            l06::Frame value;
            value.timestamp_delta = 1;
            value.payload = bytes_of("x");
            bytes = join({bytes, frame(value)});
        }
        peer.data(request.stream, bytes, true);
        return true;
    };
    EXPECT_EQ(evaluate_l06_fetch_short_run(run_group(config)), kNotRun);
}

TEST(Lite06FetchGroup, NoCompleteGroupMeansNoFetchAndNoVerdict) {
    auto config = base_config();
    config.groups_per_subscription = 0;  // only the live group, left open: nothing ever completes
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, scen::l06_fetch_group_probe(kDeadline, kBroadcast, kTrack, 3000ms, 1500ms));
    EXPECT_EQ(lite06::step_labelled(t, scen::kL06FetchWholeLabel), nullptr);
    EXPECT_EQ(evaluate_l06_fetch_short_run(t), kNotRun);
}

TEST(Lite06FetchGroup, ATooShortReferenceGroupIsNotLearned) {
    auto config = base_config();
    config.frames_per_group = 2;  // below the three frames the ranges need
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, scen::l06_fetch_group_probe(kDeadline, kBroadcast, kTrack, 3000ms, 1500ms));
    EXPECT_EQ(lite06::step_labelled(t, scen::kL06FetchWholeLabel), nullptr);
    EXPECT_EQ(evaluate_l06_fetch_short_run(t), kNotRun);
}

TEST(Lite06FetchGroup, ASilentPublisherOnFetchIsNotRun) {
    auto config = base_config();
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer&, const LiteRunnerRequest& request) {
        return is_fetch(request);
    };
    EXPECT_EQ(evaluate_l06_fetch_short_run(run_group(config)), kNotRun);
}

TEST(Lite06FetchGroup, ATrackLookupThatFailsMeansTheRunnerHasNoTrackInfoAndNoVerdict) {
    auto config = base_config();
    config.hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                 const LiteRunnerRequest& request) {
        if (request.stream_type != 0x6 || !request.bidirectional) return false;
        publisher.refuse(peer, request.stream, 0x33);
        return true;
    };
    EXPECT_EQ(evaluate_l06_fetch_short_run(run_group(config)), kNotRun);
}

TEST(Lite06FetchGroup, AWrongScenarioOrAnUnjudgeableTranscriptIsNotRun) {
    const auto t = run_group();
    ASSERT_EQ(evaluate_l06_fetch_short_run(t), kPass);
    auto other = t;
    other.scenario_id = scen::kL06FetchUnknownGroup;
    EXPECT_EQ(evaluate_l06_fetch_short_run(other), kNotRun);
    auto limited = t;
    limited.event_limit_reached = true;
    EXPECT_EQ(evaluate_l06_fetch_short_run(limited), kNotRun);
    auto no_fixture = t;
    no_fixture.broadcast_path.clear();
    EXPECT_EQ(evaluate_l06_fetch_short_run(no_fixture), kNotRun);
    auto altered = t;
    for (auto& step : altered.steps) {
        if (step.label == scen::kL06FetchLeadingLabel && step.bytes.size() > 3) step.bytes[3] = std::byte{0x7f};
    }
    EXPECT_EQ(evaluate_l06_fetch_short_run(altered), kNotRun) << "a stimulus that is not the one the scenario builds";
}

// --- l06-fetch-unknown-group (L06-5-1-3-MUST-066) ----------------------------------------------------------------

TEST(Lite06FetchUnknownGroup, AConformingPublisherResetsAndPasses) {
    const auto t = run_unknown();
    ASSERT_TRUE(scen::judgeable_with_stimulus(t)) << t.harness_failure_reason;
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(t), kPass);
    const auto* record = stream_of(t, scen::kL06FetchUnknownLabel);
    ASSERT_NE(record, nullptr);
    ASSERT_TRUE(record->reset_code.has_value());
    EXPECT_EQ(*record->reset_code, 0x33u);
}

TEST(Lite06FetchUnknownGroup, AnyResetCodeIsEnough) {
    auto config = base_config();
    config.not_found_code = 0x0;
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(run_unknown(config)), kPass);
}

TEST(Lite06FetchUnknownGroup, ServingAGroupItDoesNotHaveFails) {
    auto config = base_config();
    config.defect = LiteDefect::FetchIgnoresUnknownGroup;
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(run_unknown(config)), kFail);
}

TEST(Lite06FetchUnknownGroup, AnEmptyFinFails) {
    auto config = base_config();
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!is_fetch(request)) return false;
        peer.fin(request.stream);
        return true;
    };
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(run_unknown(config)), kFail);
}

// Final review I3: the session close was documented as a Fail but the evaluator gated on judgeable_with_stimulus,
// which a peer close during the allowance always defeats.
TEST(Lite06FetchUnknownGroup, ASessionCloseInsteadOfTheResetFails) {
    auto config = base_config();
    config.hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                 const LiteRunnerRequest& request) {
        if (!is_fetch(request)) return false;
        publisher.close(peer, 0x0);
        return true;
    };
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(run_unknown(config)), kFail);
}

TEST(Lite06FetchUnknownGroup, NoAnswerInsideTheWindowIsNotRun) {
    auto config = base_config();
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer&, const LiteRunnerRequest& request) {
        return is_fetch(request);
    };
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(run_unknown(config)), kNotRun);
}

TEST(Lite06FetchUnknownGroup, AnUnreadableAnswerIsNotRun) {
    auto config = base_config();
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!is_fetch(request)) return false;
        peer.data(request.stream, bytes_of("\x03\x01"), true);  // a FRAME cut short by the Message Length
        return true;
    };
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(run_unknown(config)), kNotRun);
}

TEST(Lite06FetchUnknownGroup, AWrongScenarioOrAnUnjudgeableTranscriptIsNotRun) {
    const auto t = run_unknown();
    ASSERT_EQ(evaluate_l06_fetch_unknown_group_reset(t), kPass);
    auto other = t;
    other.scenario_id = scen::kL06FetchGroup;
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(other), kNotRun);
    auto no_fixture = t;
    no_fixture.track_name.clear();
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(no_fixture), kNotRun);
    auto limited = t;
    limited.event_limit_reached = true;
    EXPECT_EQ(evaluate_l06_fetch_unknown_group_reset(limited), kNotRun);
}

// --- the builders -----------------------------------------------------------------------------------------------

TEST(Lite06FetchBuilders, TheStimuliAreExactlyWhatTheBuilderSent) {
    const auto request = scen::l06_fetch_request(kBroadcast, kTrack, 7, 1, 3);
    const auto bytes = scen::l06_fetch_bytes(request);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), std::byte{0x03});
    EXPECT_EQ(scen::l06_decode_fetch_stimulus(bytes), std::optional<l06::FetchRequest>(request));
    EXPECT_FALSE(scen::l06_decode_fetch_stimulus({}).has_value());
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    EXPECT_FALSE(scen::l06_decode_fetch_stimulus(trailing).has_value());
    auto wrong_type = bytes;
    wrong_type[0] = std::byte{0x02};
    EXPECT_FALSE(scen::l06_decode_fetch_stimulus(wrong_type).has_value());
    // The runner never builds an inverted range by accident.
    EXPECT_TRUE(scen::l06_fetch_bytes(scen::l06_fetch_request(kBroadcast, kTrack, 7, 5, 3)).empty());
}

TEST(Lite06FetchBuilders, TheBuildersValidateTheirInputs) {
    EXPECT_THROW(scen::l06_fetch_group_probe(kDeadline, "", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_fetch_group_probe(kDeadline, kBroadcast, ""), std::invalid_argument);
    EXPECT_THROW(scen::l06_fetch_group_probe(5000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_fetch_group_probe(kDeadline, kBroadcast, kTrack, 3000ms, 0ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_fetch_group_probe(kDeadline, kBroadcast, kTrack, 3000ms, 15000ms, 0ms),
                 std::invalid_argument);
    EXPECT_THROW(scen::l06_fetch_unknown_group_probe(kDeadline, "", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_fetch_unknown_group_probe(1000ms, kBroadcast, kTrack), std::invalid_argument);
    const auto group = scen::l06_fetch_group_probe(kDeadline, kBroadcast, kTrack);
    EXPECT_TRUE(group.requires_track);
    EXPECT_EQ(group.id, scen::kL06FetchGroup);
    EXPECT_TRUE(static_cast<bool>(group.next_steps));
    const auto unknown = scen::l06_fetch_unknown_group_probe(kDeadline, kBroadcast, kTrack);
    EXPECT_TRUE(unknown.requires_track);
    EXPECT_EQ(unknown.id, scen::kL06FetchUnknownGroup);
    EXPECT_EQ(unknown.steps.back().label, lite06::kAllowanceLabel);
    // The unknown-group sequence is well beyond anything the fixture holds.
    const auto fetch = std::find_if(unknown.steps.begin(), unknown.steps.end(),
                                     [](const scen::LiteStep& step) { return step.label == scen::kL06FetchUnknownLabel; });
    ASSERT_NE(fetch, unknown.steps.end());
    const auto decoded = scen::l06_decode_fetch_stimulus(fetch->bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->group_sequence, scen::kL06FetchUnknownGroupSequence);
}

}  // namespace
