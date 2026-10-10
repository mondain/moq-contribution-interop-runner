#include "moq/interop/scenarios/lite06_fetch.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/scenarios/lite06_track.h"
#include "moq/interop/session/lite_session.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::scenarios {
namespace {

namespace l06 = wire::moqlite06;
using lite06::allowance_step;
using lite06::proved_stimulus;
using session::LiteStreamRecord;

void require_fixture(std::string_view broadcast_path, std::string_view track_name) {
    if (l06_path_segments(broadcast_path).empty())
        throw std::invalid_argument("a moq-lite-06 fetch probe needs the track fixture's broadcast path");
    if (l06_path_segments(track_name).empty())
        throw std::invalid_argument("a moq-lite-06 fetch probe needs the track fixture's track name");
}

bool fixture_present(const LiteTranscript& transcript) {
    return !l06_path_segments(transcript.broadcast_path).empty() && !l06_path_segments(transcript.track_name).empty();
}

// The skeleton shared by both probes, with the announce exchange and the Track Stream lookup in front.
LiteProbeDefinition fetch_probe(std::string_view id, std::chrono::milliseconds deadline,
                                std::chrono::milliseconds allowance, std::chrono::milliseconds needed,
                                std::chrono::milliseconds answer_allowance, std::string_view broadcast_path,
                                std::string_view track_name) {
    require_fixture(broadcast_path, track_name);
    if (answer_allowance.count() <= 0)
        throw std::invalid_argument("a moq-lite-06 fetch probe needs a positive answer allowance");
    if (deadline <= needed)
        throw std::invalid_argument("a moq-lite-06 fetch probe needs a deadline beyond its windows");
    auto definition = lite06::allowance_probe(id, deadline, allowance);
    definition.requires_track = true;
    definition.broadcast_path = std::string(broadcast_path);
    definition.track_name = std::string(track_name);
    l06_add_announce_exchange(definition, answer_allowance);
    l06_add_track_lookup(definition, broadcast_path, track_name, answer_allowance);
    return definition;
}

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

// A Group stream of the learning subscription that ended cleanly (FIN, no reset, nothing unreadable) holding at
// least kL06FetchMinFrames frames: the reference group the publisher demonstrably held.
bool complete_reference(const LiteStreamRecord& record) {
    return record.fin_seen && !record.reset_seen && session::peer_protocol_issues(record).empty() &&
           frames_of(record) >= kL06FetchMinFrames;
}

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

// The announce exchange and the Track Stream lookup both reached the publisher exactly as built and the lookup
// returned a TRACK_INFO (the runner holds the track's TRACK_INFO before it fetches, draft 5.1.3 and 7.16).
bool prefix_in_order(const LiteTranscript& transcript) {
    if (!proved_stimulus(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes())) return false;
    return l06_track_lookup_info(transcript).has_value();
}

// The Fetch Stream of `label` when its exact FETCH bytes reached the publisher; nullptr otherwise.
const LiteStreamRecord* fetch_stream(const LiteTranscript& transcript, std::string_view label,
                                     const l06::FetchRequest& request) {
    const auto* step = proved_stimulus(transcript, label, l06_fetch_bytes(request));
    if (!step || !step->stream_id) return nullptr;
    const auto* record = lite06::find_stream(transcript, *step->stream_id);
    return record && record->kind == session::LiteStreamKind::Fetch ? record : nullptr;
}

}  // namespace

l06::FetchRequest l06_fetch_request(std::string_view broadcast_path, std::string_view track_name,
                                    std::uint64_t group_sequence, std::uint64_t frame_start,
                                    std::uint64_t frame_end) {
    l06::FetchRequest request;
    request.broadcast_path = std::string(broadcast_path);
    request.track_name = std::string(track_name);
    request.subscriber_priority = 0;
    request.group_sequence = group_sequence;
    request.frame_start = frame_start;
    request.frame_end = frame_end;
    return request;
}

