#include "moq/interop/scenarios/lite06_track.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
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
        throw std::invalid_argument("a moq-lite-06 track probe needs the track fixture's broadcast path");
    if (l06_path_segments(track_name).empty())
        throw std::invalid_argument("a moq-lite-06 track probe needs the track fixture's track name");
}

bool fixture_present(const LiteTranscript& transcript) {
    return !l06_path_segments(transcript.broadcast_path).empty() && !l06_path_segments(transcript.track_name).empty();
}

// The Track Stream of `label` when its exact TRACK bytes for the fixture reached the publisher.
const LiteStreamRecord* track_stream(const LiteTranscript& transcript, std::string_view label) {
    const auto* step = proved_stimulus(transcript, label,
                                       l06_track_bytes(transcript.broadcast_path, transcript.track_name));
    if (!step || !step->stream_id) return nullptr;
    const auto* record = lite06::find_stream(transcript, *step->stream_id);
    return record && record->kind == session::LiteStreamKind::Track ? record : nullptr;
}

// The Track Stream a gate looks at: the runner's first, which carries the lookup.
bool track_answered(const session::LiteSession& session) {
    for (const auto* record : session::runner_streams(session)) {
        if (record->kind != session::LiteStreamKind::Track || !record->bidirectional) continue;
        return record->fin_seen || record->reset_seen || !session::peer_track_info(*record).empty();
    }
    return false;
}

// The TRACK_INFO of a stream; nullopt without a reply or with an unreadable stream (a reset, a decode issue, a
// second reply). A reset is the publisher's refusal (draft 5.1.4: the track does not exist) and never a reply.
std::optional<l06::TrackInfo> reply_of(const LiteStreamRecord& record) {
    if (record.reset_seen || !session::peer_protocol_issues(record).empty()) return std::nullopt;
    const auto infos = session::peer_track_info(record);
    if (infos.size() != 1) return std::nullopt;
    return std::get<l06::TrackInfo>(infos.front()->message);
}

// The transcript's two lookups of l06-track-info, once the scenario, the stimuli and the observation are in order.
struct Lookups {
    const LiteStreamRecord* first{nullptr};
    const LiteStreamRecord* second{nullptr};
};

std::optional<Lookups> lookups(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06TrackInfo || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript))
        return std::nullopt;
    if (!proved_stimulus(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes())) return std::nullopt;
    Lookups out;
    out.first = track_stream(transcript, kL06TrackFirstLabel);
    out.second = track_stream(transcript, kL06TrackSecondLabel);
    if (!out.first || !out.second) return std::nullopt;
    return out;
}

}  // namespace

std::vector<std::byte> l06_track_bytes(std::string_view broadcast_path, std::string_view track_name) {
    l06::TrackRequest request;
    request.broadcast_path = std::string(broadcast_path);
    request.track_name = std::string(track_name);
    wire::ByteWriter body(std::size_t{1} << 20);
    if (l06::encode_track_request(request, body)) return {};
    wire::ByteWriter out(std::size_t{1} << 20);
    if (!l06::write_stream_type(static_cast<std::uint64_t>(l06::BidiStreamType::Track), out)) return {};
    // encode_track_request already framed the message: append it after the STREAM_TYPE.
    for (const auto byte : body.bytes()) {
        if (!out.append_byte(byte)) return {};
    }
    return {out.bytes().begin(), out.bytes().end()};
}

void l06_add_track_lookup(LiteProbeDefinition& definition, std::string_view broadcast_path,
                          std::string_view track_name, std::chrono::milliseconds answer_allowance) {
    definition.steps.push_back(
        lite_open_bidi(l06_track_bytes(broadcast_path, track_name), false, std::string(kL06TrackFirstLabel)));
    auto answered = lite_wait(std::chrono::milliseconds{0}, std::string(kL06TrackAnsweredLabel));
    answered.gate = track_answered;
    answered.gate_deadline = answer_allowance;
    definition.steps.push_back(std::move(answered));
}

const LiteStreamRecord* l06_track_lookup_stream(const LiteTranscript& transcript) {
    if (!fixture_present(transcript)) return nullptr;
    return track_stream(transcript, kL06TrackFirstLabel);
}

std::optional<l06::TrackInfo> l06_track_lookup_info(const LiteTranscript& transcript) {
    const auto* record = l06_track_lookup_stream(transcript);
    if (!record) return std::nullopt;
    return reply_of(*record);
}

LiteProbeDefinition l06_track_info_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                         std::string_view track_name, std::chrono::milliseconds allowance,
                                         std::chrono::milliseconds answer_allowance) {
    require_fixture(broadcast_path, track_name);
    if (answer_allowance.count() <= 0)
        throw std::invalid_argument("a moq-lite-06 track probe needs a positive answer allowance");
    if (deadline <= answer_allowance + kL06TrackPause + allowance)
        throw std::invalid_argument("a moq-lite-06 track probe needs a deadline beyond its windows");
    auto definition = lite06::allowance_probe(kL06TrackInfo, deadline, allowance);
    definition.requires_track = true;
    definition.broadcast_path = std::string(broadcast_path);
    definition.track_name = std::string(track_name);
    l06_add_announce_exchange(definition, answer_allowance);
    definition.steps.push_back(
        lite_open_bidi(l06_track_bytes(broadcast_path, track_name), false, std::string(kL06TrackFirstLabel)));
    definition.steps.push_back(lite_wait(kL06TrackPause, std::string(kL06TrackPauseLabel)));
    definition.steps.push_back(
        lite_open_bidi(l06_track_bytes(broadcast_path, track_name), false, std::string(kL06TrackSecondLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

std::optional<bool> evaluate_l06_track_info_immutable(const LiteTranscript& transcript) {
    const auto found = lookups(transcript);
    if (!found) return std::nullopt;
    const auto first = reply_of(*found->first);
    const auto second = reply_of(*found->second);
    if (!first || !second) return std::nullopt;  // fewer than two replies: nothing to compare
    return *first == *second;
}

std::optional<bool> evaluate_l06_track_info_timescale_nonzero(const LiteTranscript& transcript) {
    const auto found = lookups(transcript);
    if (!found) return std::nullopt;
    bool seen = false;
    for (const auto* record : {found->first, found->second}) {
        const auto reply = reply_of(*record);
        if (!reply) continue;
        if (reply->timescale == 0) return false;
        seen = true;
    }
    if (!seen) return std::nullopt;
    return true;
}

}  // namespace moq::interop::scenarios
