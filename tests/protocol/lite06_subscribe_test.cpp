// The moq-lite-06 subscribe scenarios (L1d Task 6): every evaluator against a conforming scripted publisher and
// against publishers that violate exactly one rule, plus the NotRun conditions of the catalog rows
// (L06-6-3-2-MUST-093, L06-6-3-2-MUST-NOT-097, L06-7-19-SHOULD-190, L06-5-1-2-MUST-062, L06-3-6-MUST-023,
// L06-7-9-MUST-NOT-159, L06-7-13-MUST-172, L06-3-6-MUST-020).
//
// The subscribe behavior of the shared ConformingLitePublisher (offset reading of Group Start, no bound checks, two
// groups per subscription) is replaced here through LitePublisherHooks by a test publisher that reads Group Start
// raw (draft 3.6, moq.dev), honors Group/Frame End, resolves floors and frame starts, resets an invalid-bounds
// SUBSCRIBE and refuses unknown paths and tracks with a reset. Every defect is a hook in this file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_group_sequence_increments;
using moq::interop::scenarios::evaluate_l06_group_starts_with_group;
using moq::interop::scenarios::evaluate_l06_group_unique_sequence;
using moq::interop::scenarios::evaluate_l06_subscribe_invalid_frame_bounds_reset;
using moq::interop::scenarios::evaluate_l06_subscribe_no_group_below_floor;
using moq::interop::scenarios::evaluate_l06_subscribe_ok_group_at_floor;
using moq::interop::scenarios::evaluate_l06_subscribe_refused_reset;
using moq::interop::scenarios::evaluate_l06_subscribe_resolved_start;
using moq::interop::scenarios::judgeable;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteStep;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
namespace scen = moq::interop::scenarios;
namespace lite06 = moq::interop::scenarios::lite06;
namespace session = moq::interop::session;
namespace transport = moq::interop::transport;
namespace wire = moq::interop::wire;

using Verdict = std::optional<bool>;
using Evaluator = Verdict (*)(const LiteTranscript&);
using Triple = std::tuple<Verdict, Verdict, Verdict>;  // (093, 097, 190)
using Pair = std::pair<Verdict, Verdict>;              // (159, 172)

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 20000ms;
const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";
constexpr std::uint64_t kNotFound = 0x33;

const Verdict kPass{true};
const Verdict kFail{false};
const Verdict kNotRun{};

// Polls of the scripted publisher per second of the manual clock.
constexpr std::size_t polls(std::chrono::milliseconds at) { return static_cast<std::size_t>(at / kTick); }

// --- the test publisher -------------------------------------------------------------------------------------------

// One Group stream the publisher will open: GROUP{subscribe_id, sequence, frame_start} then `frames` FRAMEs
// (indices frame_start ...) and FIN.
struct Planned {
    std::uint64_t subscribe_id{0};
    std::uint64_t sequence{0};
    std::uint64_t frame_start{0};
    std::uint64_t frames{0};
};

Bytes group_stream(const Planned& group) {
    Bytes bytes = join({stream_type(0x0), group_header({group.subscribe_id, group.sequence, group.frame_start})});
    for (std::uint64_t f = 0; f < group.frames; ++f) {
        l06::Frame value;
        value.timestamp_delta = f == 0 ? 1000 : 33;
        value.payload = bytes_of("g" + std::to_string(group.sequence) + "f" + std::to_string(group.frame_start + f));
        bytes = join({bytes, frame(value)});
    }
    return bytes;
}

struct State;
using Action = std::function<void(ConformingLitePublisher&, ScriptedLitePeer&, State&)>;

struct State {
    std::size_t poll{0};
    std::deque<Planned> queue;  // one Group stream per poll, in order
    std::multimap<std::size_t, Action> later;
    std::vector<l06::Subscribe> subscribes;  // every SUBSCRIBE received, in order
    std::map<std::uint64_t, transport::StreamId> streams;  // Subscribe ID -> its stream

    // Runs `action` `delay` polls from now.
    void at(std::size_t delay, Action action) { later.emplace(poll + delay, std::move(action)); }
};

struct Script {
    std::uint64_t latest{5};
    std::size_t frames{3};
    std::size_t groups{3};  // per unbounded subscription
    // Groups held below the latest (draft 7.9): with a non-zero Subscriber Max Age the publisher starts an unfloored
    // subscription at its oldest held group, latest - history; with Max Age 0 at the latest group. A floor starts
    // at max(oldest held, F).
    std::uint64_t history{0};
    // Draft 2056-2058 reading of Group Start (offset by 1): start = max(latest, F - 1).
    bool offset_reading{false};
    // Replaces the answer to one SUBSCRIBE (true: answered).
    std::function<bool(ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId,
                       const l06::Subscribe&)>
        answer;
    // Edits the default answer before it is sent: the SUBSCRIBE_OK group (nullopt: none) and the planned groups.
    std::function<void(const l06::Subscribe&, std::optional<std::uint64_t>&, std::vector<Planned>&)> edit;
    // Runs on every poll before the group emission.
    Action on_poll;
};

// The default (conforming, raw reading) answer.
void default_answer(const Script& script, ConformingLitePublisher& publisher, ScriptedLitePeer& peer, State& state,
                    transport::StreamId stream, const l06::Subscribe& subscribe) {
    if (subscribe.broadcast_path != kBroadcast || subscribe.track_name != kTrack) {
        publisher.refuse(peer, stream, kNotFound);
        return;
    }
    const auto& range = subscribe.range;
    const std::uint64_t oldest = script.latest - std::min(script.history, script.latest);
    std::uint64_t start = range.subscriber_max_age_ms == 0 ? script.latest : oldest;
    if (range.group_start > 0)
        start = script.offset_reading ? std::max(oldest, range.group_start - 1) : std::max(oldest, range.group_start);
    const std::uint64_t first_frame = start == range.group_start ? range.frame_start : 0;
    const std::uint64_t last = range.group_end != 0 ? range.group_end - 1 : start + script.groups - 1;
    std::vector<Planned> groups;
    for (std::uint64_t sequence = start; script.groups > 0 && sequence <= last; ++sequence) {
        std::uint64_t total = script.frames;
        if (sequence == last && range.group_end != 0 && range.frame_end != 0)
            total = std::min<std::uint64_t>(total, range.frame_end);
        const std::uint64_t from = sequence == start ? first_frame : 0;
        groups.push_back({subscribe.subscribe_id, sequence, from, total > from ? total - from : 0});
        if (groups.size() >= script.groups) break;
    }
    std::optional<std::uint64_t> ok = start;
    if (script.edit) script.edit(subscribe, ok, groups);
    if (ok) peer.data(stream, subscribe_response(l06::SubscribeOk{*ok}));
    for (const auto& group : groups) state.queue.push_back(group);
}

ConformingLitePublisherConfig make_config(Script script, std::shared_ptr<State> state = std::make_shared<State>()) {
    ConformingLitePublisherConfig config;
    config.broadcast = kBroadcast;
    config.track = kTrack;
    auto shared = std::make_shared<Script>(std::move(script));
    config.hooks.on_request = [shared, state](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                              const LiteRunnerRequest& request) {
        if (!request.bidirectional || request.stream_type != 0x2) return false;
        const auto* subscribe = std::get_if<l06::Subscribe>(&request.message);
        if (!subscribe) {
            // decode_subscribe refuses a Frame End without a Group End (the only malformed SUBSCRIBE sent here):
            // draft 3.6, a protocol violation answered by resetting the stream.
            publisher.refuse(peer, request.stream, 0x0);
            return true;
        }
        state->subscribes.push_back(*subscribe);
        state->streams[subscribe->subscribe_id] = request.stream;
        if (shared->answer && shared->answer(publisher, peer, *state, request.stream, *subscribe)) return true;
        default_answer(*shared, publisher, peer, *state, request.stream, *subscribe);
        return true;
    };
    config.hooks.on_poll = [shared, state](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        ++state->poll;
        while (!state->later.empty() && state->later.begin()->first <= state->poll) {
            auto action = std::move(state->later.begin()->second);
            state->later.erase(state->later.begin());
            action(publisher, peer, *state);
            if (peer.peer_closed()) return;
        }
        if (shared->on_poll) shared->on_poll(publisher, peer, *state);
        if (peer.peer_closed() || state->queue.empty()) return;
        const auto group = state->queue.front();
        state->queue.pop_front();
        peer.data(peer.open_peer_uni(), group_stream(group), true);
    };
    return config;
}

LiteTranscript run(LiteProbeDefinition definition, ConformingLitePublisherConfig config,
                   const std::function<void(ScriptedLitePeer&)>& tweak = {}) {
    ConformingLitePublisher publisher(std::move(config));
    ScriptedLitePeer peer(publisher.reaction());
    if (tweak) tweak(peer);
    ManualLiteClock clock;
    return run_lite_probe(peer, std::move(definition), clock, kTick);
}
LiteTranscript run(LiteProbeDefinition definition, Script script = {}) {
    return run(std::move(definition), make_config(std::move(script)));
}

