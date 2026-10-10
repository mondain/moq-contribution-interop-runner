// The moq-lite-06 track scenario (L2a Task B2): both evaluators against the conforming scripted publisher and against
// publishers that violate exactly one rule, plus the NotRun conditions of the catalog rows (L06-7-12-MUST-NOT-163,
// L06-7-12-MUST-170).

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_track.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_track_info_immutable;
using moq::interop::scenarios::evaluate_l06_track_info_timescale_nonzero;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
namespace scen = moq::interop::scenarios;
namespace lite06 = moq::interop::scenarios::lite06;
namespace session = moq::interop::session;

using Verdict = std::optional<bool>;

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 20000ms;
const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";

const Verdict kPass{true};
const Verdict kFail{false};
const Verdict kNotRun{};

LiteTranscript run(ConformingLitePublisher& publisher, LiteProbeDefinition definition) {
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    return run_lite_probe(peer, std::move(definition), clock, kTick);
}

LiteTranscript run_track(ConformingLitePublisherConfig config = {}) {
    ConformingLitePublisher publisher(std::move(config));
    return run(publisher, scen::l06_track_info_probe(kDeadline, kBroadcast, kTrack));
}

struct Verdicts {
    Verdict immutable;
    Verdict timescale;
    bool operator==(const Verdicts&) const = default;
};

Verdicts judge(const LiteTranscript& t) {
    return {evaluate_l06_track_info_immutable(t), evaluate_l06_track_info_timescale_nonzero(t)};
}

TEST(Lite06Track, AConformingPublisherPassesBothRows) {
    const auto t = run_track();
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    EXPECT_EQ(judge(t), (Verdicts{kPass, kPass}));
    // Two lookups, each answered once.
    std::size_t track_streams = 0;
    for (const auto& record : t.streams) {
        if (record.kind != session::LiteStreamKind::Track) continue;
        ++track_streams;
        EXPECT_EQ(session::peer_track_info(record).size(), 1u);
    }
    EXPECT_EQ(track_streams, 2u);
}

TEST(Lite06Track, ATrackInfoThatChangesBetweenRequestsFailsImmutability) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::TrackInfoChangesBetweenRequests;
    EXPECT_EQ(judge(run_track(config)), (Verdicts{kFail, kPass}));
}

TEST(Lite06Track, AZeroTimescaleFailsTheTimescaleRowAndNotImmutability) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::TrackInfoZeroTimescale;
    EXPECT_EQ(judge(run_track(config)), (Verdicts{kPass, kFail}));
}

TEST(Lite06Track, EachFieldCountsForImmutability) {
    // A hook that changes one field of the SECOND reply at a time.
    for (const auto field : {0, 1, 2}) {
        ConformingLitePublisherConfig config;
        std::size_t replies = 0;
        config.hooks.on_request = [&](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
            if (request.stream_type != 0x6 || !request.bidirectional) return false;
            l06::TrackInfo info{60, 30000, 90000};
            if (replies++ == 1) {
                if (field == 0) info.publisher_priority = 61;
                if (field == 1) info.publisher_max_age_ms = 0;
                if (field == 2) info.timescale = 1000;
            }
            peer.data(request.stream, track_info(info), true);
            return true;
        };
        EXPECT_EQ(evaluate_l06_track_info_immutable(run_track(config)), kFail) << field;
    }
}

TEST(Lite06Track, ATrackThatDoesNotExistIsNotRun) {
    ConformingLitePublisherConfig config;
    config.track = "audio";  // the fixture track is "video": the publisher resets both lookups
    EXPECT_EQ(judge(run_track(config)), (Verdicts{kNotRun, kNotRun}));
}

