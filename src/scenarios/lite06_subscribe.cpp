#include "moq/interop/scenarios/lite06_subscribe.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/session/lite_session.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/group.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::scenarios {
namespace {

namespace l06 = wire::moqlite06;
using lite06::allowance_step;
using lite06::proved_stimulus;
using lite06::step_labelled;
using session::LiteStreamRecord;

void require_fixture(std::string_view broadcast_path, std::string_view track_name) {
    if (l06_path_segments(broadcast_path).empty())
        throw std::invalid_argument("a moq-lite-06 subscribe probe needs the track fixture's broadcast path");
    if (l06_path_segments(track_name).empty())
        throw std::invalid_argument("a moq-lite-06 subscribe probe needs the track fixture's track name");
}

void require_positive(std::chrono::milliseconds value, const char* what) {
    if (value.count() <= 0)
        throw std::invalid_argument(std::string("a moq-lite-06 subscribe probe needs a positive ") + what);
}

// The skeleton: id, deadline (checked against `needed`), zero observation window, the fixture.
LiteProbeDefinition fixture_probe(std::string_view id, std::chrono::milliseconds deadline,
                                  std::chrono::milliseconds window, std::chrono::milliseconds needed,
                                  std::string_view broadcast_path, std::string_view track_name) {
    require_fixture(broadcast_path, track_name);
    if (deadline <= needed)
        throw std::invalid_argument("a moq-lite-06 subscribe probe needs a deadline beyond its windows");
    auto definition = lite06::allowance_probe(id, deadline, window);
    definition.requires_track = true;
    definition.broadcast_path = std::string(broadcast_path);
    definition.track_name = std::string(track_name);
    return definition;
}

// The runner's (only) Announce stream has its answer: ANNOUNCE_OK and the Active Count messages after it, or the
// publisher ended the stream. Never opened by a decode issue (an issue stops decoding; the gate then expires).
bool announce_answered(const session::LiteSession& session) {
    for (const auto* record : session::runner_streams(session)) {
        if (record->kind != session::LiteStreamKind::Announce || !record->bidirectional) continue;
        if (record->fin_seen || record->reset_seen) return true;
        const auto messages = session::peer_messages(*record);
        if (messages.empty()) return false;
        const auto* ok = std::get_if<l06::AnnounceOk>(&messages.front()->message);
        if (!ok) return false;
        return messages.size() - 1 >= ok->active_count;
    }
    return false;
}

// The first decoded message of a peer stream when it is a GROUP.
const l06::GroupHeader* group_header_of(const LiteStreamRecord& record) {
    const auto messages = session::peer_messages(record);
    if (messages.empty()) return nullptr;
    return std::get_if<l06::GroupHeader>(&messages.front()->message);
}

std::size_t frames_of(const LiteStreamRecord& record) {
    std::size_t count = 0;
    for (const auto* message : session::peer_messages(record))
        if (std::holds_alternative<l06::Frame>(message->message)) ++count;
    return count;
}

// The highest Group Sequence decoded so far on Group streams naming `subscribe_id`.
std::optional<std::uint64_t> highest_group(const session::LiteSession& session, std::uint64_t subscribe_id) {
    std::optional<std::uint64_t> highest;
    for (const auto* record : session::peer_streams(session)) {
        if (record->kind != session::LiteStreamKind::Group) continue;
        const auto* header = group_header_of(*record);
        if (!header || header->subscribe_id != subscribe_id) continue;
        if (!highest || header->group_sequence > *highest) highest = header->group_sequence;
    }
    return highest;
}

// The runner stream of step `index` ended by the publisher (FIN or RESET_STREAM).
bool step_stream_ended(const session::LiteSession& session, const LiteProbeContext& context, std::size_t index) {
    const auto stream = context.stream_of(index);
    if (!stream) return false;
    const auto* record = session.find(*stream);
    return record && (record->fin_seen || record->reset_seen);
}

bool elapsed(const LiteProbeContext& context, std::uint64_t since_ns, std::chrono::milliseconds allowance) {
    return context.now_ns >= since_ns &&
           context.now_ns - since_ns >= static_cast<std::uint64_t>(
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(allowance).count());
}

}  // namespace

std::vector<std::byte> l06_announce_all_bytes() { return l06_announce_request_bytes(""); }

// ANNOUNCE_REQUEST "" then the Wait gated on its answer.
void l06_add_announce_exchange(LiteProbeDefinition& definition, std::chrono::milliseconds answer_allowance) {
    definition.steps.push_back(lite_open_bidi(l06_announce_all_bytes(), false, std::string(kL06SubAnnounceLabel)));
    auto announced = lite_wait(std::chrono::milliseconds{0}, std::string(kL06SubAnnouncedLabel));
    announced.gate = announce_answered;
    announced.gate_deadline = answer_allowance;
    definition.steps.push_back(std::move(announced));
}