LiteProbeDefinition latest_probe() { return scen::l06_subscribe_latest_probe(kDeadline, kBroadcast, kTrack); }
LiteProbeDefinition refused_probe() { return scen::l06_subscribe_refused_probe(kDeadline, kBroadcast, kTrack); }
LiteProbeDefinition invalid_probe() {
    return scen::l06_subscribe_invalid_frame_bounds_probe(kDeadline, kBroadcast, kTrack);
}
LiteProbeDefinition floor_probe() { return scen::l06_subscribe_group_floor_probe(kDeadline, kBroadcast, kTrack); }
LiteProbeDefinition abutting_probe() {
    return scen::l06_subscribe_abutting_frame_start_probe(kDeadline, kBroadcast, kTrack);
}

std::vector<std::function<LiteProbeDefinition()>> every_probe() {
    return {latest_probe, refused_probe, invalid_probe, floor_probe, abutting_probe};
}

std::vector<Evaluator> all_evaluators() {
    return {evaluate_l06_group_starts_with_group,          evaluate_l06_group_unique_sequence,
            evaluate_l06_group_sequence_increments,        evaluate_l06_subscribe_refused_reset,
            evaluate_l06_subscribe_invalid_frame_bounds_reset, evaluate_l06_subscribe_no_group_below_floor,
            evaluate_l06_subscribe_ok_group_at_floor,      evaluate_l06_subscribe_resolved_start};
}

Triple judge_latest(const LiteTranscript& t) {
    return {evaluate_l06_group_starts_with_group(t), evaluate_l06_group_unique_sequence(t),
            evaluate_l06_group_sequence_increments(t)};
}
Pair judge_floor(const LiteTranscript& t) {
    return {evaluate_l06_subscribe_no_group_below_floor(t), evaluate_l06_subscribe_ok_group_at_floor(t)};
}

// The evaluators of every scenario but `id` (each must be NotRun on a transcript of `id`).
std::vector<Evaluator> foreign_evaluators(std::string_view id) {
    std::vector<Evaluator> out;
    if (id != scen::kL06SubscribeLatest) {
        out.push_back(evaluate_l06_group_starts_with_group);
        out.push_back(evaluate_l06_group_unique_sequence);
        out.push_back(evaluate_l06_group_sequence_increments);
    }
    if (id != scen::kL06SubscribeRefused) out.push_back(evaluate_l06_subscribe_refused_reset);
    if (id != scen::kL06SubscribeInvalidFrameBounds) out.push_back(evaluate_l06_subscribe_invalid_frame_bounds_reset);
    if (id != scen::kL06SubscribeGroupFloor) {
        out.push_back(evaluate_l06_subscribe_no_group_below_floor);
        out.push_back(evaluate_l06_subscribe_ok_group_at_floor);
    }
    if (id != scen::kL06SubscribeAbuttingFrameStart) out.push_back(evaluate_l06_subscribe_resolved_start);
    return out;
}

// Answers the SUBSCRIBE whose Subscribe ID is `id` with `answer`; the others get the default answer.
Script answering(std::uint64_t id,
                 std::function<void(ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId,
                                    const l06::Subscribe&)>
                     answer,
                 Script script = {}) {
    script.answer = [id, answer = std::move(answer)](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                                     State& state, transport::StreamId stream,
                                                     const l06::Subscribe& subscribe) {
        if (subscribe.subscribe_id != id) return false;
        answer(publisher, peer, state, stream, subscribe);
        return true;
    };
    return script;
}

// Leaves the SUBSCRIBE whose Subscribe ID is `id` pending (no answer at all).
Script pending(std::uint64_t id, Script script = {}) {
    return answering(id, [](auto&, auto&, auto&, auto, const auto&) {}, std::move(script));
}

// Edits the default answer of the SUBSCRIBE whose Subscribe ID is `id`.
Script editing(std::uint64_t id,
               std::function<void(std::optional<std::uint64_t>&, std::vector<Planned>&)> edit, Script script = {}) {
    script.edit = [id, edit = std::move(edit)](const l06::Subscribe& subscribe, std::optional<std::uint64_t>& ok,
                                              std::vector<Planned>& groups) {
        if (subscribe.subscribe_id == id) edit(ok, groups);
    };
    return script;
}

// Opens one extra publisher uni stream `delay` polls after the first SUBSCRIBE arrived.
Script with_extra_stream(std::size_t delay, Bytes bytes, bool fin = true, Script script = {}) {
    auto armed = std::make_shared<bool>(false);
    script.on_poll = [=](ConformingLitePublisher&, ScriptedLitePeer&, State& state) {
        if (*armed || state.subscribes.empty()) return;
        *armed = true;
        state.at(delay, [bytes, fin](ConformingLitePublisher&, ScriptedLitePeer& peer, State&) {
            peer.data(peer.open_peer_uni(), bytes, fin);
        });
    };
    return script;
}

const session::LiteStreamRecord* stream_of(const LiteTranscript& t, std::string_view label) {
    const auto* step = lite06::step_labelled(t, label);
    if (!step || !step->stream_id) return nullptr;
    return lite06::find_stream(t, *step->stream_id);
}

std::uint64_t floor_of(const LiteTranscript& t, std::string_view label) {
    const auto* step = lite06::step_labelled(t, label);
    if (!step) throw std::logic_error("no floor step");
    const auto subscribe = scen::l06_decode_subscribe_stimulus(step->bytes);
    if (!subscribe) throw std::logic_error("undecodable floor step");
    return subscribe->range.group_start;
}

double ended_after_s(const LiteTranscript& t) {
    return static_cast<double>(t.ended_ns - t.established_ns) / 1e9;
}

// --- helpers and probe structure ----------------------------------------------------------------------------------