std::vector<std::byte> l06_fetch_bytes(const l06::FetchRequest& request) {
    wire::ByteWriter message(std::size_t{1} << 20);
    if (l06::encode_fetch_request(request, message)) return {};
    wire::ByteWriter out(std::size_t{1} << 20);
    if (!l06::write_stream_type(static_cast<std::uint64_t>(l06::BidiStreamType::Fetch), out)) return {};
    for (const auto byte : message.bytes()) {
        if (!out.append_byte(byte)) return {};
    }
    return {out.bytes().begin(), out.bytes().end()};
}

std::optional<l06::FetchRequest> l06_decode_fetch_stimulus(std::span<const std::byte> bytes) {
    wire::Cursor cursor(bytes);
    const auto type = l06::read_stream_type(cursor);
    const auto* value = std::get_if<std::uint64_t>(&type);
    if (!value || *value != static_cast<std::uint64_t>(l06::BidiStreamType::Fetch)) return std::nullopt;
    const auto fetch = l06::decode_fetch_request(cursor);
    const auto* message = std::get_if<l06::FetchRequest>(&fetch);
    if (!message || cursor.remaining() != 0) return std::nullopt;
    return *message;
}

LiteProbeDefinition l06_fetch_group_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                          std::string_view track_name, std::chrono::milliseconds allowance,
                                          std::chrono::milliseconds learn_allowance,
                                          std::chrono::milliseconds answer_allowance) {
    if (learn_allowance.count() <= 0)
        throw std::invalid_argument("a moq-lite-06 fetch probe needs a positive learning allowance");
    auto definition = fetch_probe(kL06FetchGroup, deadline, allowance, answer_allowance * 2 + learn_allowance + allowance,
                                  answer_allowance, broadcast_path, track_name);
    const auto learn_index = definition.steps.size();
    definition.steps.push_back(lite_open_bidi(l06_learning_subscribe_bytes(broadcast_path, track_name), false,
                                              std::string(kL06FetchLearnLabel)));
    definition.next_steps = [path = std::string(broadcast_path), track = std::string(track_name), allowance,
                             learn_allowance, learn_index](const session::LiteSession& session,
                                                           LiteProbeContext& context) -> std::vector<LiteStep> {
        for (const auto* record : session::peer_streams(session)) {
            if (record->kind != session::LiteStreamKind::Group) continue;
            const auto* header = group_header_of(*record);
            if (!header || header->subscribe_id != kL06FetchLearnSubscribeId || !complete_reference(*record)) continue;
            context.finished = true;
            const auto group = header->group_sequence;
            // Opened at once (ungated, consecutive) and observed together for the allowance.
            std::vector<LiteStep> steps;
            steps.push_back(lite_open_bidi(l06_fetch_bytes(l06_fetch_request(path, track, group, 0, 0)), false,
                                           std::string(kL06FetchWholeLabel)));
            steps.push_back(lite_open_bidi(
                l06_fetch_bytes(l06_fetch_request(path, track, group, 0, kL06FetchLeadingEnd)), false,
                std::string(kL06FetchLeadingLabel)));
            steps.push_back(lite_open_bidi(
                l06_fetch_bytes(l06_fetch_request(path, track, group, kL06FetchTrailingStart, 0)), false,
                std::string(kL06FetchTrailingLabel)));
            steps.push_back(allowance_step(allowance));
            return steps;
        }
        // No complete group yet: give up once the learning stream ended, the session closed or the allowance passed.
        if (step_stream_ended(session, context, learn_index) || session.peer_close() ||
            elapsed(context, context.established_ns, learn_allowance))
            context.finished = true;
        return {};
    };
    return definition;
}

