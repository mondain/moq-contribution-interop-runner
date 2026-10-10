// The moq-lite-06 goaway scenarios (L2a Task B3): the three evaluators against the conforming scripted publisher and
// against publishers that violate exactly one rule, plus the NotRun conditions of the catalog rows
// (L06-5-1-6-MUST-NOT-077, L06-7-18-MUST-186, L06-7-18-MUST-179).

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_goaway.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_goaway_no_new_streams;
using moq::interop::scenarios::evaluate_l06_goaway_oversize_violation;
using moq::interop::scenarios::evaluate_l06_goaway_second_closes;
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

LiteTranscript run(ConformingLitePublisher& publisher, LiteProbeDefinition definition) {
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    return run_lite_probe(peer, std::move(definition), clock, kTick);
}

// A publisher with a group every poll for four seconds of manual time: the cadence the rule is judged against.
ConformingLitePublisherConfig busy_config() {
    ConformingLitePublisherConfig config;
    config.groups_per_subscription = 400;
    config.keep_live_group_open = false;
    return config;
}

LiteTranscript run_single(ConformingLitePublisherConfig config) {
    ConformingLitePublisher publisher(std::move(config));
    return run(publisher, scen::l06_goaway_single_probe(kDeadline, kBroadcast, kTrack));
}

LiteTranscript run_duplicate(ConformingLitePublisherConfig config = {}) {
    ConformingLitePublisher publisher(std::move(config));
    return run(publisher, scen::l06_goaway_duplicate_probe(kDeadline));
}

LiteTranscript run_oversize(ConformingLitePublisherConfig config = {}) {
    ConformingLitePublisher publisher(std::move(config));
    return run(publisher, scen::l06_goaway_oversize_probe(kDeadline));
}

// --- l06-goaway-single (L06-5-1-6-MUST-NOT-077) -----------------------------------------------------------------

TEST(Lite06GoawaySingle, APublisherThatStopsOpeningStreamsPasses) {
    const auto t = run_single(busy_config());
    ASSERT_TRUE(scen::judgeable_with_stimulus(t)) << t.harness_failure_reason;
    ASSERT_NE(lite06::step_labelled(t, scen::kL06GoawayLabel), nullptr) << "the cadence proof triggered the GOAWAY";
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(t), kPass);
}

TEST(Lite06GoawaySingle, APublisherThatKeepsOpeningStreamsFails) {
    auto config = busy_config();
    config.defect = LiteDefect::OpensStreamsAfterGoaway;
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(run_single(config)), kFail);
}

TEST(Lite06GoawaySingle, AStreamThatCrossedTheGoawayIsNotHeldAgainstThePublisher) {
    // One extra stream opened inside the allowance after the GOAWAY (in flight when the publisher saw it): conforming.
    auto config = busy_config();
    config.groups_per_subscription = 6;
    bool late_sent = false;
    config.hooks.on_poll = [&](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        if (late_sent || !publisher.goaway_received()) return;
        late_sent = true;
        peer.data(peer.open_peer_uni(),
                  join({stream_type(0x0), group_header({0, 99, 0})}), true);
    };
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(run_single(config)), kPass);
}

TEST(Lite06GoawaySingle, NoCadenceBeforeTheGoawayMeansNoGoawayAndNoVerdict) {
    ConformingLitePublisherConfig config;
    config.groups_per_subscription = 0;
    config.keep_live_group_open = true;  // a single, never-completed group: no cadence
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, scen::l06_goaway_single_probe(kDeadline, kBroadcast, kTrack, 6000ms, 1500ms));
    EXPECT_EQ(lite06::step_labelled(t, scen::kL06GoawayLabel), nullptr);
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(t), kNotRun);
}

TEST(Lite06GoawaySingle, ASessionCloseOnTheGoawayIsAGracefulShutdownAndNotRun) {
    auto config = busy_config();
    config.defect = LiteDefect::GoawayClosesSessionOnFirst;
    const auto t = run_single(config);
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(t), kNotRun);
}