TEST(Lite06SubscribeCommon, BuildersRejectAShortDeadlineAndAMissingFixture) {
    // latest: answer allowance + window; refused: answer allowance + allowance; invalid: allowance;
    // floor: learning allowance + window; abutting: two step allowances + window.
    EXPECT_THROW(scen::l06_subscribe_latest_probe(9000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_subscribe_latest_probe(9001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_subscribe_refused_probe(6000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_subscribe_refused_probe(6001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_subscribe_invalid_frame_bounds_probe(3000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_subscribe_invalid_frame_bounds_probe(3001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_subscribe_group_floor_probe(9000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_subscribe_group_floor_probe(9001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_subscribe_abutting_frame_start_probe(12000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_subscribe_abutting_frame_start_probe(12001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_subscribe_latest_probe(kDeadline, kBroadcast, kTrack, 6000ms, 0ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_subscribe_group_floor_probe(kDeadline, kBroadcast, kTrack, 6000ms, 0ms),
                 std::invalid_argument);
    for (const auto& [path, track] :
         {std::pair<std::string, std::string>{"", kTrack}, {"/", kTrack}, {kBroadcast, ""}}) {
        EXPECT_THROW(scen::l06_subscribe_latest_probe(kDeadline, path, track), std::invalid_argument);
        EXPECT_THROW(scen::l06_subscribe_refused_probe(kDeadline, path, track), std::invalid_argument);
        EXPECT_THROW(scen::l06_subscribe_invalid_frame_bounds_probe(kDeadline, path, track), std::invalid_argument);
        EXPECT_THROW(scen::l06_subscribe_group_floor_probe(kDeadline, path, track), std::invalid_argument);
        EXPECT_THROW(scen::l06_subscribe_abutting_frame_start_probe(kDeadline, path, track), std::invalid_argument);
    }
}

TEST(Lite06SubscribeCommon, ProbeStimuli) {
    using Kind = LiteStep::Kind;
    for (const auto& probe : every_probe()) {
        const auto definition = probe();
        EXPECT_TRUE(definition.requires_track) << definition.id;
        EXPECT_EQ(definition.broadcast_path, kBroadcast);
        EXPECT_EQ(definition.track_name, kTrack);
        EXPECT_EQ(definition.observation_window, 0ms);
        EXPECT_FALSE(definition.done) << definition.id;  // never ends early on a condition
    }
    // latest: announce exchange, default SUBSCRIBE with a large Max Age, the ungated window.
    const auto latest = latest_probe();
    ASSERT_EQ(latest.steps.size(), 4u);
    EXPECT_EQ(latest.steps[0].bytes, scen::l06_announce_request_bytes(""));
    EXPECT_EQ(latest.steps[1].kind, Kind::Wait);
    EXPECT_TRUE(static_cast<bool>(latest.steps[1].gate));
    EXPECT_EQ(latest.steps[1].gate_deadline, scen::kLiteResponseAllowance);
    const auto subscribe = scen::l06_decode_subscribe_stimulus(latest.steps[2].bytes);
    ASSERT_TRUE(subscribe.has_value());
    EXPECT_EQ(subscribe->subscribe_id, scen::kL06LatestSubscribeId);
    EXPECT_EQ(subscribe->broadcast_path, kBroadcast);
    EXPECT_EQ(subscribe->track_name, kTrack);
    EXPECT_EQ(subscribe->range.subscriber_max_age_ms, scen::kL06LargeMaxAgeMs);
    EXPECT_EQ(subscribe->range.group_start, 0u);
    EXPECT_EQ(subscribe->range.group_end, 0u);
    EXPECT_EQ(subscribe->range.frame_start, 0u);
    EXPECT_EQ(subscribe->range.frame_end, 0u);
    EXPECT_EQ(latest.steps[3].label, lite06::kAllowanceLabel);
    EXPECT_FALSE(static_cast<bool>(latest.steps[3].gate));
    EXPECT_EQ(latest.steps[3].delay, scen::kLiteObservationWindow);
    // refused: the two SUBSCRIBEs are consecutive and ungated.
    const auto refused = refused_probe();
    ASSERT_EQ(refused.steps.size(), 5u);
    const auto uncovered = scen::l06_decode_subscribe_stimulus(refused.steps[2].bytes);
    const auto unknown = scen::l06_decode_subscribe_stimulus(refused.steps[3].bytes);
    ASSERT_TRUE(uncovered && unknown);
    EXPECT_EQ(uncovered->broadcast_path, "l1d-disjoint/l1d-unserved");
    EXPECT_EQ(uncovered->track_name, kTrack);
    EXPECT_EQ(unknown->broadcast_path, kBroadcast);
    EXPECT_EQ(unknown->track_name, "video-l1d-unknown");
    EXPECT_NE(uncovered->subscribe_id, unknown->subscribe_id);
    EXPECT_FALSE(static_cast<bool>(refused.steps[2].gate));
    EXPECT_FALSE(static_cast<bool>(refused.steps[3].gate));
    EXPECT_EQ(refused.steps[4].delay, scen::kLiteResponseAllowance);
    // invalid bounds: the raw SUBSCRIBE that decode_subscribe refuses.
    const auto invalid = invalid_probe();
    ASSERT_EQ(invalid.steps.size(), 2u);
    EXPECT_EQ(invalid.steps[0].bytes, scen::l06_invalid_bounds_subscribe_bytes(kBroadcast, kTrack));
    EXPECT_FALSE(scen::l06_decode_subscribe_stimulus(invalid.steps[0].bytes).has_value());
    auto valid = scen::l06_subscribe(scen::kL06InvalidSubscribeId, kBroadcast, kTrack);
    EXPECT_FALSE(scen::l06_subscribe_bytes(valid).empty());
    // The raw bytes differ from the valid SUBSCRIBE only in the last field (Frame End 0 -> 1).
    auto raw = scen::l06_invalid_bounds_subscribe_bytes(kBroadcast, kTrack);
    auto good = scen::l06_subscribe_bytes(valid);
    ASSERT_EQ(raw.size(), good.size());
    EXPECT_EQ(raw.back(), std::byte{0x1});
    raw.back() = std::byte{0x0};
    EXPECT_EQ(raw, good);
    EXPECT_EQ(invalid.steps[1].delay, scen::kLiteResponseAllowance);
    // floor and abutting: one static learning SUBSCRIBE and a continuation.
    for (const auto& definition : {floor_probe(), abutting_probe()}) {
        ASSERT_EQ(definition.steps.size(), 1u);
        EXPECT_EQ(definition.steps[0].label, scen::kL06SubscribeLearnLabel);
        EXPECT_EQ(definition.steps[0].bytes,
                  scen::l06_learning_subscribe_bytes(kBroadcast, kTrack));
        EXPECT_TRUE(static_cast<bool>(definition.next_steps));
    }
}

// --- l06-subscribe-latest (093, 097, 190) -------------------------------------------------------------------------

TEST(Lite06SubscribeLatest, ConformingPublisherPassesAllThreeAfterTheWholeWindow) {
    const auto t = run(latest_probe());
    EXPECT_EQ(judge_latest(t), Triple(kPass, kPass, kPass));
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_GE(ended_after_s(t), 6.0);
    EXPECT_TRUE(lite06::allowance_elapsed(t));
}

TEST(Lite06SubscribeLatest, AGroupStreamNotStartingWithGroupFailsOnly093) {
    // STREAM_TYPE Group then a FRAME: decoded as a GROUP it breaks (a PeerProtocol issue on that stream).
    l06::Frame value;
    value.payload = bytes_of("hello");
    const auto t = run(latest_probe(), with_extra_stream(2, join({stream_type(0x0), frame(value)})));
    bool issue = false;
    for (const auto* record : lite06::peer_streams(t))
        if (record->kind == session::LiteStreamKind::Group && !session::peer_protocol_issues(*record).empty())
            issue = true;
    EXPECT_TRUE(issue);
    EXPECT_EQ(judge_latest(t), Triple(kFail, kPass, kPass));
}

TEST(Lite06SubscribeLatest, AGroupStreamEndingWithoutGroupFailsOnly093) {
    const auto t = run(latest_probe(), with_extra_stream(2, stream_type(0x0)));
    EXPECT_EQ(judge_latest(t), Triple(kFail, kPass, kPass));
}

TEST(Lite06SubscribeLatest, AGroupStreamResetBeforeAnyByteIsSkipped) {
    // STREAM_TYPE Group, nothing more, then a RESET_STREAM three polls later.
    Script plain;
    auto armed = std::make_shared<std::optional<transport::StreamId>>();
    plain.on_poll = [armed](ConformingLitePublisher&, ScriptedLitePeer& peer, State& state) {
        if (state.subscribes.empty() || armed->has_value()) return;
        const auto id = peer.open_peer_uni();
        *armed = id;
        peer.data(id, stream_type(0x0));
        state.at(3, [id](ConformingLitePublisher&, ScriptedLitePeer& p, State&) { p.peer_reset(id, 0x0); });
    };
    const auto t = run(latest_probe(), plain);
    bool reset = false;
    for (const auto* record : lite06::peer_streams(t))
        if (record->kind == session::LiteStreamKind::Group && record->reset_seen) reset = true;
    EXPECT_TRUE(reset);
    EXPECT_EQ(judge_latest(t), Triple(kPass, kPass, kPass));
}

TEST(Lite06SubscribeLatest, AGroupStreamResetAfterPartOfItsGroupIsSkipped) {
    // STREAM_TYPE Group and the first two bytes of a GROUP, then a RESET_STREAM: no GROUP decoded, no issue.
    Script partial;
    auto armed = std::make_shared<bool>(false);
    partial.on_poll = [armed](ConformingLitePublisher&, ScriptedLitePeer& peer, State& state) {
        if (state.subscribes.empty() || *armed) return;
        *armed = true;
        const auto id = peer.open_peer_uni();
        const auto header = group_header({0, 9, 0});
        peer.data(id, join({stream_type(0x0), Bytes(header.begin(), header.begin() + 2)}));
        state.at(3, [id](ConformingLitePublisher&, ScriptedLitePeer& p, State&) { p.peer_reset(id, 0x0); });
    };
    const auto t = run(latest_probe(), partial);
    bool skipped = false;
    for (const auto* record : lite06::peer_streams(t))
        if (record->kind == session::LiteStreamKind::Group && record->reset_seen &&
            session::peer_messages(*record).empty() && session::peer_protocol_issues(*record).empty() &&
            record->bytes > 1)
            skipped = true;
    EXPECT_TRUE(skipped);
    // 190 sees an undecoded stream but no gap (5, 6, 7), so it still passes.
    EXPECT_EQ(judge_latest(t), Triple(kPass, kPass, kPass));
}

TEST(Lite06SubscribeLatest, AnUnansweredAnnounceRequestLeavesTheGroupRowsNotRun) {
    // The announce wait expires: the runner still subscribes and receives groups, but the stimulus (an announce
    // exchange, then a SUBSCRIBE to an announced track) was not delivered in full.
    auto config = make_config({});
    config.defect = LiteDefect::SilentOnAnnounce;
    const auto t = run(latest_probe(), config);
    const auto* announced = lite06::step_labelled(t, scen::kL06SubAnnouncedLabel);
    ASSERT_TRUE(announced);
    EXPECT_TRUE(announced->gate_expired);
    EXPECT_FALSE(t.stimulus_delivered);
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_latest(t), Triple(kNotRun, kNotRun, kNotRun));
}

TEST(Lite06SubscribeLatest, ADuplicateGroupSequenceFailsOnly097) {
    const auto t = run(latest_probe(), editing(0, [](auto&, std::vector<Planned>& groups) {
                           groups.insert(groups.begin() + 2, groups[1]);  // 5, 6, 6, 7
                       }));
    EXPECT_EQ(judge_latest(t), Triple(kPass, kFail, kPass));
}

TEST(Lite06SubscribeLatest, ALateDuplicateIsStillSeen) {
    // The first three groups are consecutive and unique; the duplicate of 5 arrives 5 s later.
    auto script = editing(0, [](auto&, std::vector<Planned>&) {});
    script.on_poll = [armed = std::make_shared<bool>(false)](ConformingLitePublisher&, ScriptedLitePeer&,
                                                              State& state) {
        if (*armed || state.subscribes.empty()) return;
        *armed = true;
        state.at(polls(5000ms), [](ConformingLitePublisher&, ScriptedLitePeer& peer, State&) {
            peer.data(peer.open_peer_uni(), group_stream({0, 5, 0, 1}), true);
        });
    };
    const auto t = run(latest_probe(), script);
    EXPECT_EQ(judge_latest(t), Triple(kPass, kFail, kPass));
}

TEST(Lite06SubscribeLatest, ANumberingGapFailsOnly190) {
    const auto t = run(latest_probe(), editing(0, [](auto&, std::vector<Planned>& groups) {
                           groups[1].sequence = 7;
                           groups[2].sequence = 8;  // 5, 7, 8
                       }));
    EXPECT_EQ(judge_latest(t), Triple(kPass, kPass, kFail));
}

TEST(Lite06SubscribeLatest, AGapCoveredByASubscribeDropPasses) {
    auto script = editing(0, [](auto&, std::vector<Planned>& groups) {
        groups[1].sequence = 7;
        groups[2].sequence = 8;
    });
    script.on_poll = [armed = std::make_shared<bool>(false)](ConformingLitePublisher&, ScriptedLitePeer& peer,
                                                              State& state) {
        if (*armed || !state.streams.contains(0)) return;
        *armed = true;
        peer.data(state.streams[0], subscribe_response(l06::SubscribeDrop{6, 6, 0}));
    };
    const auto t = run(latest_probe(), script);
    EXPECT_EQ(judge_latest(t), Triple(kPass, kPass, kPass));
}

TEST(Lite06SubscribeLatest, AGapNextToAnUndecodedResetStreamIsInconclusive) {
    auto script = editing(0, [](auto&, std::vector<Planned>& groups) {
        groups[1].sequence = 7;
        groups[2].sequence = 8;
    });
    script.on_poll = [armed = std::make_shared<bool>(false)](ConformingLitePublisher&, ScriptedLitePeer& peer,
                                                              State& state) {
        if (*armed || state.subscribes.empty()) return;
        *armed = true;
        const auto id = peer.open_peer_uni();
        peer.data(id, stream_type(0x0));
        peer.peer_reset(id, 0x0);  // group 6, reset before its GROUP decoded
    };
    const auto t = run(latest_probe(), script);
    EXPECT_EQ(judge_latest(t), Triple(kPass, kPass, kNotRun));
}

TEST(Lite06SubscribeLatest, FewerThanTwoGroupsIsNotRunFor097And190) {
    Script one;
    one.groups = 1;
    EXPECT_EQ(judge_latest(run(latest_probe(), one)), Triple(kPass, kNotRun, kNotRun));
}

TEST(Lite06SubscribeLatest, NoGroupStreamIsNotRunForAllThree) {
    Script none;
    none.groups = 0;
    const auto t = run(latest_probe(), none);
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_latest(t), Triple(kNotRun, kNotRun, kNotRun));
}

TEST(Lite06SubscribeLatest, GroupsOfAnotherSubscribeIdAreNotCounted) {
    // A stray GROUP naming Subscribe ID 9 with a sequence already delivered: neither a duplicate nor a gap here.
    const auto t = run(latest_probe(), with_extra_stream(4, group_stream({9, 5, 0, 1})));
    EXPECT_EQ(judge_latest(t), Triple(kPass, kPass, kPass));
}

TEST(Lite06SubscribeLatest, ASlowFirstGroupInsideTheWindowPasses) {
    auto script = answering(0, [](ConformingLitePublisher&, ScriptedLitePeer&, State& state,
                                  transport::StreamId stream, const l06::Subscribe&) {
        state.at(polls(4000ms), [stream](ConformingLitePublisher&, ScriptedLitePeer& p, State& s) {
            p.data(stream, subscribe_response(l06::SubscribeOk{5}));
            for (std::uint64_t g = 5; g < 8; ++g) s.queue.push_back({0, g, 0, 2});
        });
    });
    EXPECT_EQ(judge_latest(run(latest_probe(), script)), Triple(kPass, kPass, kPass));
}

// --- l06-subscribe-refused (062) ----------------------------------------------------------------------------------

TEST(Lite06SubscribeRefused, ConformingPublisherResetsBoth) {
    const auto t = run(refused_probe());
    const auto* a = stream_of(t, scen::kL06SubscribeUncoveredLabel);
    const auto* b = stream_of(t, scen::kL06SubscribeUnknownTrackLabel);
    ASSERT_TRUE(a && b);
    EXPECT_TRUE(a->reset_seen);
    EXPECT_TRUE(b->reset_seen);
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(t), kPass);
    EXPECT_GE(ended_after_s(t), 3.0);
}

TEST(Lite06SubscribeRefused, AnUncoveredPathLeftPendingFails) {
    const auto t = run(refused_probe(), pending(scen::kL06UncoveredSubscribeId));
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(lite06::allowance_elapsed(t));
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(t), kFail);
}

TEST(Lite06SubscribeRefused, AnUnknownTrackLeftPendingIsNotRun) {
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(run(refused_probe(), pending(scen::kL06UnknownTrackSubscribeId))),
              kNotRun);
}

TEST(Lite06SubscribeRefused, ASessionCloseInsteadOfTheResetFails) {
    for (const auto id : {scen::kL06UncoveredSubscribeId, scen::kL06UnknownTrackSubscribeId}) {
        const auto t = run(refused_probe(), answering(id, [](ConformingLitePublisher& publisher,
                                                             ScriptedLitePeer& peer, auto&, auto, const auto&) {
                               publisher.close(peer, 0x3);
                           }));
        ASSERT_TRUE(t.peer_close.has_value());
        EXPECT_EQ(evaluate_l06_subscribe_refused_reset(t), kFail) << id;
    }
}

TEST(Lite06SubscribeRefused, ASubscribeEndOrFinInsteadOfTheResetFails) {
    for (const auto id : {scen::kL06UncoveredSubscribeId, scen::kL06UnknownTrackSubscribeId}) {
        const auto end = run(refused_probe(), answering(id, [](auto&, ScriptedLitePeer& peer, auto&,
                                                               transport::StreamId stream, const auto&) {
                                 peer.data(stream, subscribe_response(l06::SubscribeEnd{0}), true);
                             }));
        EXPECT_EQ(evaluate_l06_subscribe_refused_reset(end), kFail) << id;
        const auto fin = run(refused_probe(), answering(id, [](auto&, ScriptedLitePeer& peer, auto&,
                                                               transport::StreamId stream, const auto&) {
                                 peer.fin(stream);
                             }));
        EXPECT_EQ(evaluate_l06_subscribe_refused_reset(fin), kFail) << id;
    }
}

TEST(Lite06SubscribeRefused, ServingTheUncoveredPathFailsAndServingTheUnknownTrackIsNotRun) {
    const auto serve = [](ConformingLitePublisher&, ScriptedLitePeer& peer, State& state, transport::StreamId stream,
                          const l06::Subscribe& subscribe) {
        peer.data(stream, subscribe_response(l06::SubscribeOk{5}));
        state.queue.push_back({subscribe.subscribe_id, 5, 0, 1});
    };
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(run(refused_probe(), answering(0, serve))), kFail);
    // The track may exist after all: case (b) cannot be shown to be a refusal case.
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(run(refused_probe(), answering(1, serve))), kNotRun);
}

TEST(Lite06SubscribeRefused, ASubscribeEndFollowedByTheResetPasses) {
    // END then RESET_STREAM is a refusal by reset (the END is information; a reset may discard it in flight).
    for (const auto id : {scen::kL06UncoveredSubscribeId, scen::kL06UnknownTrackSubscribeId}) {
        const auto t = run(refused_probe(), answering(id, [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                                             auto&, transport::StreamId stream, const auto&) {
                               peer.data(stream, subscribe_response(l06::SubscribeEnd{0}));
                               publisher.refuse(peer, stream, kNotFound);
                           }));
        EXPECT_EQ(evaluate_l06_subscribe_refused_reset(t), kPass) << id;
        // A reset arriving later in the allowance after the END passes too.
        const auto late = run(refused_probe(), answering(id, [](auto&, ScriptedLitePeer& peer, State& state,
                                                                transport::StreamId stream, const auto&) {
                                  peer.data(stream, subscribe_response(l06::SubscribeEnd{0}));
                                  state.at(polls(2000ms), [stream](ConformingLitePublisher& publisher,
                                                                   ScriptedLitePeer& p, State&) {
                                      publisher.refuse(p, stream, kNotFound);
                                  });
                              }));
        EXPECT_EQ(evaluate_l06_subscribe_refused_reset(late), kPass) << id;
    }
}

TEST(Lite06SubscribeRefused, ASubscribeEndLeftWithoutAResetFails) {
    for (const auto id : {scen::kL06UncoveredSubscribeId, scen::kL06UnknownTrackSubscribeId}) {
        // END, then nothing until the allowance elapsed (also for the unknown track: END is no refusal).
        const auto pending_end = run(refused_probe(), answering(id, [](auto&, ScriptedLitePeer& peer, auto&,
                                                                       transport::StreamId stream, const auto&) {
                                         peer.data(stream, subscribe_response(l06::SubscribeEnd{0}));
                                     }));
        EXPECT_TRUE(lite06::allowance_elapsed(pending_end));
        EXPECT_EQ(evaluate_l06_subscribe_refused_reset(pending_end), kFail) << id;
        // END, then a session close.
        const auto closed = run(refused_probe(), answering(id, [](auto&, ScriptedLitePeer& peer, State& state,
                                                                  transport::StreamId stream, const auto&) {
                                    peer.data(stream, subscribe_response(l06::SubscribeEnd{0}));
                                    state.at(5, [](ConformingLitePublisher& pub, ScriptedLitePeer& p, State&) {
                                        pub.close(p, 0x0);
                                    });
                                }));
        ASSERT_TRUE(closed.peer_close.has_value());
        EXPECT_EQ(evaluate_l06_subscribe_refused_reset(closed), kFail) << id;
    }
}

TEST(Lite06SubscribeRefused, ASlowResetInsideTheAllowancePasses) {
    const auto t = run(refused_probe(), answering(0, [](auto&, ScriptedLitePeer&, State& state,
                                                       transport::StreamId stream, const auto&) {
                           state.at(polls(2500ms), [stream](ConformingLitePublisher& publisher,
                                                            ScriptedLitePeer& peer, State&) {
                               publisher.refuse(peer, stream, kNotFound);
                           });
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(t), kPass);
}

TEST(Lite06SubscribeRefused, AResetAfterAGroupStillFails) {
    // A Group stream for the uncovered subscription, then the reset: served, not refused.
    const auto t = run(refused_probe(), answering(0, [](ConformingLitePublisher&, ScriptedLitePeer&, State& state,
                                                       transport::StreamId stream, const l06::Subscribe&) {
                           state.queue.push_back({0, 5, 0, 1});
                           state.at(polls(1000ms), [stream](ConformingLitePublisher& publisher,
                                                            ScriptedLitePeer& peer, State&) {
                               publisher.refuse(peer, stream, kNotFound);
                           });
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(t), kFail);
}

TEST(Lite06SubscribeRefused, AnAnnouncedRouteCoveringThePathMakesPendingInconclusive) {
    // The publisher announces a route covering the "uncovered" path too, and leaves that SUBSCRIBE pending.
    auto config = make_config(pending(scen::kL06UncoveredSubscribeId));
    const auto subscribe_hook = config.hooks.on_request;
    config.hooks.on_request = [subscribe_hook](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                               const LiteRunnerRequest& request) {
        if (std::holds_alternative<l06::AnnounceRequest>(request.message)) {
            l06::AnnounceStart first;
            first.suffix = kBroadcast;
            l06::AnnounceStart second;
            second.suffix = "l1d-disjoint";
            peer.data(request.stream,
                      join({announce_ok({7, 2}), announce_message(first), announce_message(second)}));
            return true;
        }
        return subscribe_hook(publisher, peer, request);
    };
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(run(refused_probe(), config)), kNotRun);
}

TEST(Lite06SubscribeRefused, NoAnnounceAnswerStillJudgesTheResets) {
    auto config = make_config({});
    config.defect = LiteDefect::SilentOnAnnounce;
    const auto t = run(refused_probe(), config);
    const auto* announced = lite06::step_labelled(t, scen::kL06SubAnnouncedLabel);
    ASSERT_TRUE(announced);
    EXPECT_TRUE(announced->gate_expired);
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(t), kPass);
    // Without the announce exchange the "uncovered" path cannot be shown uncovered: pending is NotRun.
    auto silent = make_config(pending(scen::kL06UncoveredSubscribeId));
    silent.defect = LiteDefect::SilentOnAnnounce;
    EXPECT_EQ(evaluate_l06_subscribe_refused_reset(run(refused_probe(), silent)), kNotRun);
}

// --- l06-subscribe-invalid-frame-bounds (023) ---------------------------------------------------------------------

TEST(Lite06SubscribeInvalidBounds, AResetPassesAfterTheWholeAllowance) {
    ConformingLitePublisher publisher(make_config({}));
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, invalid_probe(), clock, kTick);
    // The publisher received Group End 0 with Frame End 1.
    const auto* runner = peer.runner_stream(1);
    ASSERT_TRUE(runner);
    EXPECT_EQ(runner->bytes, scen::l06_invalid_bounds_subscribe_bytes(kBroadcast, kTrack));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kPass);
    EXPECT_GE(ended_after_s(t), 3.0);
}

// Replaces the invalid-bounds handling (the SUBSCRIBE arrives undecodable to the hook as monostate).
ConformingLitePublisherConfig invalid_answer(
    std::function<void(ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId)> answer) {
    auto state = std::make_shared<State>();
    auto config = make_config({}, state);
    config.hooks.on_request = [state, answer](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                              const LiteRunnerRequest& request) {
        if (!request.bidirectional || request.stream_type != 0x2) return false;
        answer(publisher, peer, *state, request.stream);
        return true;
    };
    return config;
}

TEST(Lite06SubscribeInvalidBounds, AcceptingTheInvalidBoundsFails) {
    const auto t = run(invalid_probe(), invalid_answer([](auto&, ScriptedLitePeer& peer, State& state,
                                                          transport::StreamId stream) {
                           peer.data(stream, subscribe_response(l06::SubscribeOk{5}));
                           state.queue.push_back({0, 5, 0, 2});
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kFail);
    // A Group stream alone (no SUBSCRIBE_OK) fails too.
    const auto groups = run(invalid_probe(), invalid_answer([](auto&, ScriptedLitePeer&, State& state, auto) {
                                state.queue.push_back({0, 5, 0, 2});
                            }));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(groups), kFail);
}

TEST(Lite06SubscribeInvalidBounds, NoReactionFailsOnceTheAllowanceElapsed) {
    const auto t = run(invalid_probe(), invalid_answer([](auto&, auto&, auto&, auto) {}));
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(lite06::allowance_elapsed(t));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kFail);
}

TEST(Lite06SubscribeInvalidBounds, ASessionCloseInsteadOfTheResetFails) {
    // The shared publisher with its named defect: close with PROTOCOL_VIOLATION on the undecodable SUBSCRIBE.
    ConformingLitePublisherConfig config;
    config.broadcast = kBroadcast;
    config.track = kTrack;
    config.defect = LiteDefect::CloseOnInvalidSubscribe;
    const auto t = run(invalid_probe(), config);
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kFail);
}

TEST(Lite06SubscribeInvalidBounds, ASubscribeEndOrFinInsteadOfTheResetFails) {
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(
                  run(invalid_probe(), invalid_answer([](auto&, ScriptedLitePeer& peer, auto&,
                                                         transport::StreamId stream) {
                          peer.data(stream, subscribe_response(l06::SubscribeEnd{0}), true);
                      }))),
              kFail);
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(
                  run(invalid_probe(), invalid_answer([](auto&, ScriptedLitePeer& peer, auto&,
                                                         transport::StreamId stream) { peer.fin(stream); }))),
              kFail);
}

TEST(Lite06SubscribeInvalidBounds, ASubscribeEndFollowedByTheResetPasses) {
    const auto t = run(invalid_probe(), invalid_answer([](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                                          auto&, transport::StreamId stream) {
                           peer.data(stream, subscribe_response(l06::SubscribeEnd{0}));
                           publisher.refuse(peer, stream, 0x0);
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kPass);
    // END alone, no reset within the allowance: no substitute.
    const auto alone = run(invalid_probe(), invalid_answer([](auto&, ScriptedLitePeer& peer, auto&,
                                                              transport::StreamId stream) {
                               peer.data(stream, subscribe_response(l06::SubscribeEnd{0}));
                           }));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(alone), kFail);
}

TEST(Lite06SubscribeInvalidBounds, AGroupAfterTheResetIsStillSeen) {
    const auto t = run(invalid_probe(), invalid_answer([](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                                          State& state, transport::StreamId stream) {
                           publisher.refuse(peer, stream, 0x0);
                           state.at(polls(2000ms), [](ConformingLitePublisher&, ScriptedLitePeer& p, State&) {
                               p.data(p.open_peer_uni(), group_stream({0, 5, 0, 1}), true);
                           });
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kFail);
}

TEST(Lite06SubscribeInvalidBounds, ASlowResetInsideTheAllowancePasses) {
    const auto t = run(invalid_probe(), invalid_answer([](auto&, auto&, State& state, transport::StreamId stream) {
                           state.at(polls(2500ms), [stream](ConformingLitePublisher& publisher,
                                                            ScriptedLitePeer& peer, State&) {
                               publisher.refuse(peer, stream, 0x5);
                           });
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kPass);
}

TEST(Lite06SubscribeInvalidBounds, AStopSendingAloneIsNoReset) {
    const auto t = run(invalid_probe(), invalid_answer([](auto&, ScriptedLitePeer& peer, auto&,
                                                          transport::StreamId stream) {
                           peer.peer_stop_sending(stream, 0x0);
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_invalid_frame_bounds_reset(t), kFail);
}

// --- l06-subscribe-group-floor (159, 172) -------------------------------------------------------------------------

TEST(Lite06SubscribeGroupFloor, ConformingPublisherPassesBoth) {
    const auto t = run(floor_probe());
    EXPECT_EQ(floor_of(t, scen::kL06FloorAtLatestLabel), 5u);
    EXPECT_EQ(floor_of(t, scen::kL06FloorAboveLabel), 7u);
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(lite06::allowance_elapsed(t));
    EXPECT_EQ(judge_floor(t), Pair(kPass, kPass));
}

TEST(Lite06SubscribeGroupFloor, AGroupBelowTheFloorFailsOnly159) {
    const auto t = run(floor_probe(), editing(scen::kL06FloorAboveSubscribeId, [](auto&, std::vector<Planned>& g) {
                           g.push_back({scen::kL06FloorAboveSubscribeId, 5, 0, 1});  // F = 7, 5 < F - 1
                       }));
    EXPECT_EQ(judge_floor(t), Pair(kFail, kPass));
}

TEST(Lite06SubscribeGroupFloor, ALateGroupBelowTheFloorIsStillSeen) {
    auto script = editing(scen::kL06FloorAtLatestSubscribeId, [](auto&, std::vector<Planned>&) {});
    script.on_poll = [armed = std::make_shared<bool>(false)](ConformingLitePublisher&, ScriptedLitePeer&,
                                                              State& state) {
        if (*armed || !state.streams.contains(scen::kL06FloorAtLatestSubscribeId)) return;
        *armed = true;
        state.at(polls(5000ms), [](ConformingLitePublisher&, ScriptedLitePeer& peer, State&) {
            peer.data(peer.open_peer_uni(), group_stream({scen::kL06FloorAtLatestSubscribeId, 2, 0, 1}), true);
        });
    };
    EXPECT_EQ(judge_floor(run(floor_probe(), script)), Pair(kFail, kPass));
}

TEST(Lite06SubscribeGroupFloor, WithHistoryTheLearningSubscriptionStillLearnsTheLatest) {
    // The publisher holds three groups below its latest (5): a large Max Age would start at 2, Max Age 0 at 5.
    Script history;
    history.history = 3;
    const auto t = run(floor_probe(), history);
    const auto* learn = lite06::step_labelled(t, scen::kL06SubscribeLearnLabel);
    ASSERT_TRUE(learn);
    const auto learning = scen::l06_decode_subscribe_stimulus(learn->bytes);
    ASSERT_TRUE(learning.has_value());
    EXPECT_EQ(learning->range.subscriber_max_age_ms, 0u);
    EXPECT_EQ(floor_of(t, scen::kL06FloorAtLatestLabel), 5u);
    EXPECT_EQ(floor_of(t, scen::kL06FloorAboveLabel), 7u);
    // The floored subscriptions keep the large Max Age.
    const auto floored = scen::l06_decode_subscribe_stimulus(
        lite06::step_labelled(t, scen::kL06FloorAtLatestLabel)->bytes);
    ASSERT_TRUE(floored.has_value());
    EXPECT_EQ(floored->range.subscriber_max_age_ms, scen::kL06LargeMaxAgeMs);
    EXPECT_EQ(judge_floor(t), Pair(kPass, kPass));
}

TEST(Lite06SubscribeGroupFloor, ALargeMaxAgeLearningSubscriptionWouldMisLearn) {
    // The old behavior: the same probe with a large-Max-Age learning SUBSCRIBE learns the OLDEST held group.
    Script history;
    history.history = 3;
    auto definition = floor_probe();
    definition.steps[0].bytes =
        scen::l06_subscribe_bytes(scen::l06_subscribe(scen::kL06LearnSubscribeId, kBroadcast, kTrack));
    const auto t = run(std::move(definition), history);
    EXPECT_EQ(floor_of(t, scen::kL06FloorAtLatestLabel), 2u);  // L = 2, not the latest 5
    // Such a transcript is not the stimulus the evaluators expect: NotRun.
    EXPECT_EQ(judge_floor(t), Pair(kNotRun, kNotRun));
}

TEST(Lite06SubscribeGroupFloor, ASubscribeOkBelowTheFloorFailsOnly172) {
    const auto t = run(floor_probe(), editing(scen::kL06FloorAboveSubscribeId,
                                              [](std::optional<std::uint64_t>& ok, auto&) { ok = 5; }));
    EXPECT_EQ(judge_floor(t), Pair(kPass, kFail));
}

TEST(Lite06SubscribeGroupFloor, TheOffsetReadingIsInconclusive) {
    // start = max(L, F - 1): the F = L + 2 subscription resolves to exactly F - 1 (OK and first group).
    Script offset;
    offset.offset_reading = true;
    EXPECT_EQ(judge_floor(run(floor_probe(), offset)), Pair(kNotRun, kNotRun));
    // A value below F - 1 is wrong under both readings.
    auto worse = editing(scen::kL06FloorAboveSubscribeId, [](std::optional<std::uint64_t>& ok,
                                                             std::vector<Planned>& groups) {
        ok = 5;
        for (auto& g : groups) g.sequence -= 1;  // 5, 6, 7
    }, offset);
    EXPECT_EQ(judge_floor(run(floor_probe(), worse)), Pair(kFail, kFail));
}

TEST(Lite06SubscribeGroupFloor, LatestGroupZeroJudgesOnlyTheFloorAbove) {
    Script zero;
    zero.latest = 0;
    const auto t = run(floor_probe(), zero);
    EXPECT_EQ(floor_of(t, scen::kL06FloorAtLatestLabel), 0u);
    EXPECT_EQ(floor_of(t, scen::kL06FloorAboveLabel), 2u);
    EXPECT_EQ(judge_floor(t), Pair(kPass, kPass));
    // With L = 0 and the F = 2 subscription pending, nothing can be judged (the F = 0 one has no floor).
    EXPECT_EQ(judge_floor(run(floor_probe(), pending(scen::kL06FloorAboveSubscribeId, zero))),
              Pair(kNotRun, kNotRun));
}

TEST(Lite06SubscribeGroupFloor, FloorsWithoutGroupsOrOkAreNotRun) {
    // Both floored subscriptions pending (no SUBSCRIBE_OK, no Group stream) for the whole window.
    Script silent;
    silent.answer = [](ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId,
                       const l06::Subscribe& subscribe) {
        return subscribe.subscribe_id != scen::kL06LearnSubscribeId;
    };
    const auto t = run(floor_probe(), silent);
    EXPECT_TRUE(lite06::allowance_elapsed(t));
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_floor(t), Pair(kNotRun, kNotRun));
    // Groups but no SUBSCRIBE_OK on either floor: 159 judged, 172 NotRun.
    Script groups_only;
    groups_only.edit = [](const l06::Subscribe& subscribe, std::optional<std::uint64_t>& ok, auto&) {
        if (subscribe.subscribe_id != scen::kL06LearnSubscribeId) ok.reset();
    };
    EXPECT_EQ(judge_floor(run(floor_probe(), groups_only)), Pair(kPass, kNotRun));
    // A SUBSCRIBE_OK but no Group stream on either floor: 159 NotRun, 172 judged.
    Script ok_only;
    ok_only.edit = [](const l06::Subscribe& subscribe, auto&, std::vector<Planned>& groups) {
        if (subscribe.subscribe_id != scen::kL06LearnSubscribeId) groups.clear();
    };
    EXPECT_EQ(judge_floor(run(floor_probe(), ok_only)), Pair(kNotRun, kPass));
}

TEST(Lite06SubscribeGroupFloor, ALearningSubscriptionThatIsRefusedEndsWithoutFloors) {
    const auto t = run(floor_probe(), answering(0, [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                                      auto&, transport::StreamId stream, const auto&) {
                           publisher.refuse(peer, stream, kNotFound);
                       }));
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(lite06::step_labelled(t, scen::kL06FloorAtLatestLabel), nullptr);
    EXPECT_EQ(judge_floor(t), Pair(kNotRun, kNotRun));
}

TEST(Lite06SubscribeGroupFloor, ASilentLearningSubscriptionTimesOutNotRun) {
    const auto t = run(floor_probe(), pending(0));
    EXPECT_TRUE(t.timed_out);
    EXPECT_EQ(judge_floor(t), Pair(kNotRun, kNotRun));
}

TEST(Lite06SubscribeGroupFloor, ASlowFloorAnswerInsideTheWindowPasses) {
    const auto t = run(floor_probe(), answering(scen::kL06FloorAboveSubscribeId, [](auto&, auto&, State& state,
                                                                                    transport::StreamId stream,
                                                                                    const l06::Subscribe&) {
                           state.at(polls(5000ms), [stream](ConformingLitePublisher&, ScriptedLitePeer& peer,
                                                            State& s) {
                               peer.data(stream, subscribe_response(l06::SubscribeOk{7}));
                               s.queue.push_back({scen::kL06FloorAboveSubscribeId, 7, 0, 1});
                           });
                       }));
    EXPECT_EQ(judge_floor(t), Pair(kPass, kPass));
}

// --- l06-subscribe-abutting-frame-start (020) ---------------------------------------------------------------------

TEST(Lite06SubscribeAbutting, ThePublisherHoldingGroupGResumesAtFrameN) {
    const auto t = run(abutting_probe());
    const auto* first = lite06::step_labelled(t, scen::kL06AbuttingFirstLabel);
    const auto* second = lite06::step_labelled(t, scen::kL06AbuttingSecondLabel);
    ASSERT_TRUE(first && second);
    const auto a = scen::l06_decode_subscribe_stimulus(first->bytes);
    const auto b = scen::l06_decode_subscribe_stimulus(second->bytes);
    ASSERT_TRUE(a && b);
    // The draft's pattern carries the same numbers on the wire (draft 3.6).
    EXPECT_EQ(a->range.group_start, 5u);
    EXPECT_EQ(a->range.group_end, 6u);
    EXPECT_EQ(a->range.frame_start, 0u);
    EXPECT_EQ(a->range.frame_end, scen::kL06AbuttingFrameSplit);
    EXPECT_EQ(b->range.group_start, 5u);
    EXPECT_EQ(b->range.frame_start, scen::kL06AbuttingFrameSplit);
    EXPECT_EQ(b->range.group_end, 0u);
    EXPECT_EQ(b->range.frame_end, 0u);
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_TRUE(lite06::allowance_elapsed(t));
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kPass);
}

TEST(Lite06SubscribeAbutting, WithHistoryGIsTheLatestGroup) {
    Script history;
    history.history = 3;
    const auto t = run(abutting_probe(), history);
    const auto* second = lite06::step_labelled(t, scen::kL06AbuttingSecondLabel);
    ASSERT_TRUE(second);
    const auto b = scen::l06_decode_subscribe_stimulus(second->bytes);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->range.group_start, 5u);
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kPass);
}

TEST(Lite06SubscribeAbutting, ALaterGroupAtFrameZeroPasses) {
    const auto t = run(abutting_probe(), editing(scen::kL06AbuttingSecondSubscribeId,
                                                 [](std::optional<std::uint64_t>& ok, std::vector<Planned>& groups) {
                                                     ok = 6;
                                                     groups = {{2, 6, 0, 3}, {2, 7, 0, 3}};
                                                 }));
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kPass);
}

TEST(Lite06SubscribeAbutting, AnyOtherResolvedStartFails) {
    for (const auto& start : {Planned{2, 5, 0, 3}, Planned{2, 6, 1, 2}, Planned{2, 5, 2, 1}, Planned{2, 3, 1, 2}}) {
        const auto t = run(abutting_probe(), editing(scen::kL06AbuttingSecondSubscribeId,
                                                     [start](auto&, std::vector<Planned>& groups) {
                                                         groups = {start};
                                                     }));
        EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kFail) << start.sequence << "/" << start.frame_start;
    }
}

TEST(Lite06SubscribeAbutting, GroupGMinusOneAtFrameNIsTheReadingConflict) {
    const auto t = run(abutting_probe(), editing(scen::kL06AbuttingSecondSubscribeId,
                                                 [](auto&, std::vector<Planned>& groups) {
                                                     groups = {{2, 4, 1, 2}, {2, 5, 0, 3}};
                                                 }));
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kNotRun);
}

TEST(Lite06SubscribeAbutting, ALaterPartialGroupOnTheSecondSubscriptionIsStillSeen) {
    auto script = editing(scen::kL06AbuttingSecondSubscribeId, [](auto&, auto&) {});
    script.on_poll = [armed = std::make_shared<bool>(false)](ConformingLitePublisher&, ScriptedLitePeer&,
                                                              State& state) {
        if (*armed || !state.streams.contains(scen::kL06AbuttingSecondSubscribeId)) return;
        *armed = true;
        state.at(polls(5000ms), [](ConformingLitePublisher&, ScriptedLitePeer& peer, State&) {
            peer.data(peer.open_peer_uni(), group_stream({scen::kL06AbuttingSecondSubscribeId, 9, 2, 1}), true);
        });
    };
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(run(abutting_probe(), script)), kFail);
}

