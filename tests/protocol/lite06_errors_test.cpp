// The moq-lite-06 error-handling scenarios and the client-path scenario (L1d Task 7): every evaluator against a
// conforming scripted publisher and against publishers that violate exactly one rule, plus the NotRun conditions of
// the catalog rows (L06-7-2-MUST-108, L06-7-2-MUST-NOT-109, L06-4-4-MUST-030, L06-4-4-MUST-NOT-032,
// L06-4-4-MUST-NOT-033, L06-4-4-MUST-027 on its own scenario, L06-7-1-SHOULD-107, L06-7-3-2-MUST-120,
// L06-7-3-2-SHOULD-124, L06-7-3-2-MUST-NOT-125).
//
// The shared ConformingLitePublisher answers the setup exchange, announces, unknown streams (reset + STOP_SENDING)
// and malformed requests (close with PROTOCOL_VIOLATION). What these scenarios need beyond it (subscriptions whose
// Group streams stay open, the end of a cancelled subscription, the reaction to a stopped Group stream) and every
// defect is a hook in this file; tests/support/scripted_lite_peer.h is unchanged.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include "moq/interop/scenarios/lite06_errors.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_errors_code_space;
using moq::interop::scenarios::evaluate_l06_errors_message_length_close;
using moq::interop::scenarios::evaluate_l06_errors_no_assumed_unauthorized;
using moq::interop::scenarios::evaluate_l06_errors_reserved_code_tolerated;
using moq::interop::scenarios::evaluate_l06_errors_unknown_code_tolerated;
using moq::interop::scenarios::evaluate_l06_errors_unknown_stream_type_not_fatal;
using moq::interop::scenarios::evaluate_l06_errors_unknown_stream_type_reset;
using moq::interop::scenarios::evaluate_l06_setup_path_absent_on_uri_binding;
using moq::interop::scenarios::evaluate_l06_setup_path_query_appended;
using moq::interop::scenarios::evaluate_l06_setup_path_sent;
using moq::interop::scenarios::judgeable;
using moq::interop::scenarios::LiteBinding;
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
using Pair = std::pair<Verdict, Verdict>;
using Triple = std::tuple<Verdict, Verdict, Verdict>;

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 20000ms;
const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";
const std::string kUrlPath = "/moq";
const std::string kUrlQuery = "token=l1d";
const std::string kExpectedPath = "/moq?token=l1d";
constexpr std::uint64_t kNotFound = 0x33;
constexpr std::uint64_t kCancelled = 0x1;  // stream code CANCELLED
constexpr std::uint64_t kLatest = 5;

const Verdict kPass{true};
const Verdict kFail{false};
const Verdict kNotRun{};

constexpr std::size_t polls(std::chrono::milliseconds at) { return static_cast<std::size_t>(at / kTick); }

// --- the test publisher -------------------------------------------------------------------------------------------

struct State;
using Action = std::function<void(ConformingLitePublisher&, ScriptedLitePeer&, State&)>;
using StreamAction = std::function<void(ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId)>;

struct State {
    std::size_t poll{0};
    std::multimap<std::size_t, Action> later;
    std::map<std::uint64_t, transport::StreamId> subs;    // Subscribe ID -> runner Subscribe stream
    std::map<std::uint64_t, transport::StreamId> groups;  // Subscribe ID -> its open Group stream
    std::set<transport::StreamId> handled;                 // runner cancels / stops already reacted to
    std::set<std::uint64_t> unknown_types;                 // STREAM_TYPEs of unknown runner streams received
    bool malformed_announce{false};                        // the ANNOUNCE_REQUEST with extra bytes was received
    std::optional<std::uint64_t> cancel_code;              // the code of the runner's cancel of A
    std::optional<std::uint64_t> group_stop_code;          // the code of the runner's STOP_SENDING on a Group stream

    void at(std::size_t delay, Action action) { later.emplace(poll + delay, std::move(action)); }
};

struct Script {
    // l06-errors-unknown-stream-type: the reaction to the unregistered stream (default: reset + STOP_SENDING 0x0).
    StreamAction on_unknown;
    // A well-formed ANNOUNCE_REQUEST; true when handled (default: the shared publisher's ANNOUNCE_OK).
    std::function<bool(ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId)> on_announce;
    // The ANNOUNCE_REQUEST whose Message Length covers extra bytes (default: close with PROTOCOL_VIOLATION).
    StreamAction on_length;
    // A SUBSCRIBE for a broadcast the publisher does not serve (default: reset + STOP_SENDING NOT_FOUND).
    StreamAction on_unserved;
    // A fixture SUBSCRIBE; true when handled (default: SUBSCRIBE_OK, then one Group stream left open).
    std::function<bool(ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId,
                       const l06::Subscribe&)>
        on_subscribe;
    // The runner cancelled subscription A (default: reset A's send direction and its Group stream, CANCELLED).
    std::function<void(ConformingLitePublisher&, ScriptedLitePeer&, State&, std::uint64_t)> on_cancel;
    // The runner stopped a Group stream (default: reset it, CANCELLED).
    std::function<void(ConformingLitePublisher&, ScriptedLitePeer&, State&, transport::StreamId, std::uint64_t)>
        on_group_stop;
    // Whether fixture subscriptions get a Group stream.
    bool groups{true};
};

void default_subscribe(ScriptedLitePeer& peer, State& state, transport::StreamId stream, const l06::Subscribe& sub,
                       bool groups) {
    peer.data(stream, subscribe_response(l06::SubscribeOk{kLatest}));
    if (!groups) return;
    const auto id = peer.open_peer_uni();
    l06::Frame value;
    value.timestamp_delta = 1000;
    value.payload = bytes_of("g" + std::to_string(kLatest));
    // GROUP and one FRAME, no FIN: the group is still being produced.
    peer.data(id, join({stream_type(0x0), group_header({sub.subscribe_id, kLatest, 0}), frame(value)}));
    state.groups[sub.subscribe_id] = id;
}

// Ends the cancelled subscription: its Subscribe stream and its Group stream, both reset with CANCELLED.
void end_cancelled(ConformingLitePublisher&, ScriptedLitePeer& peer, State& state) {
    peer.peer_reset(state.subs.at(scen::kL06CancelledSubscribeId), kCancelled);
    if (const auto group = state.groups.find(scen::kL06CancelledSubscribeId); group != state.groups.end()) {
        if (!state.handled.contains(group->second)) peer.peer_reset(group->second, kCancelled);
        state.handled.insert(group->second);
    }
}