TEST(Lite06GoawaySingle, AnObservationShorterThanTheCadenceIsNotRun) {
    // Groups one second apart, watched for far less than that after the allowance: silence proves nothing.
    ConformingLitePublisherConfig config;
    config.groups_per_subscription = 8;
    config.keep_live_group_open = false;
    std::size_t polls = 0;
    config.hooks.on_poll = [&](ConformingLitePublisher&, ScriptedLitePeer& peer) {
        // Two Group streams a second apart before the GOAWAY, fed by the hook rather than the default emission.
        ++polls;
        if (polls == 20 || polls == 120 || polls == 220) {
            peer.data(peer.open_peer_uni(), join({stream_type(0x0), group_header({0, polls, 0})}), true);
        }
    };
    config.hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                 const LiteRunnerRequest& request) {
        if (request.stream_type != 0x2) return false;
        peer.data(request.stream, subscribe_response(l06::SubscribeOk{5}));
        (void)publisher;
        return true;  // answered, but the default groups are suppressed
    };
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, scen::l06_goaway_single_probe(kDeadline, kBroadcast, kTrack, 2200ms));
    ASSERT_NE(lite06::step_labelled(t, scen::kL06GoawayLabel), nullptr);
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(t), kNotRun);
}

// Final review I1: with a slow GOP the first two Group streams are close together (the first is the group in
// progress when the SUBSCRIBE lands), so two streams understated the cadence and a publisher that keeps opening
// streams could pass because its next one fell after the probe. Three streams are needed and only the gaps between
// whole groups count.
TEST(Lite06GoawaySingle, ASlowGopPublisherWhoseFirstGroupWasMidGopIsNotRun) {
    ConformingLitePublisherConfig config;
    config.groups_per_subscription = 8;
    config.keep_live_group_open = false;
    std::size_t polls = 0;
    config.hooks.on_poll = [&](ConformingLitePublisher&, ScriptedLitePeer& peer) {
        ++polls;
        // Groups at 0.2 s, 0.6 s and 8.6 s: an 8 s GOP after a burst.
        if (polls == 20 || polls == 60 || polls == 860) {
            peer.data(peer.open_peer_uni(), join({stream_type(0x0), group_header({0, polls, 0})}), true);
        }
    };
    config.hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (request.stream_type != 0x2) return false;
        peer.data(request.stream, subscribe_response(l06::SubscribeOk{5}));
        return true;  // answered, but the default groups are suppressed
    };
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, scen::l06_goaway_single_probe(kDeadline, kBroadcast, kTrack));
    ASSERT_NE(lite06::step_labelled(t, scen::kL06GoawayLabel), nullptr);
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(t), kNotRun);
}

TEST(Lite06GoawaySingle, AWrongScenarioOrAnUnjudgeableTranscriptIsNotRun) {
    const auto t = run_single(busy_config());
    ASSERT_EQ(evaluate_l06_goaway_no_new_streams(t), kPass);
    auto other = t;
    other.scenario_id = scen::kL06GoawayDuplicate;
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(other), kNotRun);
    auto limited = t;
    limited.event_limit_reached = true;
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(limited), kNotRun);
    auto no_fixture = t;
    no_fixture.track_name.clear();
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(no_fixture), kNotRun);
    auto altered = t;
    for (auto& step : altered.steps) {
        if (step.label == scen::kL06GoawayLabel && step.bytes.size() > 4) step.bytes[4] = std::byte{'x'};
    }
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(altered), kNotRun);
}

// --- l06-goaway-duplicate (L06-7-18-MUST-186) -------------------------------------------------------------------

TEST(Lite06GoawayDuplicate, ASecondGoawayClosedWithProtocolViolationPasses) {
    const auto t = run_duplicate();
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
    EXPECT_EQ(evaluate_l06_goaway_second_closes(t), kPass);
}

TEST(Lite06GoawayDuplicate, ASecondGoawayThatIsIgnoredFails) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayDuplicateIgnored;
    const auto t = run_duplicate(config);
    EXPECT_FALSE(t.peer_close.has_value());
    EXPECT_EQ(evaluate_l06_goaway_second_closes(t), kFail);
}

TEST(Lite06GoawayDuplicate, AnotherCloseCodeFails) {
    ConformingLitePublisherConfig config;
    config.protocol_violation_code = 0x1;  // INTERNAL_ERROR
    EXPECT_EQ(evaluate_l06_goaway_second_closes(run_duplicate(config)), kFail);
}