TEST(Lite06SubscribeAbutting, ANonZeroFrameStartOnTheDefaultSubscriptionFails) {
    auto script = editing(scen::kL06LearnSubscribeId, [](auto&, auto&) {});
    script.on_poll = [armed = std::make_shared<bool>(false)](ConformingLitePublisher&, ScriptedLitePeer&,
                                                              State& state) {
        if (*armed || !state.streams.contains(scen::kL06AbuttingSecondSubscribeId)) return;
        *armed = true;
        state.at(polls(4000ms), [](ConformingLitePublisher&, ScriptedLitePeer& peer, State&) {
            peer.data(peer.open_peer_uni(), group_stream({scen::kL06LearnSubscribeId, 8, 1, 1}), true);
        });
    };
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(run(abutting_probe(), script)), kFail);
}

TEST(Lite06SubscribeAbutting, NoFramesOnTheFirstSubscriptionIsNotRun) {
    const auto t = run(abutting_probe(), editing(scen::kL06AbuttingFirstSubscribeId,
                                                 [](auto&, std::vector<Planned>& groups) {
                                                     for (auto& g : groups) g.frames = 0;
                                                 }));
    EXPECT_EQ(lite06::step_labelled(t, scen::kL06AbuttingSecondLabel), nullptr);
    EXPECT_TRUE(lite06::allowance_elapsed(t));  // the window is still observed
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kNotRun);
}