LiteProbeDefinition l06_fetch_unknown_group_probe(std::chrono::milliseconds deadline,
                                                  std::string_view broadcast_path, std::string_view track_name,
                                                  std::chrono::milliseconds allowance,
                                                  std::chrono::milliseconds answer_allowance) {
    auto definition = fetch_probe(kL06FetchUnknownGroup, deadline, allowance, answer_allowance * 2 + allowance,
                                  answer_allowance, broadcast_path, track_name);
    definition.steps.push_back(lite_open_bidi(
        l06_fetch_bytes(l06_fetch_request(broadcast_path, track_name, kL06FetchUnknownGroupSequence, 0, 0)), false,
        std::string(kL06FetchUnknownLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

std::optional<bool> evaluate_l06_fetch_short_run(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06FetchGroup || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript) || !prefix_in_order(transcript))
        return std::nullopt;
    const auto& path = transcript.broadcast_path;
    const auto& track = transcript.track_name;
    if (!proved_stimulus(transcript, kL06FetchLearnLabel, l06_learning_subscribe_bytes(path, track)))
        return std::nullopt;
    // The group the continuation fetched, read from the whole-group stimulus.
    const auto* whole_step = lite06::step_labelled(transcript, kL06FetchWholeLabel);
    if (!whole_step) return std::nullopt;  // no complete group was learned: nothing was fetched
    const auto whole = l06_decode_fetch_stimulus(whole_step->bytes);
    if (!whole) return std::nullopt;
    const auto group = whole->group_sequence;
    const LiteStreamRecord* reference = nullptr;
    for (const auto* record : lite06::peer_streams(transcript)) {
        if (record->kind != session::LiteStreamKind::Group) continue;
        const auto* header = group_header_of(*record);
        if (header && header->subscribe_id == kL06FetchLearnSubscribeId && header->group_sequence == group) {
            reference = record;
            break;
        }
    }
    if (!reference || !complete_reference(*reference)) return std::nullopt;
    const auto held = frames_of(*reference);

    struct Case {
        std::string_view label;
        std::uint64_t start;
        std::uint64_t end;
        std::uint64_t expected;
    };
    const Case cases[] = {{kL06FetchWholeLabel, 0, 0, held},
                          {kL06FetchLeadingLabel, 0, kL06FetchLeadingEnd, kL06FetchLeadingEnd},
                          {kL06FetchTrailingLabel, kL06FetchTrailingStart, 0, held - kL06FetchTrailingStart}};
    bool passed = false;
    for (const auto& one : cases) {
        const auto* record = fetch_stream(transcript, one.label, l06_fetch_request(path, track, group, one.start, one.end));
        if (!record) {
            // A step that never ran is not evidence; one that ran with other bytes is not this stimulus.
            if (lite06::step_labelled(transcript, one.label)) return std::nullopt;
            continue;
        }
        if (!session::peer_protocol_issues(*record).empty()) return std::nullopt;  // the answer is unreadable
        if (record->reset_seen) continue;  // the publisher may have dropped the group: a reset is conforming (7.12)
        if (!record->fin_seen) continue;   // still running when the probe ended: no verdict from this stream
        const auto frames = session::peer_fetch_frames(*record).size();
        if (frames < one.expected) return false;  // a FIN after a shorter run: the truncation the draft forbids
        if (frames == one.expected) passed = true;
    }
    if (!passed) return std::nullopt;
    return true;
}

std::optional<bool> evaluate_l06_fetch_unknown_group_reset(const LiteTranscript& transcript) {
    // The publisher's session close is an observation of this row (a close instead of the stream reset), so plain
    // judgeable(); the stimuli are proven below.
    if (transcript.scenario_id != kL06FetchUnknownGroup || !judgeable(transcript) ||
        !fixture_present(transcript) || !prefix_in_order(transcript))
        return std::nullopt;
    const auto* record = fetch_stream(
        transcript, kL06FetchUnknownLabel,
        l06_fetch_request(transcript.broadcast_path, transcript.track_name, kL06FetchUnknownGroupSequence, 0, 0));
    if (!record) return std::nullopt;
    if (!session::peer_protocol_issues(*record).empty()) return std::nullopt;  // the answer is unreadable
    // The reset is the answer the draft requires (any code: it names none for this case).
    if (record->reset_seen) return true;
    // A FIN, with or without frames, or frames still arriving is a served group the publisher cannot hold, and a
    // session close is not a stream reset.
    if (record->fin_seen || !session::peer_fetch_frames(*record).empty() || transcript.peer_close) return false;
    return std::nullopt;  // no answer inside the window
}

}  // namespace moq::interop::scenarios