std::string l06_uncovered_path(std::string_view broadcast_path) {
    return l06_disjoint_prefix(broadcast_path) + "/l1d-unserved";
}

std::string l06_unknown_track(std::string_view track_name) { return std::string(track_name) + "-l1d-unknown"; }

l06::Subscribe l06_subscribe(std::uint64_t subscribe_id, std::string_view broadcast_path, std::string_view track_name,
                             std::uint64_t group_start, std::uint64_t group_end, std::uint64_t frame_start,
                             std::uint64_t frame_end) {
    l06::Subscribe subscribe;
    subscribe.subscribe_id = subscribe_id;
    subscribe.broadcast_path = std::string(broadcast_path);
    subscribe.track_name = std::string(track_name);
    subscribe.range.subscriber_priority = 0;
    subscribe.range.subscriber_max_age_ms = kL06LargeMaxAgeMs;
    subscribe.range.group_start = group_start;
    subscribe.range.group_end = group_end;
    subscribe.range.frame_start = frame_start;
    subscribe.range.frame_end = frame_end;
    return subscribe;
}

std::vector<std::byte> l06_subscribe_bytes(const l06::Subscribe& subscribe) {
    return lite_subscribe_stream_bytes(subscribe);
}

l06::Subscribe l06_learning_subscribe(std::string_view broadcast_path, std::string_view track_name) {
    auto subscribe = l06_subscribe(kL06LearnSubscribeId, broadcast_path, track_name);
    subscribe.range.subscriber_max_age_ms = kL06LearnMaxAgeMs;
    return subscribe;
}

std::vector<std::byte> l06_learning_subscribe_bytes(std::string_view broadcast_path, std::string_view track_name) {
    return l06_subscribe_bytes(l06_learning_subscribe(broadcast_path, track_name));
}

std::vector<std::byte> l06_invalid_bounds_subscribe_bytes(std::string_view broadcast_path,
                                                          std::string_view track_name) {
    const auto subscribe = l06_subscribe(kL06InvalidSubscribeId, broadcast_path, track_name);
    wire::ByteWriter body(std::size_t{1} << 20);
    bool ok = l06::write_varint(subscribe.subscribe_id, body) && l06::write_string(subscribe.broadcast_path, body) &&
              l06::write_string(subscribe.track_name, body) &&
              body.append_byte(static_cast<std::byte>(subscribe.range.subscriber_priority)) &&
              l06::write_varint(subscribe.range.subscriber_max_age_ms, body) && l06::write_varint(0, body) &&
              l06::write_varint(0, body) &&  // Group End 0: unbounded
              l06::write_varint(0, body) &&  // Frame Start 0
              l06::write_varint(kL06InvalidFrameEnd, body);  // a non-zero Frame End without a Group End
    wire::ByteWriter out(std::size_t{1} << 21);
    ok = ok && l06::write_stream_type(static_cast<std::uint64_t>(l06::BidiStreamType::Subscribe), out) &&
         l06::write_framed_message(body.bytes(), out);
    if (!ok) return {};
    return {out.bytes().begin(), out.bytes().end()};
}

std::optional<l06::Subscribe> l06_decode_subscribe_stimulus(std::span<const std::byte> bytes) {
    wire::Cursor cursor(bytes);
    const auto type = l06::read_stream_type(cursor);
    const auto* value = std::get_if<std::uint64_t>(&type);
    if (!value || *value != static_cast<std::uint64_t>(l06::BidiStreamType::Subscribe)) return std::nullopt;
    const auto subscribe = l06::decode_subscribe(cursor);
    const auto* message = std::get_if<l06::Subscribe>(&subscribe);
    if (!message || cursor.remaining() != 0) return std::nullopt;
    return *message;
}

LiteProbeDefinition l06_subscribe_latest_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                               std::string_view track_name, std::chrono::milliseconds window,
                                               std::chrono::milliseconds answer_allowance) {
    require_positive(answer_allowance, "answer allowance");
    auto definition = fixture_probe(kL06SubscribeLatest, deadline, window, answer_allowance + window, broadcast_path,
                                    track_name);
    l06_add_announce_exchange(definition, answer_allowance);
    definition.steps.push_back(
        lite_open_bidi(l06_subscribe_bytes(l06_subscribe(kL06LatestSubscribeId, broadcast_path, track_name)), false,
                       std::string(kL06SubscribeLatestLabel)));
    definition.steps.push_back(allowance_step(window));
    return definition;
}