TEST(Lite06SubscribeAbutting, TheFirstSubscriptionAnsweredWithAnotherGroupIsNotRun) {
    const auto t = run(abutting_probe(), editing(scen::kL06AbuttingFirstSubscribeId,
                                                 [](std::optional<std::uint64_t>& ok, std::vector<Planned>& groups) {
                                                     ok = 6;
                                                     groups = {{1, 6, 0, 1}};
                                                 }));
    EXPECT_EQ(lite06::step_labelled(t, scen::kL06AbuttingSecondLabel), nullptr);
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kNotRun);
}

TEST(Lite06SubscribeAbutting, APartialGroupOnTheFirstSubscriptionFails) {
    // The first subscription asked for frame 0 of G; a GROUP at Frame Start 1 is a Position it did not choose.
    const auto t = run(abutting_probe(), editing(scen::kL06AbuttingFirstSubscribeId,
                                                 [](auto&, std::vector<Planned>& groups) {
                                                     groups = {{1, 5, 1, 1}};
                                                 }));
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kFail);
}

TEST(Lite06SubscribeAbutting, NoGroupOnTheSecondSubscriptionIsNotRun) {
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(
                  run(abutting_probe(), pending(scen::kL06AbuttingSecondSubscribeId))),
              kNotRun);
}