TEST(Lite06GoawayDuplicate, ACloseOnTheFirstGoawayIsNotTheReactionToTheSecond) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayClosesSessionOnFirst;
    const auto t = run_duplicate(config);
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(evaluate_l06_goaway_second_closes(t), kNotRun);
}

TEST(Lite06GoawayDuplicate, ACloseWithProtocolViolationOnTheFirstGoawayIsNotRunEither) {
    ConformingLitePublisherConfig config;
    config.hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                 const LiteRunnerRequest& request) {
        if (request.stream_type != 0x5 || !request.bidirectional) return false;
        publisher.close(peer, 0x3);  // the first GOAWAY is already refused
        return true;
    };
    EXPECT_EQ(evaluate_l06_goaway_second_closes(run_duplicate(config)), kNotRun);
}

TEST(Lite06GoawayDuplicate, AWrongScenarioIsNotRun) {
    auto t = run_duplicate();
    ASSERT_EQ(evaluate_l06_goaway_second_closes(t), kPass);
    t.scenario_id = scen::kL06GoawayOversize;
    EXPECT_EQ(evaluate_l06_goaway_second_closes(t), kNotRun);
}

// --- l06-goaway-oversize (L06-7-18-MUST-179) --------------------------------------------------------------------

TEST(Lite06GoawayOversize, ACloseWithProtocolViolationPasses) {
    const auto t = run_oversize();
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
    EXPECT_EQ(evaluate_l06_goaway_oversize_violation(t), kPass);
}

TEST(Lite06GoawayOversize, LoggingTheOversizeUriAndCarryingOnFails) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayOversizeLogged;
    const auto t = run_oversize(config);
    EXPECT_FALSE(t.peer_close.has_value());
    EXPECT_EQ(evaluate_l06_goaway_oversize_violation(t), kFail);
}

TEST(Lite06GoawayOversize, AnotherCloseCodeFails) {
    ConformingLitePublisherConfig config;
    config.protocol_violation_code = 0x1;
    EXPECT_EQ(evaluate_l06_goaway_oversize_violation(run_oversize(config)), kFail);
}

TEST(Lite06GoawayOversize, AWrongScenarioIsNotRun) {
    auto t = run_oversize();
    ASSERT_EQ(evaluate_l06_goaway_oversize_violation(t), kPass);
    t.scenario_id = scen::kL06GoawayDuplicate;
    EXPECT_EQ(evaluate_l06_goaway_oversize_violation(t), kNotRun);
}

// --- the builders -----------------------------------------------------------------------------------------------

TEST(Lite06GoawayBuilders, TheStimuliAreWhatTheDraftSays) {
    // STREAM_TYPE 0x5, GOAWAY{"https://goaway-target.invalid/moq"}: 05 | 22 | 21 "https://goaway-target.invalid/moq".
    const auto valid = scen::l06_goaway_bytes(scen::kL06GoawayUri);
    ASSERT_GT(valid.size(), 3u);
    EXPECT_EQ(valid[0], std::byte{0x05});
    EXPECT_EQ(valid[1], std::byte{0x22});
    EXPECT_EQ(valid[2], std::byte{0x21});
    EXPECT_EQ(valid.size(), 2 + 0x22u);
    // The oversize message claims 8193 bytes and carries them: 05 | message length 8195 (2-byte 60 03) | URI length
    // 8193 (60 01) | 8193 bytes.
    const auto oversize = scen::l06_goaway_oversize_bytes();
    ASSERT_GT(oversize.size(), 5u);
    EXPECT_EQ(oversize[0], std::byte{0x05});
    EXPECT_EQ(oversize[1], std::byte{0x60});
    EXPECT_EQ(oversize[2], std::byte{0x03});
    EXPECT_EQ(oversize[3], std::byte{0x60});
    EXPECT_EQ(oversize[4], std::byte{0x01});
    EXPECT_EQ(oversize.size(), 1u + 2u + 2u + 8193u);
    // The typed encoder refuses what the probe sends: the violation is built raw on purpose.
    EXPECT_TRUE(scen::l06_goaway_bytes(std::string(8193, 'a')).empty());
}

