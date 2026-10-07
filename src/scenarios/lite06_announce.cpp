#include "moq/interop/scenarios/lite06_announce.h"

#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/session/lite_session.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/subscribe.h"

namespace moq::interop::scenarios {
namespace {

namespace l06 = wire::moqlite06;
using lite06::allowance_step;
using lite06::proved_stimulus;
using lite06::step_labelled;

void require_fixture(std::string_view value, const char* what) {
    if (l06_path_segments(value).empty())
        throw std::invalid_argument(std::string("a moq-lite-06 announce probe needs the track fixture's ") + what);
}

LiteProbeDefinition fixture_probe(std::string_view id, std::chrono::milliseconds deadline,
                                  std::chrono::milliseconds allowance, std::string_view broadcast_path) {
    require_fixture(broadcast_path, "broadcast path");
    auto definition = lite06::allowance_probe(id, deadline, allowance);
    definition.requires_track = true;
    definition.broadcast_path = std::string(broadcast_path);
    return definition;
}

}  // namespace

std::vector<std::string> l06_path_segments(std::string_view path) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        if (end > start) out.emplace_back(path.substr(start, end - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return out;
}

std::string l06_broadcast_prefix(std::string_view broadcast_path) {
    const auto segments = l06_path_segments(broadcast_path);
    return segments.empty() ? std::string() : segments.front();
}

std::string l06_disjoint_prefix(std::string_view broadcast_path) {
    std::string prefix = "l1d-disjoint";
    const auto first = l06_broadcast_prefix(broadcast_path);
    while (prefix == first) prefix += "-x";
    return prefix;
}

bool l06_route_covers(std::string_view request_prefix, std::string_view suffix, std::string_view broadcast_path) {
    auto route = l06_path_segments(request_prefix);
    for (auto& segment : l06_path_segments(suffix)) route.push_back(std::move(segment));
    const auto path = l06_path_segments(broadcast_path);
    if (route.size() > path.size()) return false;
    for (std::size_t i = 0; i < route.size(); ++i)
        if (route[i] != path[i]) return false;
    return true;
}

std::vector<std::byte> l06_announce_request_bytes(std::string_view prefix) {
    return lite_announce_stream_bytes(l06::AnnounceRequest{std::string(prefix)});
}

std::vector<std::byte> l06_stream_close_subscribe_bytes(std::string_view broadcast_path, std::string_view track_name) {
    l06::Subscribe subscribe;
    subscribe.subscribe_id = kL06StreamCloseSubscribeId;
    subscribe.broadcast_path = std::string(broadcast_path);
    subscribe.track_name = std::string(track_name);
    return lite_subscribe_stream_bytes(subscribe);
}

LiteProbeDefinition l06_announce_prefix_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                              std::chrono::milliseconds allowance) {
    auto definition = fixture_probe(kL06AnnouncePrefix, deadline, allowance, broadcast_path);
    // The first two are ungated and consecutive, so both are open before any answer is awaited.
    definition.steps.push_back(
        lite_open_bidi(l06_announce_request_bytes(""), false, std::string(kL06AnnounceEmptyLabel)));
    definition.steps.push_back(lite_open_bidi(l06_announce_request_bytes(l06_broadcast_prefix(broadcast_path)), false,
                                              std::string(kL06AnnounceBroadcastLabel)));
    definition.steps.push_back(lite_open_bidi(l06_announce_request_bytes(l06_disjoint_prefix(broadcast_path)), false,
                                              std::string(kL06AnnounceDisjointLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_announce_lifecycle_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                                 std::chrono::milliseconds window) {
    auto definition = fixture_probe(kL06AnnounceLifecycle, deadline, window, broadcast_path);
    definition.steps.push_back(lite_open_bidi(l06_announce_request_bytes(l06_broadcast_prefix(broadcast_path)), false,
                                              std::string(kL06AnnounceRequestLabel)));
    definition.steps.push_back(allowance_step(window));
    return definition;
}

namespace {

// The publisher's first answer on the (only) runner stream of `kind`: a decoded message of the peer, or the peer
// ending its send direction.
bool answered(const session::LiteSession& session, session::LiteStreamKind kind) {
    for (const auto* record : session::runner_streams(session)) {
        if (record->kind != kind || !record->bidirectional) continue;
        if (record->fin_seen || record->reset_seen || !session::peer_messages(*record).empty()) return true;
    }
    return false;
}

}  // namespace

LiteProbeDefinition l06_session_stream_close_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                                   std::string_view track_name, std::chrono::milliseconds allowance,
                                                   std::chrono::milliseconds answer_allowance) {
    require_fixture(track_name, "track name");
    if (answer_allowance.count() <= 0)
        throw std::invalid_argument("a moq-lite-06 stream-close probe needs a positive answer allowance");
    // The deadline must cover the answer wait and the close allowance.
    if (deadline <= answer_allowance + allowance)
        throw std::invalid_argument("a moq-lite-06 stream-close probe needs a deadline beyond its allowances");
    auto definition = fixture_probe(kL06SessionStreamClose, deadline, allowance, broadcast_path);
    definition.track_name = std::string(track_name);
    definition.steps.push_back(
        lite_open_bidi(l06_announce_request_bytes(""), false, std::string(kL06AnnounceRequestLabel)));
    definition.steps.push_back(lite_open_bidi(l06_stream_close_subscribe_bytes(broadcast_path, track_name), false,
                                              std::string(kL06SubscribeRequestLabel)));
    // End the transactions mid-flight: only once the publisher answered both (or the gate expired).
    auto answers = lite_wait(std::chrono::milliseconds{0}, std::string(kL06AnswersLabel));
    answers.gate = [](const session::LiteSession& session) {
        return answered(session, session::LiteStreamKind::Announce) &&
               answered(session, session::LiteStreamKind::Subscribe);
    };
    answers.gate_deadline = answer_allowance;
    definition.steps.push_back(std::move(answers));
    definition.steps.push_back(lite_fin(0, std::string(kL06AnnounceFinLabel)));
    definition.steps.push_back(lite_fin(1, std::string(kL06SubscribeFinLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

namespace {

using session::LiteStreamRecord;

// The publisher's response side of one Announce Stream, as the announce rows may judge it.
//
// Decision (a): an ANNOUNCE message with an unknown Type (an Inconclusive-class issue) makes the stream
// inconclusive "from that point on". Messages and issues carry only the index of the stream event that completed
// them, so a message is judged only when its event came STRICTLY before the unknown Type's event; one completed in
// the same event may follow it and is not judged. ANNOUNCE_OK is the exception: the reader pre-reads a Type only
// after ANNOUNCE_OK, so the first message always precedes the unknown Type.
struct AnnounceView {
    const LiteStreamRecord* record{nullptr};
    std::optional<l06::AnnounceOk> ok;  // the first peer message, when it is ANNOUNCE_OK
    bool first_not_ok{false};           // the first peer message decoded is something else
    std::vector<const session::LiteDecoded*> after;  // judged messages after the first one (before the cut)
    bool cut{false};                    // an unknown Type was seen: everything from there on is inconclusive
    bool protocol_issue{false};         // a PeerProtocol-class issue from the peer before the cut
};

AnnounceView view_of(const LiteStreamRecord& record) {
    AnnounceView view;
    view.record = &record;
    std::optional<std::size_t> cut;
    for (const auto* issue : session::peer_issues(record)) {
        if (session::classify_issue(issue->code) != session::LiteIssueClass::Inconclusive) continue;
        if (!cut || issue->stream_event_index < *cut) cut = issue->stream_event_index;
    }
    view.cut = cut.has_value();
    const auto before = [&](std::size_t index) { return !cut || index < *cut; };
    for (const auto* issue : session::peer_protocol_issues(record))
        if (before(issue->stream_event_index)) view.protocol_issue = true;
    const auto messages = session::peer_messages(record);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto* message = messages[i];
        if (i == 0) {
            if (const auto* ok = std::get_if<l06::AnnounceOk>(&message->message)) {
                view.ok = *ok;
            } else {
                view.first_not_ok = true;
            }
            continue;
        }
        if (before(message->stream_event_index)) view.after.push_back(message);
    }
    return view;
}

const l06::RouteMetadata* route_of(const session::LiteDecoded& decoded) {
    if (const auto* start = std::get_if<l06::AnnounceStart>(&decoded.message)) return &start->route;
    if (const auto* update = std::get_if<l06::AnnounceUpdate>(&decoded.message)) return &update->route;
    return nullptr;
}

// One request of l06-announce-prefix.
struct PrefixRequest {
    std::string_view label;
    std::string prefix;
    bool needs_cover;  // the empty prefix and the broadcast's prefix must see a covering route
};

std::vector<PrefixRequest> prefix_requests(std::string_view broadcast_path) {
    return {{kL06AnnounceEmptyLabel, "", true},
            {kL06AnnounceBroadcastLabel, l06_broadcast_prefix(broadcast_path), true},
            {kL06AnnounceDisjointLabel, l06_disjoint_prefix(broadcast_path), false}};
}

bool runner_setup_proved(const LiteTranscript& transcript) {
    return proved_stimulus(transcript, lite06::kRunnerSetupLabel, lite_default_runner_setup()) != nullptr;
}

// The recorded stimulus is the one the transcript's fixture implies (each request step exists with exactly the
// bytes rebuilt from transcript.broadcast_path); otherwise nothing on the transcript can be attributed.
bool prefix_stimuli_match(const LiteTranscript& transcript) {
    if (l06_path_segments(transcript.broadcast_path).empty()) return false;
    for (const auto& request : prefix_requests(transcript.broadcast_path)) {
        const auto* step = step_labelled(transcript, request.label);
        if (!step || step->bytes != l06_announce_request_bytes(request.prefix)) return false;
    }
    return true;
}

// The response stream of a request step whose bytes reached the publisher; nullptr otherwise.
const LiteStreamRecord* response_stream(const LiteTranscript& transcript, std::string_view label,
                                        const std::vector<std::byte>& expected) {
    const auto* step = proved_stimulus(transcript, label, expected);
    if (!step || !step->stream_id) return nullptr;
    return lite06::find_stream(transcript, *step->stream_id);
}

// An incomplete message is still buffered on a stream the publisher has not ended: what it will say is unknown, so
// a Pass over the decoded prefix would hide it.
bool pending_tail(const LiteStreamRecord& record) {
    return record.peer_pending_bytes != 0 && !record.fin_seen && !record.reset_seen;
}

enum class Judged { Pass, Fail, Open };  // Open: inconclusive, or the observation was not over

// Row 139 on one response stream.
Judged judge_answer(const LiteTranscript& transcript, const LiteStreamRecord& record, const PrefixRequest& request) {
    const auto view = view_of(record);
    // The observation of this stream is over: the allowance elapsed, the session closed, or the publisher ended
    // its send direction (a reset or a FIN).
    const bool over = lite06::allowance_elapsed(transcript) || transcript.peer_close.has_value() || record.reset_seen ||
                      record.fin_seen;
    // A decode failure (an ANNOUNCE_START/UPDATE with a Hop Count mismatch or a duplicate non-zero Hop ID, an
    // announcement before ANNOUNCE_OK, a FIN inside a message) is this row's Fail.
    if (view.protocol_issue || view.first_not_ok) return Judged::Fail;
    if (!view.ok) return over ? Judged::Fail : Judged::Open;  // no ANNOUNCE_OK (time-bounded)
    bool open = view.cut;
    // ANNOUNCE_OK exactly once (the reader cannot decode a second one as such; checked for completeness).
    for (const auto* message : view.after)
        if (std::holds_alternative<l06::AnnounceOk>(message->message)) return Judged::Fail;
    // Active Count: the next that many messages are ANNOUNCE_STARTs. An ANNOUNCE_END for an id of the initial set
    // makes this check inconclusive; anything else is a Fail; fewer is a Fail once over (time-bounded).
    std::uint64_t assigned = 0;
    for (std::uint64_t k = 0; k < view.ok->active_count; ++k) {
        if (k >= view.after.size()) {
            if (view.cut || !over) {
                open = true;
                break;
            }
            return Judged::Fail;
        }
        const auto& message = view.after[static_cast<std::size_t>(k)]->message;
        if (std::holds_alternative<l06::AnnounceStart>(message)) {
            ++assigned;
            continue;
        }
        if (const auto* end = std::get_if<l06::AnnounceEnd>(&message); end && end->announce_id < assigned) {
            open = true;
            break;
        }
        return Judged::Fail;
    }
    // Coverage: the empty prefix and the broadcast's prefix must see a route covering the configured broadcast.
    if (request.needs_cover) {
        bool covers = false;
        for (const auto* message : view.after) {
            const auto* start = std::get_if<l06::AnnounceStart>(&message->message);
            if (start && l06_route_covers(request.prefix, start->suffix, transcript.broadcast_path)) covers = true;
        }
        if (!covers) {
            if (view.cut || !over) {
                open = true;
            } else {
                return Judged::Fail;
            }
        }
    }
    // An incomplete message still buffered on a stream the publisher has not ended hides what follows the decoded
    // prefix (for example a second ANNOUNCE_OK read as a Type 2 message that waits for bytes): not a Pass.
    if (pending_tail(record)) open = true;
    return open ? Judged::Open : Judged::Pass;
}

// The first time the publisher ended its send direction of `stream_id` (a FIN or a RESET_STREAM), from the
// transport events (which carry only the peer's direction).
std::optional<std::uint64_t> peer_send_end_ns(const LiteTranscript& transcript, std::uint64_t stream_id) {
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

}  // namespace

std::optional<bool> evaluate_l06_announce_ok_then_starts(const LiteTranscript& transcript) {
    // A session close or a reset instead of the answer is a Fail, so the peer's close is part of the observation:
    // judgeable() plus the proof of each request, not judgeable_with_stimulus().
    if (transcript.scenario_id != kL06AnnouncePrefix || !judgeable(transcript) || transcript.runner_closed)
        return std::nullopt;
    if (!runner_setup_proved(transcript) || !prefix_stimuli_match(transcript)) return std::nullopt;
    bool fail = false;
    bool open = false;
    for (const auto& request : prefix_requests(transcript.broadcast_path)) {
        const auto* step = step_labelled(transcript, request.label);
        if (step->refused == transport::TransportStatus::PeerReset ||
            step->refused == transport::TransportStatus::PeerStopped) {
            fail = true;  // the publisher reset the request instead of answering
            continue;
        }
        const auto* record = response_stream(transcript, request.label, l06_announce_request_bytes(request.prefix));
        if (!record) {
            open = true;  // the request never reached the publisher
            continue;
        }
        switch (judge_answer(transcript, *record, request)) {
            case Judged::Fail: fail = true; break;
            case Judged::Open: open = true; break;
            case Judged::Pass: break;
        }
    }
    if (fail) return false;
    if (open) return std::nullopt;
    return true;
}

std::optional<bool> evaluate_l06_announce_ok_hop_assigned(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06AnnouncePrefix || !judgeable_with_stimulus(transcript) ||
        !prefix_stimuli_match(transcript))
        return std::nullopt;
    bool any = false;
    for (const auto& request : prefix_requests(transcript.broadcast_path)) {
        const auto* record = response_stream(transcript, request.label, l06_announce_request_bytes(request.prefix));
        if (!record) return std::nullopt;
        const auto messages = session::peer_messages(*record);
        if (messages.empty()) continue;
        const auto* ok = std::get_if<l06::AnnounceOk>(&messages.front()->message);
        if (!ok) continue;  // no ANNOUNCE_OK on this stream: row 139's matter
        if (ok->hop_id == 0) return false;  // no identity, or withheld: a Fail of this SHOULD
        any = true;
    }
    // No ANNOUNCE_OK at all: NotRun (row 139 judges the absence).
    return any ? std::optional<bool>{true} : std::nullopt;
}

std::optional<bool> evaluate_l06_announce_hop_list_excludes_own(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06AnnouncePrefix || !judgeable_with_stimulus(transcript) ||
        !prefix_stimuli_match(transcript))
        return std::nullopt;
    bool fail = false;
    bool open = false;
    bool judged = false;
    for (const auto& request : prefix_requests(transcript.broadcast_path)) {
        const auto* record = response_stream(transcript, request.label, l06_announce_request_bytes(request.prefix));
        if (!record) return std::nullopt;
        const auto view = view_of(*record);
        // Hop ID 0 means unknown and repeated 0 entries are legal: the stream is not judged (NotRun).
        if (!view.ok || view.ok->hop_id == 0) continue;
        for (const auto* message : view.after) {
            const auto* route = route_of(*message);
            if (!route) continue;
            judged = true;
            // Draft 7.5 forbids the ANNOUNCE_OK Hop ID as the LAST entry; elsewhere in the list it is not this row.
            if (!route->hop_ids.empty() && route->hop_ids.back() == view.ok->hop_id) fail = true;
        }
        if (view.cut || pending_tail(*record)) open = true;
    }
    if (fail) return false;
    if (open || !judged) return std::nullopt;  // Pass needs a decoded START or UPDATE on a non-zero Hop ID stream
    return true;
}

std::optional<bool> evaluate_l06_announce_retired_id_unused(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06AnnounceLifecycle || !judgeable_with_stimulus(transcript)) return std::nullopt;
    if (l06_path_segments(transcript.broadcast_path).empty()) return std::nullopt;
    const auto* record =
        response_stream(transcript, kL06AnnounceRequestLabel,
                        l06_announce_request_bytes(l06_broadcast_prefix(transcript.broadcast_path)));
    if (!record) return std::nullopt;
    const auto view = view_of(*record);
    if (!view.ok) return std::nullopt;
    // Each ANNOUNCE_START assigns the next id (from 0); an ANNOUNCE_END retires a live one.
    std::uint64_t next = 0;
    std::set<std::uint64_t> retired;
    bool retraction = false;
    for (const auto* message : view.after) {
        if (std::holds_alternative<l06::AnnounceStart>(message->message)) {
            ++next;
        } else if (const auto* end = std::get_if<l06::AnnounceEnd>(&message->message)) {
            if (retired.contains(end->announce_id)) return false;
            // A never-assigned id is recorded as information only (not a keyword rule of this row).
            if (end->announce_id < next) {
                retired.insert(end->announce_id);
                retraction = true;
            }
        } else if (const auto* update = std::get_if<l06::AnnounceUpdate>(&message->message)) {
            if (retired.contains(update->announce_id)) return false;
        }
    }
    // No retraction in the window: NotRun, never a Pass. An unknown Type or a decode failure hides what followed.
    if (!retraction || view.cut || view.protocol_issue || view.first_not_ok || pending_tail(*record))
        return std::nullopt;
    return true;
}

std::optional<bool> evaluate_l06_session_peer_closes_send(const LiteTranscript& transcript) {
    // A session close is observed (it is no substitute, so a Fail): judgeable() plus the proof of each stimulus.
    if (transcript.scenario_id != kL06SessionStreamClose || !judgeable(transcript) || transcript.runner_closed)
        return std::nullopt;
    if (!runner_setup_proved(transcript) || l06_path_segments(transcript.broadcast_path).empty() ||
        l06_path_segments(transcript.track_name).empty())
        return std::nullopt;
    const auto* announce = proved_stimulus(transcript, kL06AnnounceRequestLabel, l06_announce_request_bytes(""));
    const auto* subscribe =
        proved_stimulus(transcript, kL06SubscribeRequestLabel,
                        l06_stream_close_subscribe_bytes(transcript.broadcast_path, transcript.track_name));
    if (!announce || !subscribe) return std::nullopt;
    // The publisher must have answered both before the runner ended them (otherwise: publisher never answers).
    const auto* answers = step_labelled(transcript, kL06AnswersLabel);
    if (!answers || answers->gate_expired || !answers->executed()) return std::nullopt;
    bool fail = false;
    bool open = false;
    bool pass = false;
    for (const auto& [request, fin_label] : {std::pair{announce, kL06AnnounceFinLabel},
                                             std::pair{subscribe, kL06SubscribeFinLabel}}) {
        const auto* fin = proved_stimulus(transcript, fin_label);
        if (!fin || !request->stream_id) continue;  // the runner's FIN never went out: not tested
        const auto end = peer_send_end_ns(transcript, *request->stream_id);
        // Events of a poll are handled before its steps, so an end at the FIN's own time came first.
        if (end && *end <= *fin->executed_at_ns) continue;  // ended before the runner's FIN: cannot show the rule
        if (end) {
            pass = true;  // closed (FIN or RESET_STREAM) after the runner's FIN, within the probe's allowance
        } else if (transcript.peer_close || lite06::allowance_elapsed(transcript)) {
            fail = true;  // not closed within the allowance; a session close is no substitute
        } else {
            open = true;
        }
    }
    if (fail) return false;
    if (open || !pass) return std::nullopt;
    return true;
}

}  // namespace moq::interop::scenarios