ConformingLitePublisherConfig make_config(Script script, std::shared_ptr<State> state = std::make_shared<State>()) {
    ConformingLitePublisherConfig config;
    config.broadcast = kBroadcast;
    config.track = kTrack;
    auto shared = std::make_shared<Script>(std::move(script));
    config.hooks.on_request = [shared, state](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                              const LiteRunnerRequest& request) {
        if (!request.bidirectional) return false;
        if (request.stream_type == scen::kL06UnregisteredStreamType) {
            state->unknown_types.insert(request.stream_type);
            if (shared->on_unknown) {
                shared->on_unknown(publisher, peer, *state, request.stream);
            } else {
                publisher.refuse(peer, request.stream, 0x0);
            }
            return true;
        }
        if (request.stream_type == 0x1) {
            if (std::holds_alternative<l06::AnnounceRequest>(request.message))
                return shared->on_announce && shared->on_announce(publisher, peer, *state, request.stream);
            state->malformed_announce = true;
            if (!shared->on_length) return false;  // the shared publisher closes with PROTOCOL_VIOLATION
            shared->on_length(publisher, peer, *state, request.stream);
            return true;
        }
        if (request.stream_type == 0x2) {
            const auto* subscribe = std::get_if<l06::Subscribe>(&request.message);
            if (!subscribe) return false;
            if (subscribe->broadcast_path != kBroadcast || subscribe->track_name != kTrack) {
                if (shared->on_unserved) {
                    shared->on_unserved(publisher, peer, *state, request.stream);
                } else {
                    publisher.refuse(peer, request.stream, kNotFound);
                }
                return true;
            }
            state->subs[subscribe->subscribe_id] = request.stream;
            if (shared->on_subscribe && shared->on_subscribe(publisher, peer, *state, request.stream, *subscribe))
                return true;
            default_subscribe(peer, *state, request.stream, *subscribe, shared->groups);
            return true;
        }
        return false;
    };
    config.hooks.on_poll = [shared, state](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        ++state->poll;
        while (!state->later.empty() && state->later.begin()->first <= state->poll) {
            auto action = std::move(state->later.begin()->second);
            state->later.erase(state->later.begin());
            if (peer.peer_closed()) return;
            action(publisher, peer, *state);
        }
        if (peer.peer_closed()) return;
        // A runner STOP_SENDING on a Group stream.
        for (const auto& [id, group] : state->groups) {
            const auto* runner = peer.runner_stream(group);
            if (!runner || !runner->stop_sending_code || state->handled.contains(group)) continue;
            state->handled.insert(group);
            state->group_stop_code = runner->stop_sending_code;
            if (shared->on_group_stop) {
                shared->on_group_stop(publisher, peer, *state, group, *runner->stop_sending_code);
            } else {
                peer.peer_reset(group, kCancelled);
            }
            if (peer.peer_closed()) return;
        }
        // The runner's cancel of subscription A (RESET_STREAM or STOP_SENDING).
        const auto a = state->subs.find(scen::kL06CancelledSubscribeId);
        if (a == state->subs.end() || state->handled.contains(a->second)) return;
        const auto* runner = peer.runner_stream(a->second);
        if (!runner || (!runner->reset_code && !runner->stop_sending_code)) return;
        state->handled.insert(a->second);
        state->cancel_code = runner->reset_code ? runner->reset_code : runner->stop_sending_code;
        if (shared->on_cancel) {
            shared->on_cancel(publisher, peer, *state, *state->cancel_code);
        } else {
            end_cancelled(publisher, peer, *state);
        }
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

LiteProbeDefinition unknown_stream_probe() { return scen::l06_errors_unknown_stream_type_probe(kDeadline); }
LiteProbeDefinition unknown_code_probe() {
    return scen::l06_errors_unknown_reset_code_probe(kDeadline, kBroadcast, kTrack);
}
LiteProbeDefinition reserved_probe() {
    return scen::l06_errors_reserved_reset_code_probe(kDeadline, kBroadcast, kTrack);
}
LiteProbeDefinition code_space_probe() { return scen::l06_errors_code_space_probe(kDeadline); }
LiteProbeDefinition client_path_probe(LiteBinding binding = LiteBinding::NativeQuic) {
    auto definition = scen::l06_setup_client_path_probe(kDeadline, kUrlPath, kUrlQuery);
    definition.binding = binding;
    return definition;
}

std::vector<std::function<LiteProbeDefinition()>> every_probe() {
    return {unknown_stream_probe, unknown_code_probe, reserved_probe, code_space_probe,
            [] { return client_path_probe(); }};
}

// The ten evaluators this scenario family is judged by (027 on its own scenario included).
std::vector<Evaluator> all_evaluators() {
    return {evaluate_l06_errors_unknown_stream_type_reset, evaluate_l06_errors_unknown_stream_type_not_fatal,
            evaluate_l06_errors_unknown_code_tolerated,    evaluate_l06_errors_no_assumed_unauthorized,
            evaluate_l06_errors_reserved_code_tolerated,   evaluate_l06_errors_code_space,
            evaluate_l06_errors_message_length_close,      evaluate_l06_setup_path_sent,
            evaluate_l06_setup_path_query_appended,        evaluate_l06_setup_path_absent_on_uri_binding};
}

// The nine evaluators Task 7 adds (each judges only its own scenario).
std::vector<Evaluator> new_evaluators() {
    auto out = all_evaluators();
    out.erase(out.begin() + 5);  // evaluate_l06_errors_code_space is shared (bound to five scenarios)
    return out;
}

// The evaluators of Tasks 4-6 (027 excluded).
std::vector<Evaluator> earlier_evaluators() {
    namespace s = moq::interop::scenarios;
    return {s::evaluate_l06_setup_stream_single_setup,        s::evaluate_l06_setup_parameters_unique,
            s::evaluate_l06_setup_unknown_parameter_ignored,  s::evaluate_l06_setup_duplicate_parameter_close,
            s::evaluate_l06_setup_duplicate_stream_close,     s::evaluate_l06_setup_server_path_close,
            s::evaluate_l06_setup_server_role_close,          s::evaluate_l06_announce_ok_then_starts,
            s::evaluate_l06_announce_ok_hop_assigned,         s::evaluate_l06_announce_hop_list_excludes_own,
            s::evaluate_l06_announce_retired_id_unused,       s::evaluate_l06_session_peer_closes_send,
            s::evaluate_l06_group_starts_with_group,          s::evaluate_l06_group_unique_sequence,
            s::evaluate_l06_group_sequence_increments,        s::evaluate_l06_subscribe_refused_reset,
            s::evaluate_l06_subscribe_invalid_frame_bounds_reset, s::evaluate_l06_subscribe_no_group_below_floor,
            s::evaluate_l06_subscribe_ok_group_at_floor,      s::evaluate_l06_subscribe_resolved_start};
}

std::vector<Verdict> verdicts(const LiteTranscript& t) {
    std::vector<Verdict> out;
    for (const auto evaluator : all_evaluators()) out.push_back(evaluator(t));
    return out;
}

Pair judge_unknown(const LiteTranscript& t) {
    return {evaluate_l06_errors_unknown_stream_type_reset(t), evaluate_l06_errors_unknown_stream_type_not_fatal(t)};
}
Pair judge_code(const LiteTranscript& t) {
    return {evaluate_l06_errors_unknown_code_tolerated(t), evaluate_l06_errors_no_assumed_unauthorized(t)};
}
Pair judge_code_space(const LiteTranscript& t) {
    return {evaluate_l06_errors_code_space(t), evaluate_l06_errors_message_length_close(t)};
}
Triple judge_path(const LiteTranscript& t) {
    return {evaluate_l06_setup_path_sent(t), evaluate_l06_setup_path_query_appended(t),
            evaluate_l06_setup_path_absent_on_uri_binding(t)};
}

// The ms elapsed between establishment and the end of the probe.
std::chrono::milliseconds lasted(const LiteTranscript& t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(t.ended_ns - t.established_ns));
}

const scen::LiteStepRecord& step(const LiteTranscript& t, std::string_view label) {
    const auto* record = lite06::step_labelled(t, label);
    if (!record) throw std::logic_error("no step " + std::string(label));
    return *record;
}

// --- scripted defects ---------------------------------------------------------------------------------------------

StreamAction unknown_reaction(std::function<void(ScriptedLitePeer&, transport::StreamId)> reaction) {
    return [reaction](ConformingLitePublisher&, ScriptedLitePeer& peer, State&, transport::StreamId id) {
        reaction(peer, id);
    };
}

// Closes the session `delay` after the runner's cancel (any code, any space).
Script close_on_cancel(std::uint64_t code, std::size_t delay = 0,
                       transport::CloseErrorSpace space = transport::CloseErrorSpace::Application) {
    Script script;
    script.on_cancel = [code, delay, space](ConformingLitePublisher& publisher, ScriptedLitePeer& peer, State& state,
                                            std::uint64_t) {
        if (delay == 0) {
            peer.close_session(code, {}, space);
            return;
        }
        // The subscription is ended properly first; the close comes later in the allowance.
        end_cancelled(publisher, peer, state);
        state.at(delay, [code, space](auto&, ScriptedLitePeer& p, State&) { p.close_session(code, {}, space); });
    };
    return script;
}

// Refuses subscription C (reset), `delay` polls after answering it with SUBSCRIBE_OK when `delay` is set.
Script refuse_later(std::optional<std::size_t> after_ok = std::nullopt) {
    Script script;
    script.on_subscribe = [after_ok](ConformingLitePublisher& publisher, ScriptedLitePeer& peer, State& state,
                                     transport::StreamId stream, const l06::Subscribe& subscribe) {
        if (subscribe.subscribe_id != scen::kL06LaterSubscribeId) return false;
        if (!after_ok) {
            publisher.refuse(peer, stream, kCancelled);
            return true;
        }
        default_subscribe(peer, state, stream, subscribe, true);
        state.at(*after_ok, [stream](ConformingLitePublisher& p, ScriptedLitePeer& pr, State&) {
            p.refuse(pr, stream, kCancelled);
        });
        return true;
    };
    return script;
}

Script reset_kept_on_cancel() {
    Script script;
    script.on_cancel = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer, State& state, std::uint64_t) {
        end_cancelled(publisher, peer, state);
        peer.peer_reset(state.subs.at(scen::kL06KeptSubscribeId), kCancelled);
    };
    return script;
}