TEST(Lite06SubscribeAbutting, ASlowSecondAnswerInsideTheWindowPasses) {
    const auto t = run(abutting_probe(),
                       answering(scen::kL06AbuttingSecondSubscribeId, [](auto&, auto&, State& state,
                                                                         transport::StreamId stream, const auto&) {
                           state.at(polls(5000ms), [stream](ConformingLitePublisher&, ScriptedLitePeer& peer,
                                                            State& s) {
                               peer.data(stream, subscribe_response(l06::SubscribeOk{5}));
                               s.queue.push_back({scen::kL06AbuttingSecondSubscribeId, 5, 1, 2});
                           });
                       }));
    EXPECT_EQ(evaluate_l06_subscribe_resolved_start(t), kPass);
}

// --- attribution --------------------------------------------------------------------------------------------------

TEST(Lite06SubscribeAttribution, EachEvaluatorJudgesOnlyItsOwnScenario) {
    for (const auto& probe : every_probe()) {
        const auto t = run(probe());
        bool judged = false;
        for (const auto evaluator : all_evaluators()) judged = judged || evaluator(t).has_value();
        EXPECT_TRUE(judged) << t.scenario_id;
        for (const auto evaluator : foreign_evaluators(t.scenario_id))
            EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
        // The setup and announce evaluators (and the shared code-space evaluator) judge none of these.
        const std::vector<Evaluator> earlier{
            scen::evaluate_l06_setup_stream_single_setup,      scen::evaluate_l06_setup_parameters_unique,
            scen::evaluate_l06_setup_unknown_parameter_ignored, scen::evaluate_l06_setup_duplicate_parameter_close,
            scen::evaluate_l06_setup_duplicate_stream_close,   scen::evaluate_l06_setup_server_path_close,
            scen::evaluate_l06_setup_server_role_close,        scen::evaluate_l06_errors_code_space,
            scen::evaluate_l06_announce_ok_then_starts,        scen::evaluate_l06_announce_ok_hop_assigned,
            scen::evaluate_l06_announce_hop_list_excludes_own, scen::evaluate_l06_announce_retired_id_unused,
            scen::evaluate_l06_session_peer_closes_send};
        for (const auto evaluator : earlier) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
    // And these evaluators judge none of the earlier probes.
    for (const auto& definition :
         {scen::l06_setup_stream_probe(kDeadline), scen::l06_setup_duplicate_parameter_probe(kDeadline),
          scen::l06_announce_prefix_probe(kDeadline, kBroadcast),
          scen::l06_session_stream_close_probe(kDeadline, kBroadcast, kTrack)}) {
        const auto t = run(definition);
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeAttribution, OneGroupDefectFailsExactlyItsEvaluator) {
    l06::Frame value;
    value.payload = bytes_of("hello");
    struct Case {
        std::string name;
        Script script;
        Triple expected;
    };
    const std::vector<Case> cases{
        {"conforming", {}, {kPass, kPass, kPass}},
        {"not GROUP first", with_extra_stream(2, join({stream_type(0x0), frame(value)})), {kFail, kPass, kPass}},
        {"duplicate", editing(0, [](auto&, std::vector<Planned>& g) { g.insert(g.begin() + 1, g[0]); }),
         {kPass, kFail, kPass}},
        {"gap", editing(0, [](auto&, std::vector<Planned>& g) { g[2].sequence = 9; }), {kPass, kPass, kFail}},
        {"one group", [] {
             Script s;
             s.groups = 1;
             return s;
         }(),
         {kPass, kNotRun, kNotRun}},
    };
    for (const auto& c : cases) {
        const auto t = run(latest_probe(), c.script);
        EXPECT_EQ(judge_latest(t), c.expected) << c.name;
        for (const auto evaluator : foreign_evaluators(t.scenario_id)) EXPECT_EQ(evaluator(t), kNotRun) << c.name;
    }
}

TEST(Lite06SubscribeAttribution, OneFloorDefectFailsExactlyItsEvaluator) {
    struct Case {
        std::string name;
        Script script;
        Pair expected;
    };
    const std::vector<Case> cases{
        {"conforming", {}, {kPass, kPass}},
        {"group below", editing(2, [](auto&, std::vector<Planned>& g) { g.push_back({2, 1, 0, 1}); }),
         {kFail, kPass}},
        {"ok below", editing(2, [](std::optional<std::uint64_t>& ok, auto&) { ok = 1; }), {kPass, kFail}},
    };
    for (const auto& c : cases) {
        const auto t = run(floor_probe(), c.script);
        EXPECT_EQ(judge_floor(t), c.expected) << c.name;
        for (const auto evaluator : foreign_evaluators(t.scenario_id)) EXPECT_EQ(evaluator(t), kNotRun) << c.name;
    }
}

// --- NotRun conditions shared by all five probes ------------------------------------------------------------------

TEST(Lite06SubscribeNotRun, EmptyTranscriptIsNeverAPass) {
    const LiteTranscript empty;
    for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(empty), kNotRun);
    for (const auto& probe : every_probe()) {
        LiteTranscript t;
        t.scenario_id = probe().id;
        t.broadcast_path = kBroadcast;
        t.track_name = kTrack;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeNotRun, PublisherThatNeverConnects) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.connect_deadline = 100ms;
        const auto t = run(std::move(definition), make_config({}),
                           [](ScriptedLitePeer& peer) { peer.establish_on_poll.reset(); });
        EXPECT_FALSE(t.established);
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeNotRun, WrongAlpnIsAHarnessFailure) {
    for (const auto& probe : every_probe()) {
        ConformingLitePublisher publisher(make_config({}));
        ScriptedLitePeer peer(publisher.reaction(), "moqt-22");
        ManualLiteClock clock;
        const auto t = run_lite_probe(peer, probe(), clock, kTick);
        ASSERT_TRUE(t.harness_failed) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeNotRun, FlagsOnAnOtherwiseJudgedTranscript) {
    // Each base transcript carries a Fail somewhere, so NotRun below is not vacuous.
    const std::vector<std::pair<std::function<LiteProbeDefinition()>, Script>> bases{
        {latest_probe, editing(0, [](auto&, std::vector<Planned>& g) { g[2].sequence = 9; })},
        {refused_probe, pending(scen::kL06UncoveredSubscribeId)},
        {floor_probe, editing(2, [](std::optional<std::uint64_t>& ok, auto&) { ok = 1; })},
        {abutting_probe, editing(2, [](auto&, std::vector<Planned>& g) { g = {{2, 5, 0, 3}}; })},
    };
    std::vector<LiteTranscript> transcripts;
    for (const auto& [probe, script] : bases) transcripts.push_back(run(probe(), script));
    transcripts.push_back(run(invalid_probe(), invalid_answer([](auto&, auto&, auto&, auto) {})));
    for (const auto& base : transcripts) {
        bool failed = false;
        for (const auto evaluator : all_evaluators()) failed = failed || evaluator(base) == kFail;
        ASSERT_TRUE(failed) << base.scenario_id;
        for (const auto flag : {&LiteTranscript::harness_failed, &LiteTranscript::timed_out,
                                &LiteTranscript::event_limit_reached}) {
            auto t = base;
            t.*flag = true;
            for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
        }
        auto incomplete = base;
        incomplete.complete = false;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(incomplete), kNotRun);
    }
}

TEST(Lite06SubscribeNotRun, EventLimit) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.limits.max_bytes = 4;
        const auto t = run(std::move(definition), make_config({}));
        ASSERT_TRUE(t.event_limit_reached) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeNotRun, AHarnessClassIssueFromThePeer) {
    // Bytes after a FIN on a publisher Group stream (trailing_after_fin, Harness class): never a Fail, never a Pass.
    for (const auto& probe : every_probe()) {
        auto config = make_config(pending(0));  // would be a Fail of 062 (a) and 023-style no-reaction otherwise
        config.hooks.on_start = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
            publisher.send_setup(peer);
            const auto id = peer.open_peer_uni();
            peer.data(id, join({stream_type(0x0), group_header({0, 5, 0})}), true);
            peer.data(id, bytes_of("late"));
            return true;
        };
        const auto t = run(probe(), config);
        ASSERT_FALSE(judgeable(t)) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeNotRun, StimulusNeverDelivered) {
    for (const auto& probe : every_probe()) {
        auto config = make_config({});
        config.hooks.on_start = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
            publisher.close(peer, 0x0);
            return true;
        };
        const auto t = run(probe(), config);
        ASSERT_TRUE(t.peer_close.has_value()) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeNotRun, ASubscribeWriteRefusedByThePeer) {
    // The publisher stops the runner's first bidirectional stream before the write: not delivered.
    for (const auto& probe : {invalid_probe, floor_probe, abutting_probe}) {
        const auto t = run(probe(), make_config({}),
                           [](ScriptedLitePeer& peer) {
                               peer.forced_status[1] = transport::TransportStatus::PeerStopped;
                           });
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06SubscribeNotRun, AFixtureMismatchIsNotRun) {
    const std::vector<std::pair<std::function<LiteProbeDefinition()>, Script>> bases{
        {latest_probe, {}}, {refused_probe, pending(0)}, {invalid_probe, {}}, {floor_probe, {}}, {abutting_probe, {}}};
    for (const auto& [probe, script] : bases) {
        for (const auto& [path, track] :
             {std::pair<std::string, std::string>{"elsewhere/live", kTrack}, {kBroadcast, "audio"}}) {
            auto t = run(probe(), script);
            t.broadcast_path = path;
            t.track_name = track;
            for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
        }
    }
}

}  // namespace
