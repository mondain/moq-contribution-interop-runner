#include "moq/interop/scenarios/lite06_errors.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/session/lite_session.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::scenarios {
namespace {

namespace l06 = wire::moqlite06;
using lite06::allowance_step;
using lite06::proved_stimulus;
using lite06::step_labelled;
using session::LiteStreamRecord;

void require_positive(std::chrono::milliseconds value, const char* what) {
    if (value.count() <= 0)
        throw std::invalid_argument(std::string("a moq-lite-06 error probe needs a positive ") + what);
}

// The skeleton: id, deadline (checked against `needed`), zero observation window.
LiteProbeDefinition error_probe(std::string_view id, std::chrono::milliseconds deadline,
                                std::chrono::milliseconds allowance, std::chrono::milliseconds needed) {
    require_positive(allowance, "allowance");
    if (deadline <= needed)
        throw std::invalid_argument("a moq-lite-06 error probe needs a deadline beyond its allowances");
    return lite06::allowance_probe(id, deadline, allowance);
}

// The runner Subscribe stream whose own SUBSCRIBE names `subscribe_id` (the runner's writes are decoded too).
const LiteStreamRecord* runner_subscribe_stream(const session::LiteSession& session, std::uint64_t subscribe_id) {
    for (const auto* record : session::runner_streams(session)) {
        if (record->kind != session::LiteStreamKind::Subscribe) continue;
        for (const auto* message : session::runner_messages(*record)) {
            const auto* subscribe = std::get_if<l06::Subscribe>(&message->message);
            if (subscribe && subscribe->subscribe_id == subscribe_id) return record;
        }
    }
    return nullptr;
}

// The publisher answered the subscription with SUBSCRIBE_OK and has not ended the stream.
bool subscription_live(const session::LiteSession& session, std::uint64_t subscribe_id) {
    const auto* record = runner_subscribe_stream(session, subscribe_id);
    if (!record || record->fin_seen || record->reset_seen || record->stop_sending_seen) return false;
    const auto messages = session::peer_messages(*record);
    return !messages.empty() && std::holds_alternative<l06::SubscribeOk>(messages.front()->message);
}

bool both_live(const session::LiteSession& session) {
    return subscription_live(session, kL06CancelledSubscribeId) && subscription_live(session, kL06KeptSubscribeId);
}

// A Group stream of the cancelled subscription the publisher has not ended yet (its GROUP decoded).
std::optional<transport::StreamId> open_group_of_cancelled(const session::LiteSession& session) {
    for (const auto* record : session::peer_streams(session)) {
        if (record->kind != session::LiteStreamKind::Group || record->fin_seen || record->reset_seen) continue;
        const auto messages = session::peer_messages(*record);
        if (messages.empty()) continue;
        const auto* header = std::get_if<l06::GroupHeader>(&messages.front()->message);
        if (header && header->subscribe_id == kL06CancelledSubscribeId) return record->stream_id;
    }
    return std::nullopt;
}

// The publisher reset, stopped or ended the stream of the code-space probe's SUBSCRIBE.
bool unserved_answered(const session::LiteSession& session) {
    const auto* record = runner_subscribe_stream(session, kL06UnservedSubscribeId);
    return record && (record->reset_seen || record->stop_sending_seen || record->fin_seen);
}

std::vector<std::byte> cancel_subscribe_bytes(std::uint64_t id, std::string_view path, std::string_view track) {
    return l06_subscribe_bytes(l06_subscribe(id, path, track));
}

LiteProbeDefinition reset_code_probe(std::string_view id, std::chrono::milliseconds deadline,
                                     std::string_view broadcast_path, std::string_view track_name,
                                     std::chrono::milliseconds allowance, std::chrono::milliseconds answer_allowance,
                                     std::uint64_t code, bool stop_group) {
    require_positive(answer_allowance, "answer allowance");
    if (l06_path_segments(broadcast_path).empty() || l06_path_segments(track_name).empty())
        throw std::invalid_argument("a moq-lite-06 reset-code probe needs the track fixture");
    const auto needed = (stop_group ? answer_allowance * 2 : answer_allowance) + allowance;
    auto definition = error_probe(id, deadline, allowance, needed);
    definition.requires_track = true;
    definition.broadcast_path = std::string(broadcast_path);
    definition.track_name = std::string(track_name);
    // Ungated and consecutive: both subscriptions are live together.
    definition.steps.push_back(lite_open_bidi(
        cancel_subscribe_bytes(kL06CancelledSubscribeId, broadcast_path, track_name), false,
        std::string(kL06CancelledLabel)));
    definition.steps.push_back(lite_open_bidi(cancel_subscribe_bytes(kL06KeptSubscribeId, broadcast_path, track_name),
                                              false, std::string(kL06KeptLabel)));
    auto live = lite_wait(std::chrono::milliseconds{0}, std::string(kL06LiveLabel));
    live.gate = both_live;
    live.gate_deadline = answer_allowance;
    definition.steps.push_back(std::move(live));
    const std::size_t cancelled = 0;
    if (stop_group) {
        LiteStep stop;
        stop.kind = LiteStep::Kind::StopSending;
        stop.code = code;
        stop.label = std::string(kL06StopGroupLabel);
        stop.gate = [](const session::LiteSession& session) { return open_group_of_cancelled(session).has_value(); };
        stop.gate_deadline = answer_allowance;
        stop.target = open_group_of_cancelled;
        definition.steps.push_back(std::move(stop));
    }
    definition.steps.push_back(lite_reset(cancelled, code, std::string(kL06CancelResetLabel)));
    definition.steps.push_back(lite_stop_sending(cancelled, code, std::string(kL06CancelStopLabel)));
    definition.steps.push_back(lite_open_bidi(cancel_subscribe_bytes(kL06LaterSubscribeId, broadcast_path, track_name),
                                              false, std::string(kL06LaterLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

}  // namespace

std::vector<std::byte> l06_unknown_stream_type_bytes() {
    wire::ByteWriter out(16);
    if (!l06::write_stream_type(kL06UnregisteredStreamType, out)) return {};
    return {out.bytes().begin(), out.bytes().end()};
}

std::vector<std::byte> l06_message_length_extra_bytes() {
    wire::ByteWriter body(64);
    bool ok = l06::write_string("", body);
    for (const auto c : kL06LengthTrailing) ok = ok && body.append_byte(static_cast<std::byte>(c));
    wire::ByteWriter out(128);
    ok = ok && l06::write_stream_type(static_cast<std::uint64_t>(l06::BidiStreamType::Announce), out) &&
         l06::write_framed_message(body.bytes(), out);
    if (!ok) return {};
    return {out.bytes().begin(), out.bytes().end()};
}

std::vector<std::byte> l06_unserved_subscribe_bytes() {
    return l06_subscribe_bytes(l06_subscribe(kL06UnservedSubscribeId, kL06UnservedBroadcast, kL06UnservedTrack));
}

std::string l06_expected_client_path(std::string_view url_path, std::string_view url_query) {
    std::string value(url_path);
    if (!url_query.empty()) value += "?" + std::string(url_query);
    return value;
}

LiteProbeDefinition l06_errors_unknown_stream_type_probe(std::chrono::milliseconds deadline,
                                                         std::chrono::milliseconds allowance) {
    auto definition = error_probe(kL06ErrorsUnknownStreamType, deadline, allowance, allowance);
    // Only the STREAM_TYPE, no FIN: the publisher's reset of the stream is the observation.
    definition.steps.push_back(
        lite_open_bidi(l06_unknown_stream_type_bytes(), false, std::string(kL06UnknownStreamLabel)));
    definition.steps.push_back(
        lite_open_bidi(l06_announce_request_bytes(""), false, std::string(kL06ErrAnnounceLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_errors_unknown_reset_code_probe(std::chrono::milliseconds deadline,
                                                        std::string_view broadcast_path, std::string_view track_name,
                                                        std::chrono::milliseconds allowance,
                                                        std::chrono::milliseconds answer_allowance) {
    return reset_code_probe(kL06ErrorsUnknownResetCode, deadline, broadcast_path, track_name, allowance,
                            answer_allowance, kL06UnknownErrorCode, true);
}

LiteProbeDefinition l06_errors_reserved_reset_code_probe(std::chrono::milliseconds deadline,
                                                         std::string_view broadcast_path, std::string_view track_name,
                                                         std::chrono::milliseconds allowance,
                                                         std::chrono::milliseconds answer_allowance) {
    return reset_code_probe(kL06ErrorsReservedResetCode, deadline, broadcast_path, track_name, allowance,
                            answer_allowance, kL06ReservedErrorCode, false);
}

LiteProbeDefinition l06_errors_code_space_probe(std::chrono::milliseconds deadline,
                                                std::chrono::milliseconds allowance,
                                                std::chrono::milliseconds answer_allowance) {
    require_positive(answer_allowance, "answer allowance");
    auto definition = error_probe(kL06ErrorsCodeSpace, deadline, allowance, answer_allowance + allowance);
    definition.steps.push_back(
        lite_open_bidi(l06_unserved_subscribe_bytes(), false, std::string(kL06UnservedLabel)));
    auto refused = lite_wait(std::chrono::milliseconds{0}, std::string(kL06RefusedLabel));
    refused.gate = unserved_answered;
    refused.gate_deadline = answer_allowance;
    definition.steps.push_back(std::move(refused));
    definition.steps.push_back(
        lite_open_bidi(l06_message_length_extra_bytes(), false, std::string(kL06LengthLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_setup_client_path_probe(std::chrono::milliseconds deadline, std::string_view url_path,
                                                std::string_view url_query, std::chrono::milliseconds allowance) {
    auto definition = error_probe(kL06SetupClientPath, deadline, allowance, allowance);
    definition.session_url_path = std::string(url_path);
    definition.session_url_query = std::string(url_query);
    definition.session_url_has_path = !url_path.empty();
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

namespace {

bool runner_setup_proved(const LiteTranscript& transcript) {
    return proved_stimulus(transcript, lite06::kRunnerSetupLabel, lite_default_runner_setup()) != nullptr;
}

bool fixture_present(const LiteTranscript& transcript) {
    return !l06_path_segments(transcript.broadcast_path).empty() && !l06_path_segments(transcript.track_name).empty();
}

// The runner stream of a stimulus step whose exact bytes reached the publisher; nullptr otherwise.
const LiteStreamRecord* stimulus_stream(const LiteTranscript& transcript, std::string_view label,
                                        std::span<const std::byte> expected) {
    const auto* step = proved_stimulus(transcript, label, expected);
    if (!step || !step->stream_id) return nullptr;
    return lite06::find_stream(transcript, *step->stream_id);
}

bool refused_by_peer(const LiteStepRecord& step) {
    return step.refused == transport::TransportStatus::PeerReset ||
           step.refused == transport::TransportStatus::PeerStopped;
}

// The first time the publisher ended its send direction of `stream_id` (FIN or RESET_STREAM), from the transport
// events (the peer's direction only; the precedent of row 025's evaluator).
std::optional<std::uint64_t> peer_end_ns(const LiteTranscript& transcript, std::uint64_t stream_id) {
    for (std::size_t i = 0; i < transcript.events.size() && i < transcript.event_times.size(); ++i) {
        const auto& event = transcript.events[i];
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && data->stream_id == stream_id && data->fin)
            return transcript.event_times[i];
        if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event); reset && reset->stream_id == stream_id)
            return transcript.event_times[i];
    }
    return std::nullopt;
}

// The first time the publisher refused `stream_id`: a RESET_STREAM or a STOP_SENDING.
std::optional<std::uint64_t> peer_refusal_ns(const LiteTranscript& transcript, std::uint64_t stream_id) {
    for (std::size_t i = 0; i < transcript.events.size() && i < transcript.event_times.size(); ++i) {
        const auto& event = transcript.events[i];
        if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event); reset && reset->stream_id == stream_id)
            return transcript.event_times[i];
        if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event);
            stop && stop->stream_id == stream_id)
            return transcript.event_times[i];
    }
    return std::nullopt;
}

// --- l06-errors-unknown-reset-code / l06-errors-reserved-reset-code ---

enum class Judged { Pass, Fail, Open };

// What the reset-code evaluators share, once the gate holds.
struct CancelView {
    // The earliest time a step carrying the code reached the publisher (the Group stream STOP_SENDING or the
    // cancel's RESET_STREAM).
    std::uint64_t code_ns{0};
    // Both halves of the cancel (RESET_STREAM and STOP_SENDING of A) delivered with the code.
    bool cancel_delivered{false};
    const LiteStreamRecord* cancelled{nullptr};  // A
    const LiteStreamRecord* kept{nullptr};       // B
};

// A code-carrying step delivered with `code`; nullptr otherwise.
const LiteStepRecord* code_step(const LiteTranscript& transcript, std::string_view label, std::uint64_t code) {
    const auto* step = proved_stimulus(transcript, label);
    if (!step || step->code != code) return nullptr;
    return step;
}

// The gate of rows 030, 032 and 033: the scenario, judgeable() (a session close is part of the observation), the
// runner's Setup, the fixture, both subscriptions proven with the fixture's exact bytes and live (the "live" Wait
// opened: SUBSCRIBE_OK on both and neither ended), and a step carrying `code` delivered. nullopt otherwise.
std::optional<CancelView> cancel_view(const LiteTranscript& transcript, std::string_view scenario_id,
                                      std::uint64_t code) {
    if (transcript.scenario_id != scenario_id || !judgeable(transcript) || transcript.runner_closed)
        return std::nullopt;
    if (!runner_setup_proved(transcript) || !fixture_present(transcript)) return std::nullopt;
    const auto& path = transcript.broadcast_path;
    const auto& track = transcript.track_name;
    CancelView view;
    view.cancelled = stimulus_stream(transcript, kL06CancelledLabel,
                                     cancel_subscribe_bytes(kL06CancelledSubscribeId, path, track));
    view.kept = stimulus_stream(transcript, kL06KeptLabel, cancel_subscribe_bytes(kL06KeptSubscribeId, path, track));
    if (!view.cancelled || !view.kept) return std::nullopt;
    const auto* live = step_labelled(transcript, kL06LiveLabel);
    if (!live || !live->executed() || live->gate_expired) return std::nullopt;  // not two live subscriptions
    const auto* group_stop = code_step(transcript, kL06StopGroupLabel, code);
    const auto* reset = code_step(transcript, kL06CancelResetLabel, code);
    const auto* stop = code_step(transcript, kL06CancelStopLabel, code);
    // The cancel acts on A's stream (step 0).
    const std::optional<transport::StreamId> a{view.cancelled->stream_id};
    if (reset && reset->stream_id != a) reset = nullptr;
    if (stop && stop->stream_id != a) stop = nullptr;
    view.cancel_delivered = reset && stop;
    std::optional<std::uint64_t> first;
    for (const auto* step : {group_stop, reset}) {
        if (step && (!first || *step->executed_at_ns < *first)) first = *step->executed_at_ns;
    }
    if (!first) return std::nullopt;  // no code reached the publisher
    view.code_ns = *first;
    return view;
}

// A and B were both still live when the code arrived (neither ended nor refused before it). The engine handles
// every event of a poll before it runs that poll's steps and stamps both with the same time, so a peer event AT
// code_ns arrived before the code was sent: `<=`, never `<` (otherwise a FIN of A in that poll would be credited as
// the cancel's end, and a reset of B in that poll counted as a refusal after the code). Every later use of A's end
// and B's refusal relies on this precondition, so whatever passes it happened strictly after the code.
bool live_at_code(const LiteTranscript& transcript, const CancelView& view) {
    for (const auto* record : {view.cancelled, view.kept}) {
        if (const auto at = peer_end_ns(transcript, record->stream_id); at && *at <= view.code_ns) return false;
        if (const auto at = peer_refusal_ns(transcript, record->stream_id); at && *at <= view.code_ns) return false;
    }
    return true;
}

// The judgement of rows 030 and 033 once no session close decided it: the precondition (A and B still live when
// the code arrived), the refusal of B or C after it (Fail), then the Pass evidence (A ended after the cancel, the
// whole allowance elapsed, C answered).
Judged judge_tolerated(const LiteTranscript& transcript, const CancelView& view) {
    if (!live_at_code(transcript, view)) return Judged::Open;
    // The later request refused (its write refused, or the stream reset or stopped by the publisher).
    const auto* later = step_labelled(transcript, kL06LaterLabel);
    if (later && refused_by_peer(*later)) return Judged::Fail;
    const auto* c = stimulus_stream(
        transcript, kL06LaterLabel,
        cancel_subscribe_bytes(kL06LaterSubscribeId, transcript.broadcast_path, transcript.track_name));
    if (c && peer_refusal_ns(transcript, c->stream_id)) return Judged::Fail;
    // The other subscription refused after the code (a FIN may be the track ending: not judged).
    if (peer_refusal_ns(transcript, view.kept->stream_id)) return Judged::Fail;
    // Pass evidence: the cancel delivered and its subscription ended by the publisher (FIN or RESET_STREAM), so a
    // publisher that ignores the cancel cannot pass; the session open for the whole allowance; C answered.
    if (!view.cancel_delivered || !c) return Judged::Open;
    if (!peer_end_ns(transcript, view.cancelled->stream_id)) return Judged::Open;
    if (!lite06::allowance_elapsed(transcript)) return Judged::Open;
    if (view.kept->fin_seen) return Judged::Open;  // B ended: the session's usability is not shown by it
    const auto messages = session::peer_messages(*c);
    if (messages.empty() || !std::holds_alternative<l06::SubscribeOk>(messages.front()->message))
        return Judged::Open;  // C unanswered (or answered otherwise): not this row's evidence
    return Judged::Pass;
}

std::optional<bool> to_verdict(Judged judged) {
    if (judged == Judged::Open) return std::nullopt;
    return judged == Judged::Pass;
}

bool unauthorized_close(const LiteTranscript& transcript) {
    return lite06::session_close_code(transcript) == std::optional<std::uint64_t>{kL06Unauthorized};
}

// --- l06-setup-client-path ---

// The publisher's SETUP Path values, once the transcript may be judged: the scenario, judgeable_with_stimulus, the
// runner's Setup, a session URL with a path and a query given to the publisher, a known binding, and a complete
// publisher SETUP (its absence or broken framing is row 014's matter). nullopt otherwise.
std::optional<std::vector<std::vector<std::byte>>> client_paths(const LiteTranscript& transcript) {
    // The engine closing the session for a Path on WebTransport (LiteRunnerDuties::close_on_webtransport_path) ends
    // the probe before its allowance: the SETUP it closed on is the observation, so that close stands in for the
    // stimulus (it fires only on a decoded SETUP carrying Path, so it can only confirm a row 125 Fail).
    const bool closed_for_path =
        judgeable(transcript) && transcript.runner_closed_for_path && !transcript.peer_closed_early;
    if (transcript.scenario_id != kL06SetupClientPath || !(judgeable_with_stimulus(transcript) || closed_for_path))
        return std::nullopt;
    if (!runner_setup_proved(transcript)) return std::nullopt;
    // A non-empty query is required for all three rows (the task brief: the scenario only has meaning with a URL
    // carrying a path AND a query). For 124 and 125 this is stricter than the rationales, which put no condition on
    // the URL: it can only lose verdicts (NotRun), never turn one.
    if (!transcript.session_url_has_path || transcript.session_url_path.empty() ||
        transcript.session_url_query.empty())
        return std::nullopt;
    if (transcript.binding == LiteBinding::Unknown) return std::nullopt;
    const auto setup = lite06::peer_setup_message(transcript);
    if (!setup || !setup->message || setup->malformed) return std::nullopt;
    std::vector<std::vector<std::byte>> values;
    for (const auto& parameter : setup->message->parameters)
        if (parameter.id == l06::kParamPath) values.push_back(parameter.value);
    return values;
}

}  // namespace

std::optional<bool> evaluate_l06_errors_unknown_stream_type_reset(const LiteTranscript& transcript) {
    // judgeable() plus this row's own proofs rather than judgeable_with_stimulus(): a refused follow-up ANNOUNCE
    // write (row 109's stimulus) must not lose this verdict. Any session close makes the reset unobservable, so this
    // row is NotRun and the close is judged by row 109 only; the observation is over only once the allowance step
    // executed with the session open.
    if (transcript.scenario_id != kL06ErrorsUnknownStreamType || !judgeable(transcript) || transcript.peer_close ||
        transcript.runner_closed)
        return std::nullopt;
    if (!runner_setup_proved(transcript)) return std::nullopt;
    const auto* stream = stimulus_stream(transcript, kL06UnknownStreamLabel, l06_unknown_stream_type_bytes());
    if (!stream || !lite06::allowance_elapsed(transcript)) return std::nullopt;
    // A RESET_STREAM of the publisher's send half or a STOP_SENDING of the runner's: the stream is reset. A FIN
    // without either, or nothing within the whole allowance, is a Fail (time-bounded).
    return stream->reset_seen || stream->stop_sending_seen;
}

std::optional<bool> evaluate_l06_errors_unknown_stream_type_not_fatal(const LiteTranscript& transcript) {
    // A close is a Fail here, so the gate is judgeable() plus the proof of the unknown stream.
    if (transcript.scenario_id != kL06ErrorsUnknownStreamType || !judgeable(transcript) || transcript.runner_closed)
        return std::nullopt;
    if (!runner_setup_proved(transcript) ||
        !proved_stimulus(transcript, kL06UnknownStreamLabel, l06_unknown_stream_type_bytes()))
        return std::nullopt;
    if (transcript.peer_close) return false;  // any code, any space, after the unknown stream was delivered
    const auto* announce = step_labelled(transcript, kL06ErrAnnounceLabel);
    if (announce && refused_by_peer(*announce)) return false;  // the follow-up request refused
    const auto* stream = stimulus_stream(transcript, kL06ErrAnnounceLabel, l06_announce_request_bytes(""));
    if (!stream) return std::nullopt;
    if (stream->reset_seen || stream->stop_sending_seen) return false;  // refused (also after an ANNOUNCE_OK)
    if (!lite06::allowance_elapsed(transcript)) return std::nullopt;
    // The session stayed open for the window; the request must be answered with ANNOUNCE_OK. No answer (or an
    // unreadable one) without a close or a refusal belongs to the announce rows: NotRun.
    const auto messages = session::peer_messages(*stream);
    if (!messages.empty() && std::holds_alternative<l06::AnnounceOk>(messages.front()->message)) return true;
    return std::nullopt;
}

std::optional<bool> evaluate_l06_errors_unknown_code_tolerated(const LiteTranscript& transcript) {
    const auto view = cancel_view(transcript, kL06ErrorsUnknownResetCode, kL06UnknownErrorCode);
    if (!view) return std::nullopt;
    if (transcript.peer_close) {
        // UNAUTHORIZED is row 032's alone (one defect never fails both rows); any other close, in either space,
        // after the code reached the publisher is a Fail (the Group stream STOP_SENDING included: row 098 note).
        if (unauthorized_close(transcript)) return std::nullopt;
        return false;
    }
    return to_verdict(judge_tolerated(transcript, *view));
}

std::optional<bool> evaluate_l06_errors_no_assumed_unauthorized(const LiteTranscript& transcript) {
    const auto view = cancel_view(transcript, kL06ErrorsUnknownResetCode, kL06UnknownErrorCode);
    if (!view) return std::nullopt;
    if (transcript.peer_close) {
        // Only a session close with UNAUTHORIZED (session code 0x2) is an authorization inference; another close
        // is row 030's matter and ends the observation.
        if (unauthorized_close(transcript)) return false;
        return std::nullopt;
    }
    if (!live_at_code(transcript, *view)) return std::nullopt;
    // The cancel took effect (A ended by the publisher, so ignoring it cannot pass) and the session stayed open for
    // the whole allowance: no authorization inference. Refusals with stream codes (DELIVERY_TIMEOUT 0x2 included)
    // are row 030's.
    if (!view->cancel_delivered || !peer_end_ns(transcript, view->cancelled->stream_id)) return std::nullopt;
    if (!lite06::allowance_elapsed(transcript)) return std::nullopt;
    return true;
}

std::optional<bool> evaluate_l06_errors_reserved_code_tolerated(const LiteTranscript& transcript) {
    const auto view = cancel_view(transcript, kL06ErrorsReservedResetCode, kL06ReservedErrorCode);
    if (!view) return std::nullopt;
    if (transcript.peer_close) return false;  // any close gives the reserved value a meaning
    return to_verdict(judge_tolerated(transcript, *view));
}

std::optional<bool> evaluate_l06_errors_message_length_close(const LiteTranscript& transcript) {
    // The close judged is the first one after the Message Length request was sent. When the `refused` step's gate
    // expired (step record gate_expired: the unserved SUBSCRIBE got no refusal within its allowance), a close may
    // still be the publisher's late reaction to that SUBSCRIBE; the step record is in the transcript (Task 8 records
    // it in the evidence) so a reader can discount such a close.
    // The close probe verdict (PROTOCOL_VIOLATION Pass; another close, a stream reaction alone or a session still
    // open when the allowance elapsed Fail), on the Message Length stimulus proven with its exact bytes.
    if (!runner_setup_proved(transcript)) return std::nullopt;
    return lite06::judge_close_probe(transcript, kL06ErrorsCodeSpace,
                                     proved_stimulus(transcript, kL06LengthLabel, l06_message_length_extra_bytes()));
}

std::optional<bool> evaluate_l06_setup_path_sent(const LiteTranscript& transcript) {
    // Row 124: binding 1 (native QUIC) only.
    if (transcript.binding != LiteBinding::NativeQuic) return std::nullopt;
    const auto paths = client_paths(transcript);
    if (!paths) return std::nullopt;
    return !paths->empty();  // an empty value counts as sent
}

std::optional<bool> evaluate_l06_setup_path_query_appended(const LiteTranscript& transcript) {
    // Row 120: binding 1 (native QUIC) only; a missing Path is row 124's matter.
    if (transcript.binding != LiteBinding::NativeQuic) return std::nullopt;
    const auto paths = client_paths(transcript);
    if (!paths || paths->empty()) return std::nullopt;
    // An exact byte match: Task 9 passes the publisher a path and a query of unreserved characters only, so no
    // percent-encoding or normalization can make a conforming value differ.
    const auto expected = l06_expected_client_path(transcript.session_url_path, transcript.session_url_query);
    for (const auto& value : *paths) {
        if (!std::equal(value.begin(), value.end(), expected.begin(), expected.end(),
                        [](std::byte b, char c) { return b == static_cast<std::byte>(c); }))
            return false;
    }
    return true;
}

std::optional<bool> evaluate_l06_setup_path_absent_on_uri_binding(const LiteTranscript& transcript) {
    // Row 125: binding 2 (WebTransport) only; a Path parameter (any value, empty included) is a Fail.
    if (transcript.binding != LiteBinding::WebTransport) return std::nullopt;
    const auto paths = client_paths(transcript);
    if (!paths) return std::nullopt;
    return paths->empty();
}

}  // namespace moq::interop::scenarios