Script ignore_cancel() {
    Script script;
    script.on_cancel = [](auto&, auto&, auto&, std::uint64_t) {};
    return script;
}

}  // namespace

// === common =========================================================================================================

TEST(Lite06ErrorsCommon, BuildersRejectShortDeadlinesAndMissingFixture) {
    EXPECT_THROW(scen::l06_errors_unknown_stream_type_probe(3000ms), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_errors_unknown_stream_type_probe(3001ms));
    EXPECT_THROW(scen::l06_errors_unknown_stream_type_probe(5000ms, 0ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_errors_unknown_reset_code_probe(9000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_errors_unknown_reset_code_probe(9001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_errors_reserved_reset_code_probe(6000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_errors_reserved_reset_code_probe(6001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_errors_unknown_reset_code_probe(kDeadline, "", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_errors_unknown_reset_code_probe(kDeadline, kBroadcast, ""), std::invalid_argument);
    EXPECT_THROW(scen::l06_errors_reserved_reset_code_probe(kDeadline, "/", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_errors_reserved_reset_code_probe(kDeadline, kBroadcast, kTrack, 3000ms, 0ms),
                 std::invalid_argument);
    EXPECT_THROW(scen::l06_errors_code_space_probe(6000ms), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_errors_code_space_probe(6001ms));
    EXPECT_THROW(scen::l06_errors_code_space_probe(kDeadline, 3000ms, 0ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_setup_client_path_probe(2000ms, kUrlPath, kUrlQuery), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_setup_client_path_probe(2001ms, kUrlPath, kUrlQuery));
}

TEST(Lite06ErrorsCommon, ProbeStimuli) {
    // Row 108: only an unregistered STREAM_TYPE.
    EXPECT_EQ(scen::l06_unknown_stream_type_bytes(), Bytes{std::byte{0x3f}});
    EXPECT_GT(scen::kL06UnregisteredStreamType, 0x6u);
    EXPECT_GE(scen::kL06UnknownErrorCode, 64u);
    EXPECT_GE(scen::kL06ReservedErrorCode, 32u);
    EXPECT_LE(scen::kL06ReservedErrorCode, 47u);
    // Row 107: STREAM_TYPE 0x1, Message Length 4 covering the empty prefix and "l1d".
    const auto length = scen::l06_message_length_extra_bytes();
    EXPECT_EQ(length, join({Bytes{std::byte{0x01}, std::byte{0x04}, std::byte{0x00}}, bytes_of("l1d")}));
    {
        wire::Cursor cursor(std::span<const std::byte>(length).subspan(1));
        const auto decoded = l06::decode_announce_request(cursor);
        EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(decoded));  // a length mismatch for a receiver
    }
    const auto unserved = scen::l06_decode_subscribe_stimulus(scen::l06_unserved_subscribe_bytes());
    ASSERT_TRUE(unserved.has_value());
    EXPECT_EQ(unserved->broadcast_path, scen::kL06UnservedBroadcast);
    EXPECT_EQ(scen::l06_expected_client_path("/moq", "token=l1d"), "/moq?token=l1d");
    EXPECT_EQ(scen::l06_expected_client_path("/moq", ""), "/moq");

    const auto labels = [](const LiteProbeDefinition& d) {
        std::vector<std::string> out;
        for (const auto& s : d.steps) out.push_back(s.label);
        return out;
    };
    const auto unknown = unknown_stream_probe();
    EXPECT_EQ(labels(unknown), (std::vector<std::string>{"unknown-stream", "announce-request", "allowance"}));
    EXPECT_FALSE(unknown.requires_track);
    EXPECT_FALSE(unknown.steps[0].fin);
    EXPECT_EQ(unknown.steps[1].bytes, scen::l06_announce_request_bytes(""));
    const auto code = unknown_code_probe();
    EXPECT_EQ(labels(code),
              (std::vector<std::string>{"subscribe-cancelled", "subscribe-kept", "live", "stop-group", "cancel-reset",
                                        "cancel-stop", "subscribe-later", "allowance"}));
    EXPECT_TRUE(code.requires_track);
    EXPECT_EQ(code.broadcast_path, kBroadcast);
    EXPECT_EQ(code.track_name, kTrack);
    EXPECT_EQ(code.steps[3].code, scen::kL06UnknownErrorCode);
    EXPECT_EQ(code.steps[4].code, scen::kL06UnknownErrorCode);
    EXPECT_EQ(code.steps[5].code, scen::kL06UnknownErrorCode);
    EXPECT_TRUE(code.steps[2].gate && code.steps[3].gate && code.steps[3].target);
    EXPECT_EQ(code.steps[2].gate_deadline, scen::kLiteResponseAllowance);
    EXPECT_EQ(code.steps[3].gate_deadline, scen::kLiteResponseAllowance);
    const auto reserved = reserved_probe();
    EXPECT_EQ(labels(reserved), (std::vector<std::string>{"subscribe-cancelled", "subscribe-kept", "live",
                                                          "cancel-reset", "cancel-stop", "subscribe-later",
                                                          "allowance"}));
    EXPECT_EQ(reserved.steps[3].code, scen::kL06ReservedErrorCode);
    EXPECT_EQ(reserved.steps[4].code, scen::kL06ReservedErrorCode);
    const auto space = code_space_probe();
    EXPECT_EQ(labels(space),
              (std::vector<std::string>{"subscribe-unserved", "refused", "announce-length", "allowance"}));
    EXPECT_FALSE(space.requires_track);
    EXPECT_EQ(space.steps[2].bytes, length);
    EXPECT_EQ(space.steps[1].gate_deadline, scen::kLiteResponseAllowance);
    const auto path = client_path_probe();
    EXPECT_EQ(labels(path), (std::vector<std::string>{"allowance"}));
    EXPECT_TRUE(path.session_url_has_path);
    EXPECT_EQ(path.session_url_path, kUrlPath);
    EXPECT_EQ(path.session_url_query, kUrlQuery);
    // Every probe ends with the ungated allowance Wait and never ends early on a condition.
    for (const auto& probe : every_probe()) {
        const auto d = probe();
        EXPECT_FALSE(d.done) << d.id;
        EXPECT_EQ(d.observation_window, 0ms) << d.id;
        const auto& last = d.steps.back();
        EXPECT_EQ(last.kind, LiteStep::Kind::Wait) << d.id;
        EXPECT_FALSE(last.gate) << d.id;
    }
    EXPECT_EQ(unknown.steps.back().delay, scen::kLiteResponseAllowance);
    EXPECT_EQ(code.steps.back().delay, scen::kLiteResponseAllowance);
    EXPECT_EQ(space.steps.back().delay, scen::kLiteCloseAllowance);
    EXPECT_EQ(path.steps.back().delay, scen::kLiteSetupAllowance);
}

TEST(Lite06ErrorsCommon, SessionUrlIsCopiedToTheTranscript) {
    ConformingLitePublisher publisher(make_config({}));
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, client_path_probe(LiteBinding::WebTransport), clock, kTick);
    EXPECT_TRUE(t.session_url_has_path);
    EXPECT_EQ(t.session_url_path, kUrlPath);
    EXPECT_EQ(t.session_url_query, kUrlQuery);
    EXPECT_EQ(t.binding, LiteBinding::WebTransport);
}

// === l06-errors-unknown-stream-type (108, 109) ======================================================================

TEST(Lite06ErrorsUnknownStreamType, ResetAndStopPassBoth) {
    auto state = std::make_shared<State>();
    const auto t = run(unknown_stream_probe(), make_config({}, state));
    EXPECT_EQ(state->unknown_types, (std::set<std::uint64_t>{scen::kL06UnregisteredStreamType}));
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_FALSE(t.timed_out);
    EXPECT_GE(lasted(t), scen::kLiteResponseAllowance);
    EXPECT_EQ(judge_unknown(t), Pair(kPass, kPass));
}

TEST(Lite06ErrorsUnknownStreamType, EitherHalfOfTheResetPasses) {
    Script reset_only;
    reset_only.on_unknown = unknown_reaction([](ScriptedLitePeer& p, auto id) { p.peer_reset(id, 0x0); });
    EXPECT_EQ(judge_unknown(run(unknown_stream_probe(), reset_only)), Pair(kPass, kPass));
    Script stop_only;
    stop_only.on_unknown = unknown_reaction([](ScriptedLitePeer& p, auto id) { p.peer_stop_sending(id, 0x0); });
    EXPECT_EQ(judge_unknown(run(unknown_stream_probe(), stop_only)), Pair(kPass, kPass));
    // A FIN first, then the reset within the allowance.
    Script fin_then_reset;
    fin_then_reset.on_unknown = [](auto&, ScriptedLitePeer& p, State& state, transport::StreamId id) {
        p.fin(id);
        state.at(polls(1000ms), [id](auto&, ScriptedLitePeer& pr, State&) { pr.peer_stop_sending(id, 0x0); });
    };
    EXPECT_EQ(judge_unknown(run(unknown_stream_probe(), fin_then_reset)), Pair(kPass, kPass));
}

TEST(Lite06ErrorsUnknownStreamType, NotResettingFailsOnlyTheReset) {
    auto config = make_config({});
    config.hooks.on_request = nullptr;  // the shared publisher with its named defect
    config.defect = LiteDefect::IgnoreUnknownStreams;
    const auto t = run(unknown_stream_probe(), config);
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_EQ(judge_unknown(t), Pair(kFail, kPass));
}

TEST(Lite06ErrorsUnknownStreamType, AFinWithoutAResetFails) {
    Script script;
    script.on_unknown = unknown_reaction([](ScriptedLitePeer& p, auto id) { p.fin(id); });
    EXPECT_EQ(judge_unknown(run(unknown_stream_probe(), script)), Pair(kFail, kPass));
}

TEST(Lite06ErrorsUnknownStreamType, ASessionCloseFailsOnlyNotFatal) {
    for (const std::uint64_t code : {0x3u, 0x0u}) {
        Script script;
        script.on_unknown = unknown_reaction([code](ScriptedLitePeer& p, auto) { p.close_session(code); });
        const auto t = run(unknown_stream_probe(), script);
        ASSERT_TRUE(t.peer_close.has_value());
        EXPECT_EQ(judge_unknown(t), Pair(kNotRun, kFail)) << code;
    }
}

TEST(Lite06ErrorsUnknownStreamType, AResetThenALateCloseFailsOnlyNotFatal) {
    // The late-violation class: the reset comes at once, the close 2.5 s later, inside the window.
    Script script;
    script.on_unknown = [](auto&, ScriptedLitePeer& p, State& state, transport::StreamId id) {
        p.peer_reset(id, 0x0);
        state.at(polls(2500ms), [](auto&, ScriptedLitePeer& pr, State&) { pr.close_session(0x0); });
    };
    const auto t = run(unknown_stream_probe(), script);
    EXPECT_EQ(judge_unknown(t), Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsUnknownStreamType, ASlowResetInsideTheAllowancePasses) {
    Script script;
    script.on_unknown = [](auto&, ScriptedLitePeer&, State& state, transport::StreamId id) {
        state.at(polls(2500ms), [id](auto&, ScriptedLitePeer& pr, State&) { pr.peer_reset(id, 0x0); });
    };
    EXPECT_EQ(judge_unknown(run(unknown_stream_probe(), script)), Pair(kPass, kPass));
}

TEST(Lite06ErrorsUnknownStreamType, RefusingTheFollowUpRequestFailsOnlyNotFatal) {
    Script reset;
    reset.on_announce = [](ConformingLitePublisher& publisher, ScriptedLitePeer& p, State&, transport::StreamId id) {
        publisher.refuse(p, id, 0x0);
        return true;
    };
    EXPECT_EQ(judge_unknown(run(unknown_stream_probe(), reset)), Pair(kPass, kFail));
    // A late refusal after the ANNOUNCE_OK is still a refusal.
    Script late;
    late.on_announce = [](ConformingLitePublisher& publisher, ScriptedLitePeer& p, State& state,
                          transport::StreamId id) {
        publisher.answer_announce(p, id, l06::AnnounceRequest{""});
        state.at(polls(2000ms), [id](ConformingLitePublisher& pub, ScriptedLitePeer& pr, State&) {
            pub.refuse(pr, id, 0x0);
        });
        return true;
    };
    EXPECT_EQ(judge_unknown(run(unknown_stream_probe(), late)), Pair(kPass, kFail));
}

TEST(Lite06ErrorsUnknownStreamType, AFollowUpWriteRefusedByThePeer) {
    // Runner bidi ids: unknown stream 1, announce 5. The publisher stopped the announce before the write.
    const auto t = run(unknown_stream_probe(), make_config({}),
                       [](ScriptedLitePeer& peer) { peer.forced_status[5] = transport::TransportStatus::PeerStopped; });
    EXPECT_EQ(judge_unknown(t), Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsUnknownStreamType, AnUnansweredFollowUpIsNotRunForNotFatal) {
    Script silent;
    silent.on_announce = [](auto&, auto&, auto&, auto) { return true; };
    const auto t = run(unknown_stream_probe(), silent);
    EXPECT_EQ(judge_unknown(t), Pair(kPass, kNotRun));
}

// === l06-errors-unknown-reset-code (030, 032) =======================================================================

TEST(Lite06ErrorsUnknownResetCode, ConformingPublisherPassesBoth) {
    auto state = std::make_shared<State>();
    const auto t = run(unknown_code_probe(), make_config({}, state));
    EXPECT_EQ(state->cancel_code, scen::kL06UnknownErrorCode);
    EXPECT_EQ(state->group_stop_code, scen::kL06UnknownErrorCode);
    EXPECT_TRUE(step(t, "live").executed());
    EXPECT_TRUE(step(t, "stop-group").delivered());
    ASSERT_TRUE(step(t, "stop-group").stream_id.has_value());
    EXPECT_EQ(*step(t, "stop-group").stream_id, state->groups.at(0));  // a Group stream of A
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_FALSE(t.timed_out);
    EXPECT_GE(lasted(t), scen::kLiteResponseAllowance);
    EXPECT_EQ(judge_code(t), Pair(kPass, kPass));
}

TEST(Lite06ErrorsUnknownResetCode, AFinOfTheCancelledSubscriptionAlsoPasses) {
    Script script;
    script.on_cancel = [](auto&, ScriptedLitePeer& p, State& state, std::uint64_t) { p.fin(state.subs.at(0)); };
    EXPECT_EQ(judge_code(run(unknown_code_probe(), script)), Pair(kPass, kPass));
}

TEST(Lite06ErrorsUnknownResetCode, AnUnauthorizedCloseFailsOnly032) {
    const auto t = run(unknown_code_probe(), close_on_cancel(scen::kL06Unauthorized));
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(judge_code(t), Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsUnknownResetCode, AnUnauthorizedCloseWithStreamResetsStillFailsOnly032) {
    // Resets of B and C (SESSION_CLOSED) before the UNAUTHORIZED close: one defect, 032 only.
    Script script;
    script.on_cancel = [](auto&, ScriptedLitePeer& p, State& state, std::uint64_t) {
        p.peer_reset(state.subs.at(1), 0x3);
        state.at(1, [](auto&, ScriptedLitePeer& pr, State& s) {
            if (s.subs.contains(2)) pr.peer_reset(s.subs.at(2), 0x3);
            pr.close_session(scen::kL06Unauthorized);
        });
    };
    EXPECT_EQ(judge_code(run(unknown_code_probe(), script)), Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsUnknownResetCode, AnyOtherCloseFailsOnly030) {
    for (const std::uint64_t code : {0x0u, 0x1u, 0x3u}) {
        EXPECT_EQ(judge_code(run(unknown_code_probe(), close_on_cancel(code))), Pair(kFail, kNotRun)) << code;
    }
    // A close in the transport space with the value of UNAUTHORIZED is not the session code UNAUTHORIZED.
    EXPECT_EQ(judge_code(run(unknown_code_probe(),
                             close_on_cancel(scen::kL06Unauthorized, 0, transport::CloseErrorSpace::Transport))),
              Pair(kFail, kNotRun));
}

TEST(Lite06ErrorsUnknownResetCode, ALateCloseIsStillJudged) {
    EXPECT_EQ(judge_code(run(unknown_code_probe(), close_on_cancel(0x0, polls(2500ms)))), Pair(kFail, kNotRun));
    EXPECT_EQ(judge_code(run(unknown_code_probe(), close_on_cancel(scen::kL06Unauthorized, polls(2500ms)))),
              Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsUnknownResetCode, AFatalReactionToTheStoppedGroupStreamFails) {
    // The row 098 note's check: the STOP_SENDING on a Group stream must not end the session.
    Script script;
    script.on_group_stop = [](auto&, ScriptedLitePeer& p, State&, transport::StreamId, std::uint64_t) {
        p.close_session(0x1);
    };
    EXPECT_EQ(judge_code(run(unknown_code_probe(), script)), Pair(kFail, kNotRun));
    Script unauthorized;
    unauthorized.on_group_stop = [](auto&, ScriptedLitePeer& p, State&, transport::StreamId, std::uint64_t) {
        p.close_session(scen::kL06Unauthorized);
    };
    EXPECT_EQ(judge_code(run(unknown_code_probe(), unauthorized)), Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsUnknownResetCode, RefusingTheLaterRequestFailsOnly030) {
    EXPECT_EQ(judge_code(run(unknown_code_probe(), refuse_later())), Pair(kFail, kPass));
    // OK first, the reset 2 s later (late violation).
    EXPECT_EQ(judge_code(run(unknown_code_probe(), refuse_later(polls(2000ms)))), Pair(kFail, kPass));
    // The later write refused by the peer (runner bidi ids: A 1, B 5, C 9).
    const auto t = run(unknown_code_probe(), make_config({}),
                       [](ScriptedLitePeer& peer) { peer.forced_status[9] = transport::TransportStatus::PeerReset; });
    EXPECT_EQ(judge_code(t), Pair(kFail, kPass));
}

TEST(Lite06ErrorsUnknownResetCode, EndingTheOtherSubscriptionFailsOnly030) {
    EXPECT_EQ(judge_code(run(unknown_code_probe(), reset_kept_on_cancel())), Pair(kFail, kPass));
}

TEST(Lite06ErrorsUnknownResetCode, AFinOfTheOtherSubscriptionIsNotRun) {
    // SUBSCRIBE_END + FIN of B could be the track ending: not a refusal, not a Pass either.
    Script script;
    script.on_cancel = [](ConformingLitePublisher& pub, ScriptedLitePeer& p, State& state, std::uint64_t) {
        end_cancelled(pub, p, state);
        p.data(state.subs.at(1), subscribe_response(l06::SubscribeEnd{kLatest + 1}), true);
    };
    EXPECT_EQ(judge_code(run(unknown_code_probe(), script)), Pair(kNotRun, kPass));
}

TEST(Lite06ErrorsUnknownResetCode, IgnoringTheCancelCannotPass) {
    const auto t = run(unknown_code_probe(), ignore_cancel());
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_code(t), Pair(kNotRun, kNotRun));
}

TEST(Lite06ErrorsUnknownResetCode, ASlowEndOfTheCancelledSubscriptionPasses) {
    Script script;
    script.on_cancel = [](auto&, ScriptedLitePeer&, State& state, std::uint64_t) {
        state.at(polls(2500ms), [](ConformingLitePublisher& pub, ScriptedLitePeer& p, State& s) {
            end_cancelled(pub, p, s);
        });
    };
    EXPECT_EQ(judge_code(run(unknown_code_probe(), script)), Pair(kPass, kPass));
}

TEST(Lite06ErrorsUnknownResetCode, AnUnansweredLaterRequestIsNotRunFor030) {
    Script script;
    script.on_subscribe = [](auto&, auto&, auto&, auto, const l06::Subscribe& s) {
        return s.subscribe_id == scen::kL06LaterSubscribeId;
    };
    EXPECT_EQ(judge_code(run(unknown_code_probe(), script)), Pair(kNotRun, kPass));
}

TEST(Lite06ErrorsUnknownResetCode, WithoutTwoLiveSubscriptionsNothingIsJudged) {
    Script script;
    script.on_subscribe = [](ConformingLitePublisher& pub, ScriptedLitePeer& p, State&, transport::StreamId id,
                             const l06::Subscribe& s) {
        if (s.subscribe_id != scen::kL06CancelledSubscribeId) return false;
        pub.refuse(p, id, kNotFound);
        return true;
    };
    const auto t = run(unknown_code_probe(), script);
    EXPECT_TRUE(step(t, "live").gate_expired);
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_code(t), Pair(kNotRun, kNotRun));
}

TEST(Lite06ErrorsUnknownResetCode, WithoutAGroupStreamTheCancelIsStillJudged) {
    Script script;
    script.groups = false;
    const auto t = run(unknown_code_probe(), script);
    EXPECT_TRUE(step(t, "stop-group").gate_expired);
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_code(t), Pair(kPass, kPass));
}

// === l06-errors-reserved-reset-code (033) ===========================================================================

TEST(Lite06ErrorsReservedResetCode, ConformingPublisherPasses) {
    auto state = std::make_shared<State>();
    const auto t = run(reserved_probe(), make_config({}, state));
    EXPECT_EQ(state->cancel_code, scen::kL06ReservedErrorCode);
    EXPECT_FALSE(state->group_stop_code.has_value());
    EXPECT_EQ(lite06::step_labelled(t, "stop-group"), nullptr);
    EXPECT_GE(lasted(t), scen::kLiteResponseAllowance);
    EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(t), kPass);
}

TEST(Lite06ErrorsReservedResetCode, AnyReactionThatGivesTheCodeAMeaningFails) {
    for (const std::uint64_t code : {0x0u, 0x2u, 0x3u}) {
        EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(run(reserved_probe(), close_on_cancel(code))), kFail)
            << code;
    }
    EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(run(reserved_probe(), close_on_cancel(0x2, polls(2500ms)))),
              kFail);
    EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(run(reserved_probe(), refuse_later())), kFail);
    EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(run(reserved_probe(), refuse_later(polls(2000ms)))), kFail);
    EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(run(reserved_probe(), reset_kept_on_cancel())), kFail);
}

TEST(Lite06ErrorsReservedResetCode, IgnoringTheCancelCannotPassAndASlowEndPasses) {
    EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(run(reserved_probe(), ignore_cancel())), kNotRun);
    Script slow;
    slow.on_cancel = [](auto&, ScriptedLitePeer&, State& state, std::uint64_t) {
        state.at(polls(2500ms), [](ConformingLitePublisher& pub, ScriptedLitePeer& p, State& s) {
            end_cancelled(pub, p, s);
        });
    };
    EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(run(reserved_probe(), slow)), kPass);
}

// === l06-errors-code-space (027 two-half rule, 107) =================================================================

TEST(Lite06ErrorsCodeSpace, BothHalvesPass) {
    auto state = std::make_shared<State>();
    const auto t = run(code_space_probe(), make_config({}, state));
    EXPECT_TRUE(state->malformed_announce);
    EXPECT_TRUE(step(t, "refused").gate_opened_ns.has_value());
    EXPECT_FALSE(step(t, "refused").gate_expired);
    EXPECT_TRUE(step(t, "announce-length").delivered());
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(judge_code_space(t), Pair(kPass, kPass));
}

TEST(Lite06ErrorsCodeSpace, TheStreamHalfAloneIsNotRunFor027) {
    // The publisher ignores the length mismatch (answers the request): no session half.
    Script script;
    script.on_length = [](ConformingLitePublisher& publisher, ScriptedLitePeer& p, State&, transport::StreamId id) {
        publisher.answer_announce(p, id, l06::AnnounceRequest{""});
    };
    const auto t = run(code_space_probe(), script);
    EXPECT_FALSE(t.peer_close.has_value());
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_code_space(t), Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsCodeSpace, TheSessionHalfAloneIsNotRunFor027) {
    // The unserved SUBSCRIBE is left pending (no stream code), the close comes.
    Script script;
    script.on_unserved = [](auto&, auto&, auto&, auto) {};
    const auto t = run(code_space_probe(), script);
    EXPECT_TRUE(step(t, "refused").gate_expired);
    EXPECT_EQ(judge_code_space(t), Pair(kNotRun, kPass));
    // Served instead of refused: likewise no stream half.
    Script served;
    served.on_unserved = [](auto&, ScriptedLitePeer& p, auto&, transport::StreamId id) {
        p.data(id, subscribe_response(l06::SubscribeOk{0}));
    };
    EXPECT_EQ(judge_code_space(run(code_space_probe(), served)), Pair(kNotRun, kPass));
}

TEST(Lite06ErrorsCodeSpace, AWrongSpaceInEitherHalfFails027) {
    // Stream half with a code only in the session table.
    Script stream_wrong;
    stream_wrong.on_unserved = [](ConformingLitePublisher& pub, ScriptedLitePeer& p, auto&, transport::StreamId id) {
        pub.refuse(p, id, 0x10);
    };
    EXPECT_EQ(judge_code_space(run(code_space_probe(), stream_wrong)), Pair(kFail, kPass));
    // Session half with a code only in the stream table: fails 027 and 107 (the catalog's documented 027 rule).
    Script session_wrong;
    session_wrong.on_length = [](auto&, ScriptedLitePeer& p, auto&, auto) { p.close_session(0x33); };
    EXPECT_EQ(judge_code_space(run(code_space_probe(), session_wrong)), Pair(kFail, kFail));
}

TEST(Lite06ErrorsCodeSpace, TheMessageLengthCloseMustBeProtocolViolation) {
    // Another session code: 027 passes (right space), 107 fails.
    Script other;
    other.on_length = [](auto&, ScriptedLitePeer& p, auto&, auto) { p.close_session(0x1); };
    EXPECT_EQ(judge_code_space(run(code_space_probe(), other)), Pair(kPass, kFail));
    // A stream reset alone: 107 fails once the allowance elapsed; no session half for 027.
    Script reset_only;
    reset_only.on_length = [](ConformingLitePublisher& pub, ScriptedLitePeer& p, auto&, transport::StreamId id) {
        pub.refuse(p, id, 0x0);
    };
    const auto reset = run(code_space_probe(), reset_only);
    EXPECT_FALSE(reset.timed_out);
    EXPECT_EQ(judge_code_space(reset), Pair(kNotRun, kFail));
    // A transport-space close carries no moq-lite code.
    Script transport_close;
    transport_close.on_length = [](auto&, ScriptedLitePeer& p, auto&, auto) {
        p.close_session(0x3, {}, transport::CloseErrorSpace::Transport);
    };
    EXPECT_EQ(judge_code_space(run(code_space_probe(), transport_close)), Pair(kNotRun, kFail));
}

TEST(Lite06ErrorsCodeSpace, ASlowCloseInsideTheAllowanceIsJudged) {
    Script slow;
    slow.on_length = [](auto&, ScriptedLitePeer&, State& state, auto) {
        state.at(polls(2500ms), [](auto&, ScriptedLitePeer& p, State&) { p.close_session(0x3); });
    };
    EXPECT_EQ(judge_code_space(run(code_space_probe(), slow)), Pair(kPass, kPass));
    Script slow_other;
    slow_other.on_length = [](auto&, ScriptedLitePeer&, State& state, auto) {
        state.at(polls(2500ms), [](auto&, ScriptedLitePeer& p, State&) { p.close_session(0x0); });
    };
    EXPECT_EQ(judge_code_space(run(code_space_probe(), slow_other)), Pair(kPass, kFail));
}

TEST(Lite06ErrorsCodeSpace, ACloseBeforeTheLengthStimulusIsNotRun) {
    // The publisher closes on the SUBSCRIBE itself: the Message Length request is never sent.
    Script script;
    script.on_unserved = [](auto&, ScriptedLitePeer& p, auto&, auto) { p.close_session(0x3); };
    const auto t = run(code_space_probe(), script);
    EXPECT_FALSE(step(t, "announce-length").executed());
    EXPECT_EQ(judge_code_space(t), Pair(kNotRun, kNotRun));
}

TEST(Lite06ErrorsCodeSpace, HandBuiltHalves) {
    // The two-half rule on hand-edited transcripts of the conforming run.
    const auto base = run(code_space_probe(), Script{});
    ASSERT_EQ(evaluate_l06_errors_code_space(base), kPass);
    auto session_only = base;
    for (auto& record : session_only.streams) {
        record.reset_code.reset();
        record.stop_sending_code.reset();
    }
    EXPECT_EQ(evaluate_l06_errors_code_space(session_only), kNotRun);
    auto stream_only = base;
    stream_only.peer_close.reset();
    EXPECT_EQ(evaluate_l06_errors_code_space(stream_only), kNotRun);
    auto wrong_stream = base;
    for (auto& record : wrong_stream.streams)
        if (record.reset_code) record.reset_code = 0x15;
    EXPECT_EQ(evaluate_l06_errors_code_space(wrong_stream), kFail);
    auto wrong_session = base;
    wrong_session.peer_close->code = 0x36;
    EXPECT_EQ(evaluate_l06_errors_code_space(wrong_session), kFail);
}

// === l06-setup-client-path (120, 124, 125) ==========================================================================

ConformingLitePublisherConfig path_config(std::optional<std::string> path) {
    auto config = make_config({});
    if (path) config.setup_parameters = {{l06::kParamPath, bytes_of(*path)}};
    return config;
}

TEST(Lite06SetupClientPath, NativeQuicWithThePathAndQueryPasses) {
    const auto t = run(client_path_probe(LiteBinding::NativeQuic), path_config(kExpectedPath));
    EXPECT_GE(lasted(t), scen::kLiteSetupAllowance);
    EXPECT_EQ(judge_path(t), Triple(kPass, kPass, kNotRun));
}

TEST(Lite06SetupClientPath, NativeQuicWithAWrongValueFailsOnly120) {
    for (const std::string value : {"/moq", "/moq?token=other", "/other?token=l1d", "", "moq?token=l1d"}) {
        const auto t = run(client_path_probe(LiteBinding::NativeQuic), path_config(value));
        EXPECT_EQ(judge_path(t), Triple(kPass, kFail, kNotRun)) << value;
    }
}

TEST(Lite06SetupClientPath, NativeQuicWithoutPathFailsOnly124) {
    const auto t = run(client_path_probe(LiteBinding::NativeQuic), path_config(std::nullopt));
    EXPECT_EQ(judge_path(t), Triple(kFail, kNotRun, kNotRun));
}

TEST(Lite06SetupClientPath, WebTransportJudgesOnlyTheAbsence) {
    EXPECT_EQ(judge_path(run(client_path_probe(LiteBinding::WebTransport), path_config(std::nullopt))),
              Triple(kNotRun, kNotRun, kPass));
    for (const std::string& value : {kExpectedPath, std::string{}}) {
        EXPECT_EQ(judge_path(run(client_path_probe(LiteBinding::WebTransport), path_config(value))),
                  Triple(kNotRun, kNotRun, kFail))
            << value;
    }
}

TEST(Lite06SetupClientPath, AnUnknownBindingIsNotRun) {
    for (const auto& path : {std::optional<std::string>{kExpectedPath}, std::optional<std::string>{}}) {
        EXPECT_EQ(judge_path(run(client_path_probe(LiteBinding::Unknown), path_config(path))),
                  Triple(kNotRun, kNotRun, kNotRun));
    }
}

TEST(Lite06SetupClientPath, WithoutASessionUrlPathAndQueryNothingIsJudged) {
    for (const auto binding : {LiteBinding::NativeQuic, LiteBinding::WebTransport}) {
        for (const auto& [path, query] : {std::pair<std::string, std::string>{"", ""}, {kUrlPath, ""},
                                          {"", kUrlQuery}}) {
            auto definition = scen::l06_setup_client_path_probe(kDeadline, path, query);
            definition.binding = binding;
            // A publisher that would fail every row it can.
            const auto t = run(definition, path_config(binding == LiteBinding::WebTransport
                                                           ? std::optional<std::string>{"/x"}
                                                           : std::nullopt));
            EXPECT_EQ(judge_path(t), Triple(kNotRun, kNotRun, kNotRun)) << path << "?" << query;
        }
        // The flag unset although the strings are present (Task 9 says no path was given).
        auto definition = client_path_probe(binding);
        definition.session_url_has_path = false;
        EXPECT_EQ(judge_path(run(definition, path_config(std::nullopt))), Triple(kNotRun, kNotRun, kNotRun));
    }
}

TEST(Lite06SetupClientPath, NoPublisherSetupIsNotRun) {
    auto config = path_config(kExpectedPath);
    config.defect = LiteDefect::NoSetupStream;
    for (const auto binding : {LiteBinding::NativeQuic, LiteBinding::WebTransport}) {
        EXPECT_EQ(judge_path(run(client_path_probe(binding), config)), Triple(kNotRun, kNotRun, kNotRun));
    }
}

// === attribution ====================================================================================================

LiteTranscript conforming(const std::function<LiteProbeDefinition()>& probe) {
    auto definition = probe();
    if (definition.id == scen::kL06SetupClientPath) return run(definition, path_config(kExpectedPath));
    return run(definition, Script{});
}

TEST(Lite06ErrorsAttribution, EachEvaluatorJudgesOnlyItsOwnScenario) {
    const std::vector<std::string> planned{
        "l06-announce-lifecycle",          "l06-announce-prefix",
        "l06-errors-code-space",           "l06-errors-reserved-reset-code",
        "l06-errors-unknown-reset-code",   "l06-errors-unknown-stream-type",
        "l06-session-stream-close",        "l06-setup-client-path",
        "l06-setup-duplicate-parameter",   "l06-setup-duplicate-stream",
        "l06-setup-server-path",           "l06-setup-server-role",
        "l06-setup-stream",                "l06-setup-unknown-parameter",
        "l06-subscribe-abutting-frame-start", "l06-subscribe-group-floor",
        "l06-subscribe-invalid-frame-bounds", "l06-subscribe-latest",
        "l06-subscribe-refused"};
    for (const auto& probe : every_probe()) {
        const auto base = conforming(probe);
        // Every evaluator of this family passes or is NotRun on its own scenario, and something is judged.
        bool judged = false;
        for (const auto evaluator : all_evaluators()) {
            const auto verdict = evaluator(base);
            EXPECT_NE(verdict, kFail) << base.scenario_id;
            judged = judged || verdict.has_value();
        }
        EXPECT_TRUE(judged) << base.scenario_id;
        // The Task 4-6 evaluators are NotRun on these transcripts; 027 only on its own scenario.
        for (const auto evaluator : earlier_evaluators()) EXPECT_EQ(evaluator(base), kNotRun) << base.scenario_id;
        if (base.scenario_id != scen::kL06ErrorsCodeSpace) {
            EXPECT_EQ(evaluate_l06_errors_code_space(base), kNotRun) << base.scenario_id;
        }
        // Each new evaluator is NotRun on the same transcript relabelled as any other scenario.
        for (const auto& id : planned) {
            if (id == base.scenario_id) continue;
            auto relabelled = base;
            relabelled.scenario_id = id;
            for (const auto evaluator : new_evaluators())
                EXPECT_EQ(evaluator(relabelled), kNotRun) << base.scenario_id << " as " << id;
        }
    }
}

TEST(Lite06ErrorsAttribution, OneDefectFailsExactlyItsEvaluators) {
    // (108, 109, 030, 032, 033, 027, 107, 124, 120, 125)
    struct Case {
        std::string name;
        LiteTranscript transcript;
        std::vector<Verdict> expected;
    };
    const auto N = kNotRun;
    const auto P = kPass;
    const auto F = kFail;
    auto ignoring = make_config({});
    ignoring.hooks.on_request = nullptr;
    ignoring.defect = LiteDefect::IgnoreUnknownStreams;
    Script fin_unknown;
    fin_unknown.on_unknown = unknown_reaction([](ScriptedLitePeer& p, auto id) { p.fin(id); });
    Script close_unknown;
    close_unknown.on_unknown = unknown_reaction([](ScriptedLitePeer& p, auto) { p.close_session(0x3); });
    Script ignore_length;
    ignore_length.on_length = [](ConformingLitePublisher& pub, ScriptedLitePeer& p, State&, transport::StreamId id) {
        pub.answer_announce(p, id, l06::AnnounceRequest{""});
    };
    Script stream_wrong;
    stream_wrong.on_unserved = [](ConformingLitePublisher& pub, ScriptedLitePeer& p, auto&, transport::StreamId id) {
        pub.refuse(p, id, 0x10);
    };
    const std::vector<Case> cases{
        {"unknown conforming", conforming(every_probe()[0]), {P, P, N, N, N, N, N, N, N, N}},
        {"unknown ignored", run(unknown_stream_probe(), ignoring), {F, P, N, N, N, N, N, N, N, N}},
        {"unknown FIN", run(unknown_stream_probe(), fin_unknown), {F, P, N, N, N, N, N, N, N, N}},
        {"unknown close", run(unknown_stream_probe(), close_unknown), {N, F, N, N, N, N, N, N, N, N}},
        {"code conforming", conforming(every_probe()[1]), {N, N, P, P, N, N, N, N, N, N}},
        {"code unauthorized", run(unknown_code_probe(), close_on_cancel(0x2)), {N, N, N, F, N, N, N, N, N, N}},
        {"code other close", run(unknown_code_probe(), close_on_cancel(0x0)), {N, N, F, N, N, N, N, N, N, N}},
        {"code later refused", run(unknown_code_probe(), refuse_later()), {N, N, F, P, N, N, N, N, N, N}},
        {"code ignored", run(unknown_code_probe(), ignore_cancel()), {N, N, N, N, N, N, N, N, N, N}},
        {"reserved conforming", conforming(every_probe()[2]), {N, N, N, N, P, N, N, N, N, N}},
        {"reserved close", run(reserved_probe(), close_on_cancel(0x0)), {N, N, N, N, F, N, N, N, N, N}},
        {"space conforming", conforming(every_probe()[3]), {N, N, N, N, N, P, P, N, N, N}},
        {"space length ignored", run(code_space_probe(), ignore_length), {N, N, N, N, N, N, F, N, N, N}},
        {"space stream wrong", run(code_space_probe(), stream_wrong), {N, N, N, N, N, F, P, N, N, N}},
        {"path conforming", conforming(every_probe()[4]), {N, N, N, N, N, N, N, P, P, N}},
        {"path no query", run(client_path_probe(), path_config("/moq")), {N, N, N, N, N, N, N, P, F, N}},
        {"path absent", run(client_path_probe(), path_config(std::nullopt)), {N, N, N, N, N, N, N, F, N, N}},
        {"path on WebTransport", run(client_path_probe(LiteBinding::WebTransport), path_config(kExpectedPath)),
         {N, N, N, N, N, N, N, N, N, F}},
    };
    for (const auto& c : cases) EXPECT_EQ(verdicts(c.transcript), c.expected) << c.name;
}

// === NotRun =========================================================================================================

TEST(Lite06ErrorsNotRun, EmptyTranscriptIsNeverAPass) {
    const LiteTranscript empty;
    for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(empty), kNotRun);
    for (const auto& probe : every_probe()) {
        LiteTranscript t;
        const auto d = probe();
        t.scenario_id = d.id;
        t.broadcast_path = d.broadcast_path;
        t.track_name = d.track_name;
        t.binding = d.binding;
        t.session_url_has_path = d.session_url_has_path;
        t.session_url_path = d.session_url_path;
        t.session_url_query = d.session_url_query;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06ErrorsNotRun, PublisherThatNeverConnects) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.connect_deadline = 100ms;
        const auto t = run(std::move(definition), make_config({}),
                           [](ScriptedLitePeer& peer) { peer.establish_on_poll.reset(); });
        EXPECT_FALSE(t.established);
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06ErrorsNotRun, WrongAlpnIsAHarnessFailure) {
    for (const auto& probe : every_probe()) {
        ConformingLitePublisher publisher(make_config({}));
        ScriptedLitePeer peer(publisher.reaction(), "moqt-22");
        ManualLiteClock clock;
        const auto t = run_lite_probe(peer, probe(), clock, kTick);
        ASSERT_TRUE(t.harness_failed) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

std::vector<LiteTranscript> failing_bases() {
    auto ignoring = make_config({});
    ignoring.hooks.on_request = nullptr;
    ignoring.defect = LiteDefect::IgnoreUnknownStreams;
    Script ignore_length;
    ignore_length.on_length = [](ConformingLitePublisher& pub, ScriptedLitePeer& p, State&, transport::StreamId id) {
        pub.answer_announce(p, id, l06::AnnounceRequest{""});
    };
    return {run(unknown_stream_probe(), ignoring), run(unknown_code_probe(), refuse_later()),
            run(reserved_probe(), refuse_later()), run(code_space_probe(), ignore_length),
            run(client_path_probe(), path_config(std::nullopt))};
}

TEST(Lite06ErrorsNotRun, FlagsOnAnOtherwiseJudgedTranscript) {
    for (const auto& base : failing_bases()) {
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

TEST(Lite06ErrorsNotRun, EventLimit) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.limits.max_bytes = 4;
        const auto t = run(std::move(definition), make_config({}));
        ASSERT_TRUE(t.event_limit_reached) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06ErrorsNotRun, AHarnessClassIssueFromThePeer) {
    // Bytes after a FIN on a publisher Group stream (trailing_after_fin, Harness class): never a Fail, never a Pass,
    // on a conforming run and on a run that fails otherwise.
    std::vector<std::pair<LiteProbeDefinition, ConformingLitePublisherConfig>> runs;
    for (const auto& probe : every_probe()) runs.emplace_back(probe(), probe().id == scen::kL06SetupClientPath
                                                                           ? path_config(kExpectedPath)
                                                                           : make_config({}));
    auto ignoring = make_config({});
    ignoring.hooks.on_request = nullptr;
    ignoring.defect = LiteDefect::IgnoreUnknownStreams;
    runs.emplace_back(unknown_stream_probe(), ignoring);
    runs.emplace_back(unknown_code_probe(), make_config(refuse_later()));
    runs.emplace_back(reserved_probe(), make_config(refuse_later()));
    runs.emplace_back(client_path_probe(), path_config(std::nullopt));
    for (auto& [definition, config] : runs) {
        config.hooks.on_start = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
            publisher.send_setup(peer);
            const auto id = peer.open_peer_uni();
            peer.data(id, join({stream_type(0x0), group_header({9, 5, 0})}), true);
            peer.data(id, bytes_of("late"));
            return true;
        };
        const auto t = run(definition, config);
        ASSERT_FALSE(judgeable(t)) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06ErrorsNotRun, StimulusNeverDelivered) {
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

TEST(Lite06ErrorsNotRun, RunnerSetupRefusedByThePeer) {
    for (const auto& probe : every_probe()) {
        const auto t = run(probe(), make_config({}),
                           [](ScriptedLitePeer& peer) {
                               peer.forced_status[3] = transport::TransportStatus::PeerStopped;
                           });
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06ErrorsNotRun, TheFirstStimulusWriteRefusedByThePeer) {
    // The publisher stopped the runner's first bidirectional stream (1) before the write.
    for (const auto& probe : {unknown_stream_probe, unknown_code_probe, reserved_probe, code_space_probe}) {
        const auto t = run(probe(), make_config({}),
                           [](ScriptedLitePeer& peer) {
                               peer.forced_status[1] = transport::TransportStatus::PeerStopped;
                           });
        if (t.scenario_id == scen::kL06ErrorsUnknownStreamType) {
            EXPECT_EQ(judge_unknown(t), Pair(kNotRun, kNotRun));
        } else if (t.scenario_id == scen::kL06ErrorsCodeSpace) {
            EXPECT_EQ(evaluate_l06_errors_code_space(t), kNotRun);
        } else {
            EXPECT_EQ(judge_code(t), Pair(kNotRun, kNotRun)) << t.scenario_id;
            EXPECT_EQ(evaluate_l06_errors_reserved_code_tolerated(t), kNotRun);
        }
    }
}

TEST(Lite06ErrorsNotRun, AFixtureMismatchIsNotRun) {
    for (const auto& [probe, script] : {std::pair<std::function<LiteProbeDefinition()>, Script>{unknown_code_probe,
                                                                                                 refuse_later()},
                                        {reserved_probe, refuse_later()}}) {
        for (const auto& [path, track] :
             {std::pair<std::string, std::string>{"elsewhere/live", kTrack}, {kBroadcast, "audio"}, {"", ""}}) {
            auto t = run(probe(), script);
            t.broadcast_path = path;
            t.track_name = track;
            for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
        }
    }
}