TEST(Lite06Track, OneLookupAnsweredIsNotRunForImmutabilityAndJudgesTheTimescale) {
    ConformingLitePublisherConfig config;
    std::size_t seen = 0;
    config.hooks.on_request = [&](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (request.stream_type != 0x6 || !request.bidirectional) return false;
        if (seen++ == 0) peer.data(request.stream, track_info({60, 30000, 90000}), true);
        return true;  // the second lookup is never answered
    };
    EXPECT_EQ(judge(run_track(config)), (Verdicts{kNotRun, kPass}));
}

TEST(Lite06Track, ASilentPublisherIsNotRun) {
    ConformingLitePublisherConfig config;
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer&, const LiteRunnerRequest& request) {
        return request.stream_type == 0x6 && request.bidirectional;  // swallow the lookups
    };
    EXPECT_EQ(judge(run_track(config)), (Verdicts{kNotRun, kNotRun}));
}

TEST(Lite06Track, ASecondReplyOnOneStreamIsUnreadableNotRun) {
    ConformingLitePublisherConfig config;
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (request.stream_type != 0x6 || !request.bidirectional) return false;
        peer.data(request.stream, join({track_info({60, 30000, 90000}), track_info({60, 30000, 90000})}), true);
        return true;
    };
    EXPECT_EQ(judge(run_track(config)), (Verdicts{kNotRun, kNotRun}));
}

TEST(Lite06Track, AWrongScenarioOrAnUnjudgeableTranscriptIsNotRun) {
    auto t = run_track();
    ASSERT_EQ(judge(t), (Verdicts{kPass, kPass}));
    auto other = t;
    other.scenario_id = "l06-subscribe-latest";
    EXPECT_EQ(judge(other), (Verdicts{kNotRun, kNotRun}));
    auto limited = t;
    limited.event_limit_reached = true;
    EXPECT_EQ(judge(limited), (Verdicts{kNotRun, kNotRun}));
    auto no_fixture = t;
    no_fixture.track_name.clear();
    EXPECT_EQ(judge(no_fixture), (Verdicts{kNotRun, kNotRun}));
    auto never_sent = t;
    for (auto& step : never_sent.steps) {
        if (step.label == scen::kL06TrackSecondLabel) step.accepted = 0;
    }
    EXPECT_EQ(judge(never_sent), (Verdicts{kNotRun, kNotRun}));
}

TEST(Lite06Track, TheStimulusIsExactlyWhatTheBuilderSent) {
    const auto t = run_track();
    const auto bytes = scen::l06_track_bytes(kBroadcast, kTrack);
    ASSERT_FALSE(bytes.empty());
    for (const auto label : {scen::kL06TrackFirstLabel, scen::kL06TrackSecondLabel}) {
        const auto* step = lite06::step_labelled(t, label);
        ASSERT_NE(step, nullptr) << label;
        EXPECT_EQ(step->bytes, bytes);
    }
    // STREAM_TYPE 0x6 then TRACK{"demo/live", "video"}: 06 | 10 | 09 "demo/live" | 05 "video".
    EXPECT_EQ(bytes.front(), std::byte{0x06});
    EXPECT_EQ(bytes[1], std::byte{0x10});
}

TEST(Lite06Track, TheBuilderValidatesItsInputs) {
    EXPECT_THROW(scen::l06_track_info_probe(kDeadline, "", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_track_info_probe(kDeadline, kBroadcast, ""), std::invalid_argument);
    EXPECT_THROW(scen::l06_track_info_probe(1000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_track_info_probe(kDeadline, kBroadcast, kTrack, 3000ms, 0ms), std::invalid_argument);
    const auto definition = scen::l06_track_info_probe(kDeadline, kBroadcast, kTrack);
    EXPECT_TRUE(definition.requires_track);
    EXPECT_EQ(definition.id, scen::kL06TrackInfo);
    EXPECT_EQ(definition.broadcast_path, kBroadcast);
    EXPECT_EQ(definition.track_name, kTrack);
    ASSERT_FALSE(definition.steps.empty());
    EXPECT_EQ(definition.steps.back().label, lite06::kAllowanceLabel);
}

}  // namespace