TEST(Lite06GoawayBuilders, TheBuildersValidateTheirInputs) {
    EXPECT_THROW(scen::l06_goaway_single_probe(kDeadline, "", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_goaway_single_probe(kDeadline, kBroadcast, ""), std::invalid_argument);
    EXPECT_THROW(scen::l06_goaway_single_probe(8000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_goaway_single_probe(kDeadline, kBroadcast, kTrack, 6000ms, 0ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_goaway_duplicate_probe(3000ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_goaway_oversize_probe(3000ms), std::invalid_argument);
    const auto single = scen::l06_goaway_single_probe(kDeadline, kBroadcast, kTrack);
    EXPECT_TRUE(single.requires_track);
    EXPECT_TRUE(static_cast<bool>(single.next_steps));
    const auto duplicate = scen::l06_goaway_duplicate_probe(kDeadline);
    EXPECT_FALSE(duplicate.requires_track);
    EXPECT_EQ(duplicate.id, scen::kL06GoawayDuplicate);
    const auto oversize = scen::l06_goaway_oversize_probe(kDeadline);
    EXPECT_FALSE(oversize.requires_track);
    EXPECT_EQ(oversize.id, scen::kL06GoawayOversize);
}


// --- L2c: when the first GOAWAY ends the session, rows 077 and 186 are not applicable - and only then -----------------

session::PeerCloseInfo close_info(std::uint64_t code, std::uint64_t at_ns) {
    session::PeerCloseInfo info;
    info.space = moq::interop::transport::CloseErrorSpace::Application;
    info.code = code;
    info.at_ns = at_ns;
    return info;
}

TEST(Lite06GoawayInapplicable, AGracefulCloseOnTheFirstGoawayMakesBothRowsInapplicable) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayClosesSessionOnFirst;
    EXPECT_TRUE(scen::l06_goaway_single_inapplicable(run_single(config)));
    EXPECT_TRUE(scen::l06_goaway_duplicate_inapplicable(run_duplicate(config)));
}

TEST(Lite06GoawayInapplicable, ACarryingOnPublisherIsJudgedNotInapplicable) {
    EXPECT_FALSE(scen::l06_goaway_single_inapplicable(run_single(busy_config())));
    EXPECT_FALSE(scen::l06_goaway_duplicate_inapplicable(run_duplicate()));
}

// A publisher that keeps opening streams past the allowance and only then closes the session broke row 077: the close
// must not turn that into "not applicable" (the run would pass).
TEST(Lite06GoawayInapplicable, StreamsOpenedAfterTheAllowanceKeepRow077InapplicableOut) {
    auto config = busy_config();
    config.defect = LiteDefect::OpensStreamsAfterGoaway;
    auto t = run_single(config);
    ASSERT_EQ(evaluate_l06_goaway_no_new_streams(t), kFail);
    t.peer_close = close_info(0x0, t.ended_ns);
    EXPECT_EQ(evaluate_l06_goaway_no_new_streams(t), kNotRun);  // the evaluator gives no verdict on a close...
    EXPECT_FALSE(scen::l06_goaway_single_inapplicable(t));       // ...and the predicate must not excuse the streams
}

// Only the graceful close the draft allows (an application close with NO_ERROR) is "not applicable".
TEST(Lite06GoawayInapplicable, ACloseWithAnotherCodeOrSpaceIsNot) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayClosesSessionOnFirst;
    for (const auto code : {std::uint64_t{0x3}, std::uint64_t{0x1}}) {
        auto single = run_single(config);
        ASSERT_TRUE(single.peer_close.has_value());
        single.peer_close = close_info(code, single.peer_close->at_ns);
        EXPECT_FALSE(scen::l06_goaway_single_inapplicable(single)) << code;
        auto duplicate = run_duplicate(config);
        ASSERT_TRUE(duplicate.peer_close.has_value());
        duplicate.peer_close = close_info(code, duplicate.peer_close->at_ns);
        EXPECT_FALSE(scen::l06_goaway_duplicate_inapplicable(duplicate)) << code;
    }
    auto transport_space = run_single(config);
    ASSERT_TRUE(transport_space.peer_close.has_value());
    transport_space.peer_close->space = moq::interop::transport::CloseErrorSpace::Transport;
    EXPECT_FALSE(scen::l06_goaway_single_inapplicable(transport_space));
}

}  // namespace