LiteProbeDefinition l06_subscribe_refused_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                                std::string_view track_name, std::chrono::milliseconds allowance,
                                                std::chrono::milliseconds answer_allowance) {
    require_positive(answer_allowance, "answer allowance");
    auto definition = fixture_probe(kL06SubscribeRefused, deadline, allowance, answer_allowance + allowance,
                                    broadcast_path, track_name);
    l06_add_announce_exchange(definition, answer_allowance);
    // Ungated and consecutive: both are pending together and share the allowance.
    definition.steps.push_back(lite_open_bidi(
        l06_subscribe_bytes(l06_subscribe(kL06UncoveredSubscribeId, l06_uncovered_path(broadcast_path), track_name)),
        false, std::string(kL06SubscribeUncoveredLabel)));
    definition.steps.push_back(lite_open_bidi(
        l06_subscribe_bytes(l06_subscribe(kL06UnknownTrackSubscribeId, broadcast_path, l06_unknown_track(track_name))),
        false, std::string(kL06SubscribeUnknownTrackLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_subscribe_invalid_frame_bounds_probe(std::chrono::milliseconds deadline,
                                                             std::string_view broadcast_path,
                                                             std::string_view track_name,
                                                             std::chrono::milliseconds allowance) {
    auto definition = fixture_probe(kL06SubscribeInvalidFrameBounds, deadline, allowance, allowance, broadcast_path,
                                    track_name);
    definition.steps.push_back(lite_open_bidi(l06_invalid_bounds_subscribe_bytes(broadcast_path, track_name), false,
                                              std::string(kL06SubscribeInvalidLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_subscribe_group_floor_probe(std::chrono::milliseconds deadline,
                                                    std::string_view broadcast_path, std::string_view track_name,
                                                    std::chrono::milliseconds window,
                                                    std::chrono::milliseconds learn_allowance) {
    require_positive(learn_allowance, "learning allowance");
    auto definition = fixture_probe(kL06SubscribeGroupFloor, deadline, window, learn_allowance + window,
                                    broadcast_path, track_name);
    definition.steps.push_back(
        lite_open_bidi(l06_learning_subscribe_bytes(broadcast_path, track_name), false,
                       std::string(kL06SubscribeLearnLabel)));
    definition.next_steps = [path = std::string(broadcast_path), track = std::string(track_name), window,
                             learn_allowance](const session::LiteSession& session,
                                              LiteProbeContext& context) -> std::vector<LiteStep> {
        if (const auto latest = highest_group(session, kL06LearnSubscribeId)) {
            context.finished = true;
            context.values["latest"] = *latest;
            std::vector<LiteStep> steps;
            // Opened at once (ungated, consecutive) and observed together for the window.
            steps.push_back(lite_open_bidi(
                l06_subscribe_bytes(l06_subscribe(kL06FloorAtLatestSubscribeId, path, track, *latest)), false,
                std::string(kL06FloorAtLatestLabel)));
            steps.push_back(lite_open_bidi(
                l06_subscribe_bytes(l06_subscribe(kL06FloorAboveSubscribeId, path, track, *latest + 2)), false,
                std::string(kL06FloorAboveLabel)));
            steps.push_back(allowance_step(window));
            return steps;
        }
        // No GROUP yet: give up once the learning stream ended, the session closed or the allowance passed.
        if (step_stream_ended(session, context, 0) || session.peer_close() ||
            elapsed(context, context.established_ns, learn_allowance))
            context.finished = true;
        return {};
    };
    return definition;
}

LiteProbeDefinition l06_subscribe_abutting_frame_start_probe(std::chrono::milliseconds deadline,
                                                             std::string_view broadcast_path,
                                                             std::string_view track_name,
                                                             std::chrono::milliseconds window,
                                                             std::chrono::milliseconds step_allowance) {
    require_positive(step_allowance, "step allowance");
    auto definition = fixture_probe(kL06SubscribeAbuttingFrameStart, deadline, window, step_allowance * 2 + window,
                                    broadcast_path, track_name);
    definition.steps.push_back(
        lite_open_bidi(l06_learning_subscribe_bytes(broadcast_path, track_name), false,
                       std::string(kL06SubscribeLearnLabel)));
    definition.next_steps = [path = std::string(broadcast_path), track = std::string(track_name), window,
                             step_allowance](const session::LiteSession& session,
                                             LiteProbeContext& context) -> std::vector<LiteStep> {
        constexpr std::uint64_t n = kL06AbuttingFrameSplit;
        // A failed stage: no second subscription, but the window is still observed (the default and the first
        // subscriptions stay judged for a non-zero Frame Start).
        const auto give_up = [&]() -> std::vector<LiteStep> {
            context.finished = true;
            return {allowance_step(window)};
        };
        auto& values = context.values;
        if (!values.contains("stage")) {
            const auto g = highest_group(session, kL06LearnSubscribeId);
            if (!g) {
                if (step_stream_ended(session, context, 0) || session.peer_close() ||
                    elapsed(context, context.established_ns, step_allowance))
                    return give_up();
                return {};
            }
            values["stage"] = 1;
            values["G"] = *g;
            values["stage_ns"] = context.now_ns;
            values["first_index"] = context.step_count();
            // Group End and Frame End are absolute + 1 on the wire: frames 0..N-1 of group G.
            return {lite_open_bidi(
                l06_subscribe_bytes(l06_subscribe(kL06AbuttingFirstSubscribeId, path, track, *g, *g + 1, 0, n)),
                false, std::string(kL06AbuttingFirstLabel))};
        }
        const auto g = values["G"];
        bool received = false;
        for (const auto* record : session::peer_streams(session)) {
            if (record->kind != session::LiteStreamKind::Group) continue;
            const auto* header = group_header_of(*record);
            if (!header || header->subscribe_id != kL06AbuttingFirstSubscribeId) continue;
            // Another group, or a partial one: the pattern cannot proceed.
            if (header->group_sequence != g || header->frame_start != 0) return give_up();
            if (frames_of(*record) >= n) {
                received = true;
            } else if (record->fin_seen || record->reset_seen) {
                return give_up();  // group G ended before frame N-1
            }
        }
        if (received) {
            context.finished = true;
            return {lite_open_bidi(
                        l06_subscribe_bytes(l06_subscribe(kL06AbuttingSecondSubscribeId, path, track, g, 0, n, 0)),
                        false, std::string(kL06AbuttingSecondLabel)),
                    allowance_step(window)};
        }
        if (step_stream_ended(session, context, static_cast<std::size_t>(values["first_index"])) ||
            session.peer_close() || elapsed(context, values["stage_ns"], step_allowance))
            return give_up();
        return {};
    };
    return definition;
}

namespace {

enum class Judged { Pass, Fail, Open };  // Open: inconclusive, or the observation was not over

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

// One Group stream the publisher opened, read through the peer accessors only.
struct GroupView {
    const LiteStreamRecord* record{nullptr};
    std::optional<l06::GroupHeader> header;  // the first decoded message, when it is a GROUP
    bool first_not_group{false};             // the first decoded message is something else
    std::size_t frames{0};
    bool protocol_issue{false};              // a PeerProtocol issue on the stream (decoding stopped there)
};

std::vector<GroupView> group_views(const LiteTranscript& transcript) {
    std::vector<GroupView> views;
    for (const auto* record : lite06::peer_streams(transcript)) {
        if (record->kind != session::LiteStreamKind::Group) continue;
        GroupView view;
        view.record = record;
        const auto messages = session::peer_messages(*record);
        if (!messages.empty()) {
            if (const auto* header = std::get_if<l06::GroupHeader>(&messages.front()->message)) {
                view.header = *header;
            } else {
                view.first_not_group = true;
            }
        }
        view.frames = frames_of(*record);
        view.protocol_issue = !session::peer_protocol_issues(*record).empty();
        views.push_back(view);
    }
    return views;
}

std::vector<l06::GroupHeader> headers_for(const std::vector<GroupView>& views, std::uint64_t subscribe_id) {
    std::vector<l06::GroupHeader> out;
    for (const auto& view : views)
        if (view.header && view.header->subscribe_id == subscribe_id) out.push_back(*view.header);
    return out;
}

const l06::SubscribeOk* first_subscribe_ok(const LiteStreamRecord& record) {
    for (const auto* message : session::peer_messages(record))
        if (const auto* ok = std::get_if<l06::SubscribeOk>(&message->message)) return ok;
    return nullptr;
}

bool has_subscribe_end(const LiteStreamRecord& record) {
    for (const auto* message : session::peer_messages(record))
        if (std::holds_alternative<l06::SubscribeEnd>(message->message)) return true;
    return false;
}

Judged combine(const std::vector<Judged>& verdicts) {
    bool open = false;
    for (const auto verdict : verdicts) {
        if (verdict == Judged::Fail) return Judged::Fail;
        if (verdict == Judged::Open) open = true;
    }
    return open || verdicts.empty() ? Judged::Open : Judged::Pass;
}

std::optional<bool> to_verdict(Judged judged) {
    if (judged == Judged::Open) return std::nullopt;
    return judged == Judged::Pass;
}

// --- l06-subscribe-latest ---

// The default subscription's stream once the transcript may be judged (gate, fixture, the exact stimuli).
const LiteStreamRecord* latest_stream(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06SubscribeLatest || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript))
        return nullptr;
    if (!proved_stimulus(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes())) return nullptr;
    return stimulus_stream(
        transcript, kL06SubscribeLatestLabel,
        l06_subscribe_bytes(l06_subscribe(kL06LatestSubscribeId, transcript.broadcast_path, transcript.track_name)));
}

// True when SUBSCRIBE_DROP ranges (inclusive) cover every group in [from, to].
bool dropped(const std::vector<l06::SubscribeDrop>& drops, std::uint64_t from, std::uint64_t to) {
    auto cursor = from;
    while (cursor <= to) {
        bool advanced = false;
        for (const auto& drop : drops) {
            if (drop.group_start <= cursor && cursor <= drop.group_end) {
                if (drop.group_end >= to) return true;
                cursor = drop.group_end + 1;
                advanced = true;
            }
        }
        if (!advanced) return false;
    }
    return true;
}

// --- l06-subscribe-refused ---

// Case (a)'s path is shown uncovered: the announce exchange answered (ANNOUNCE_OK first), nothing unreadable on
// the announce stream (no Inconclusive or PeerProtocol issue), and no decoded ANNOUNCE_START route covers it.
bool uncovered_shown(const LiteTranscript& transcript) {
    const auto* record = stimulus_stream(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes());
    if (!record) return false;
    for (const auto* issue : session::peer_issues(*record)) {
        const auto kind = session::classify_issue(issue->code);
        if (kind == session::LiteIssueClass::Inconclusive || kind == session::LiteIssueClass::PeerProtocol)
            return false;
    }
    const auto messages = session::peer_messages(*record);
    if (messages.empty() || !std::holds_alternative<l06::AnnounceOk>(messages.front()->message)) return false;
    const auto path = l06_uncovered_path(transcript.broadcast_path);
    for (const auto* message : messages) {
        const auto* start = std::get_if<l06::AnnounceStart>(&message->message);
        if (start && l06_route_covers("", start->suffix, path)) return false;
    }
    return true;
}

// Row 062 on one Subscribe Stream. `pending_fails`: case (a) with the path shown uncovered.
Judged judge_refusal(const LiteTranscript& transcript, const std::vector<GroupView>& views, std::string_view label,
                     const std::vector<std::byte>& expected, std::uint64_t subscribe_id, bool pending_fails) {
    const auto* record = stimulus_stream(transcript, label, expected);
    if (!record) return Judged::Open;  // the SUBSCRIBE never (provably) reached the publisher
    // Served instead of refused: for (a) a Fail; for (b) the track may exist after all (not a refusal case).
    if (first_subscribe_ok(*record) || !headers_for(views, subscribe_id).empty())
        return pending_fails ? Judged::Fail : Judged::Open;
    if (!session::peer_protocol_issues(*record).empty()) return Judged::Open;  // the answer is unreadable
    // The reset is the refusal (any code; the code space is row 027). A SUBSCRIBE_END before it is information
    // only: it is not "instead of" the reset, and a RESET_STREAM may discard END bytes still in flight, so
    // judging it would make the verdict depend on timing (draft 1043-1045).
    if (record->reset_seen) return Judged::Pass;
    if (record->fin_seen) return Judged::Fail;       // FIN (after a SUBSCRIBE_END or not) instead of the reset
    if (transcript.peer_close) return Judged::Fail;  // a session close instead of the stream reset
    if (lite06::allowance_elapsed(transcript)) {
        // A SUBSCRIBE_END with no reset by the end of the allowance is an answer instead of the refusal, in both
        // cases. For (b) this holds although draft 5.1.2 allows END without SUBSCRIBE_OK: that covers a track
        // that existed and ended with no matching group, not "no such track", which must be refused by a reset.
        if (has_subscribe_end(*record)) return Judged::Fail;
        // Still pending: (a) is not reset within the allowance (time-bounded); (b) may still be resolving.
        return pending_fails ? Judged::Fail : Judged::Open;
    }
    return Judged::Open;
}

// --- l06-subscribe-group-floor ---

struct FloorSubscription {
    std::uint64_t subscribe_id{0};
    std::uint64_t floor{0};
    const LiteStreamRecord* record{nullptr};
};

// The floored subscriptions the continuation sent, each with its exact bytes checked against the fixture;
// nullopt when the transcript may not be judged.
std::optional<std::vector<FloorSubscription>> floor_subscriptions(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06SubscribeGroupFloor || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript))
        return std::nullopt;
    const auto& path = transcript.broadcast_path;
    const auto& track = transcript.track_name;
    if (!stimulus_stream(transcript, kL06SubscribeLearnLabel,
                         l06_learning_subscribe_bytes(path, track)))
        return std::nullopt;
    std::vector<FloorSubscription> out;
    for (const auto& [label, id] : {std::pair{kL06FloorAtLatestLabel, kL06FloorAtLatestSubscribeId},
                                    std::pair{kL06FloorAboveLabel, kL06FloorAboveSubscribeId}}) {
        const auto* step = step_labelled(transcript, label);
        if (!step) continue;  // learning failed: no floored subscription was sent
        const auto subscribe = l06_decode_subscribe_stimulus(step->bytes);
        if (!subscribe) return std::nullopt;
        const auto floor = subscribe->range.group_start;
        const auto* record = stimulus_stream(transcript, label,
                                             l06_subscribe_bytes(l06_subscribe(id, path, track, floor)));
        if (!record) return std::nullopt;  // not the stimulus this fixture implies
        out.push_back({id, floor, record});
    }
    return out;
}

// A group value against the raw floor F >= 1: at or above F passes; exactly F-1 is compliant only under the
// offset reading of draft 2056-2058 (the reading conflict: Open); below F-1 is wrong under both readings.
Judged against_floor(std::uint64_t value, std::uint64_t floor) {
    if (value >= floor) return Judged::Pass;
    if (value == floor - 1) return Judged::Open;
    return Judged::Fail;
}

// Combines the floored subscriptions: any Fail; else any reading conflict -> NotRun; else any Pass; a
// subscription with nothing to judge (F = 0, no group or no SUBSCRIBE_OK in the window) is left out.
std::optional<bool> combine_floors(const std::vector<std::optional<Judged>>& judged) {
    bool pass = false;
    bool conflict = false;
    for (const auto& one : judged) {
        if (!one) continue;
        if (*one == Judged::Fail) return false;
        if (*one == Judged::Open) conflict = true;
        if (*one == Judged::Pass) pass = true;
    }
    if (conflict || !pass) return std::nullopt;
    return true;
}

}  // namespace

std::optional<bool> evaluate_l06_group_starts_with_group(const LiteTranscript& transcript) {
    if (!latest_stream(transcript)) return std::nullopt;
    bool judged = false;
    for (const auto& view : group_views(transcript)) {
        if (view.first_not_group) return false;
        if (view.header) {
            judged = true;
            continue;
        }
        // No GROUP decoded and the bytes broke (a FRAME first, garbage, a FIN without a GROUP): a Fail.
        if (view.protocol_issue) return false;
        // Reset before a GROUP decoded (either side MAY reset a Group stream), or still incomplete: skipped.
    }
    // No Group stream in the window: NotRun, never a Pass.
    return judged ? std::optional<bool>{true} : std::nullopt;
}

std::optional<bool> evaluate_l06_group_unique_sequence(const LiteTranscript& transcript) {
    if (!latest_stream(transcript)) return std::nullopt;
    std::set<std::uint64_t> seen;
    std::size_t decoded = 0;
    for (const auto& header : headers_for(group_views(transcript), kL06LatestSubscribeId)) {
        if (!seen.insert(header.group_sequence).second) return false;
        ++decoded;
    }
    // A violation needs two Group streams: fewer decoded is NotRun.
    if (decoded < 2) return std::nullopt;
    return true;
}

std::optional<bool> evaluate_l06_group_sequence_increments(const LiteTranscript& transcript) {
    const auto* record = latest_stream(transcript);
    if (!record) return std::nullopt;
    const auto views = group_views(transcript);
    std::set<std::uint64_t> sequences;
    for (const auto& header : headers_for(views, kL06LatestSubscribeId)) sequences.insert(header.group_sequence);
    if (sequences.size() < 2) return std::nullopt;
    std::vector<l06::SubscribeDrop> drops;
    for (const auto* message : session::peer_messages(*record))
        if (const auto* drop = std::get_if<l06::SubscribeDrop>(&message->message)) drops.push_back(*drop);
    // A Group stream whose GROUP never decoded (reset first, or broken) may be the missing group; so may a
    // SUBSCRIBE_DROP hidden behind an unreadable subscribe stream.
    bool unknown = !session::peer_protocol_issues(*record).empty();
    for (const auto& view : views)
        if (!view.header) unknown = true;
    bool gap = false;
    for (auto it = sequences.begin(), next = std::next(it); next != sequences.end(); ++it, ++next) {
        if (*next == *it + 1) continue;
        if (!dropped(drops, *it + 1, *next - 1)) gap = true;
    }
    if (!gap) return true;
    // An unaccounted gap: a Fail of this SHOULD (inferred from delivery), unless a stream left it open.
    if (unknown) return std::nullopt;
    return false;
}

std::optional<bool> evaluate_l06_subscribe_refused_reset(const LiteTranscript& transcript) {
    // A session close instead of the reset is a Fail, so the peer's close is part of the observation.
    if (transcript.scenario_id != kL06SubscribeRefused || !judgeable(transcript) || transcript.runner_closed ||
        !fixture_present(transcript) || !runner_setup_proved(transcript))
        return std::nullopt;
    const auto& path = transcript.broadcast_path;
    const auto& track = transcript.track_name;
    const auto uncovered_bytes =
        l06_subscribe_bytes(l06_subscribe(kL06UncoveredSubscribeId, l06_uncovered_path(path), track));
    const auto unknown_bytes =
        l06_subscribe_bytes(l06_subscribe(kL06UnknownTrackSubscribeId, path, l06_unknown_track(track)));
    // Both recorded stimuli must be the ones this fixture implies before either case is attributed.
    for (const auto& [label, bytes] : {std::pair{kL06SubscribeUncoveredLabel, &uncovered_bytes},
                                       std::pair{kL06SubscribeUnknownTrackLabel, &unknown_bytes}}) {
        const auto* step = step_labelled(transcript, label);
        if (!step || step->bytes != *bytes) return std::nullopt;
    }
    const auto views = group_views(transcript);
    const auto uncovered = judge_refusal(transcript, views, kL06SubscribeUncoveredLabel, uncovered_bytes,
                                         kL06UncoveredSubscribeId, uncovered_shown(transcript));
    const auto unknown = judge_refusal(transcript, views, kL06SubscribeUnknownTrackLabel, unknown_bytes,
                                       kL06UnknownTrackSubscribeId, false);
    return to_verdict(combine({uncovered, unknown}));
}

std::optional<bool> evaluate_l06_subscribe_invalid_frame_bounds_reset(const LiteTranscript& transcript) {
    // A session close instead of the reset is a Fail, so the peer's close is part of the observation.
    if (transcript.scenario_id != kL06SubscribeInvalidFrameBounds || !judgeable(transcript) ||
        transcript.runner_closed || !fixture_present(transcript) || !runner_setup_proved(transcript))
        return std::nullopt;
    const auto* record =
        stimulus_stream(transcript, kL06SubscribeInvalidLabel,
                        l06_invalid_bounds_subscribe_bytes(transcript.broadcast_path, transcript.track_name));
    if (!record) return std::nullopt;
    // Accepted: a SUBSCRIBE_OK or a Group stream for it, at any time in the allowance (also after a reset).
    if (first_subscribe_ok(*record) || !headers_for(group_views(transcript), kL06InvalidSubscribeId).empty())
        return false;
    if (!session::peer_protocol_issues(*record).empty()) return std::nullopt;  // the answer is unreadable
    // The row's disqualifiers are SUBSCRIBE_OK and Group streams only: a SUBSCRIBE_END before the reset is
    // information (a RESET_STREAM may discard it in flight). Without a reset it is no substitute (below).
    if (record->reset_seen) {
        // On WebTransport a session close resets every stream with an HTTP error that is no application code: a reset
        // without a code beside a session close is that teardown, not the reaction the row asks for.
        if (!record->reset_code && transcript.peer_close && transcript.binding == LiteBinding::WebTransport)
            return false;
        return true;  // any stream code
    }
    if (record->fin_seen) return false;            // FIN (after a SUBSCRIBE_END or not) instead of the reset
    if (transcript.peer_close) return false;       // a session close instead of the stream reset
    if (lite06::allowance_elapsed(transcript)) return false;  // no reaction within the allowance (time-bounded)
    return std::nullopt;
}

std::optional<bool> evaluate_l06_subscribe_no_group_below_floor(const LiteTranscript& transcript) {
    const auto subscriptions = floor_subscriptions(transcript);
    if (!subscriptions) return std::nullopt;
    const auto views = group_views(transcript);
    std::vector<std::optional<Judged>> judged;
    for (const auto& subscription : *subscriptions) {
        if (subscription.floor == 0) continue;  // a floor of 0 is no floor
        const auto headers = headers_for(views, subscription.subscribe_id);
        if (headers.empty()) continue;  // no decoded Group stream in the window
        std::vector<Judged> groups;
        for (const auto& header : headers) groups.push_back(against_floor(header.group_sequence, subscription.floor));
        // Fail beats the reading conflict, which beats Pass (combine treats Open as inconclusive).
        judged.push_back(combine(groups));
    }
    return combine_floors(judged);
}

std::optional<bool> evaluate_l06_subscribe_ok_group_at_floor(const LiteTranscript& transcript) {
    const auto subscriptions = floor_subscriptions(transcript);
    if (!subscriptions) return std::nullopt;
    std::vector<std::optional<Judged>> judged;
    for (const auto& subscription : *subscriptions) {
        if (subscription.floor == 0) continue;
        const auto* ok = first_subscribe_ok(*subscription.record);
        if (!ok) continue;  // withheld, refused, or not in the window
        judged.push_back(against_floor(ok->group, subscription.floor));
    }
    return combine_floors(judged);
}

std::optional<bool> evaluate_l06_subscribe_resolved_start(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06SubscribeAbuttingFrameStart || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript))
        return std::nullopt;
    const auto& path = transcript.broadcast_path;
    const auto& track = transcript.track_name;
    if (!stimulus_stream(transcript, kL06SubscribeLearnLabel,
                         l06_learning_subscribe_bytes(path, track)))
        return std::nullopt;
    // The first subscription, when sent: its G and N, its exact bytes checked against the fixture.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> first;  // (G, N)
    if (const auto* step = step_labelled(transcript, kL06AbuttingFirstLabel)) {
        const auto subscribe = l06_decode_subscribe_stimulus(step->bytes);
        if (!subscribe || subscribe->range.group_end == 0) return std::nullopt;
        const auto g = subscribe->range.group_start;
        const auto n = subscribe->range.frame_end;
        if (!stimulus_stream(transcript, kL06AbuttingFirstLabel,
                             l06_subscribe_bytes(l06_subscribe(kL06AbuttingFirstSubscribeId, path, track, g, g + 1, 0,
                                                               n))))
            return std::nullopt;
        first = {g, n};
    }
    const auto views = group_views(transcript);
    // The default and the first subscriptions asked for Frame Start 0: a partial group there is a Position they
    // did not choose (draft 3.6), whenever it arrives in the window. The default subscription is the rationale's
    // "a separate default subscription must never receive a non-zero Frame Start"; the first subscription
    // (Group Start G, Frame Start 0) is an extension of the same rule beyond the rationale's wording, recorded as
    // such for the evidence: it cannot false-Fail, since a request for frame 0 resolves to frame 0 of G or of a
    // later group, never to a non-zero Frame Start.
    bool fail = false;
    for (const auto& view : views) {
        if (!view.header || view.header->frame_start == 0) continue;
        if (view.header->subscribe_id == kL06LearnSubscribeId) fail = true;
        if (view.header->subscribe_id == kL06AbuttingFirstSubscribeId && first) fail = true;
    }
    const auto inconclusive = [&]() -> std::optional<bool> {
        if (fail) return false;
        return std::nullopt;
    };
    const auto* second_step = step_labelled(transcript, kL06AbuttingSecondLabel);
    if (!second_step || !first) return inconclusive();  // the pattern could not be set up
    // G = 0 edge: Group Start 0 also means "no floor", but draft 3.6 lets Frame Start qualify group 0 ("group 0
    // included"), so (0, N) is still the requested Position; the reading conflict (G-1, N) needs G >= 1.
    const auto [g, n] = *first;
    if (!stimulus_stream(transcript, kL06AbuttingSecondLabel,
                         l06_subscribe_bytes(l06_subscribe(kL06AbuttingSecondSubscribeId, path, track, g, 0, n, 0))))
        return std::nullopt;
    // The precondition (frames 0..N-1 of group G received on the first subscription), from the peer's data.
    bool received = false;
    for (const auto& view : views) {
        if (view.header && view.header->subscribe_id == kL06AbuttingFirstSubscribeId &&
            view.header->group_sequence == g && view.header->frame_start == 0 && view.frames >= n)
            received = true;
    }
    if (!received) return inconclusive();
    const auto headers = headers_for(views, kL06AbuttingSecondSubscribeId);
    if (headers.empty()) return inconclusive();  // no Group stream for the second subscription in the window
    std::uint64_t lowest = headers.front().group_sequence;
    for (const auto& header : headers) lowest = std::min(lowest, header.group_sequence);
    bool conflict = false;
    for (const auto& header : headers) {
        const auto sequence = header.group_sequence;
        if (sequence != lowest) {
            // Any later group of the subscription is delivered whole.
            if (header.frame_start != 0) fail = true;
            continue;
        }
        // The resolved start: exactly (G, N), or a later group at frame 0.
        if (sequence == g && header.frame_start == n) continue;
        if (sequence > g && header.frame_start == 0) continue;
        // (G-1, N) is compliant only under the offset reading of Group Start (draft 2056-2058).
        if (g >= 1 && sequence == g - 1 && header.frame_start == n) {
            conflict = true;
            continue;
        }
        fail = true;
    }
    if (fail) return false;
    if (conflict) return std::nullopt;
    return true;
}

}  // namespace moq::interop::scenarios
