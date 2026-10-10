#include "moq/interop/scenarios/lite06_goaway.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/session/lite_session.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/goaway.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::scenarios {
namespace {

namespace l06 = wire::moqlite06;
using lite06::allowance_step;
using lite06::proved_stimulus;
using session::LiteStreamRecord;

std::uint64_t to_ns(std::chrono::milliseconds value) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}

bool elapsed(std::uint64_t now_ns, std::uint64_t since_ns, std::chrono::milliseconds allowance) {
    return now_ns >= since_ns && now_ns - since_ns >= to_ns(allowance);
}

bool fixture_present(const LiteTranscript& transcript) {
    return !l06_path_segments(transcript.broadcast_path).empty() && !l06_path_segments(transcript.track_name).empty();
}

const l06::GroupHeader* group_header_of(const LiteStreamRecord& record) {
    const auto messages = session::peer_messages(record);
    if (messages.empty()) return nullptr;
    return std::get_if<l06::GroupHeader>(&messages.front()->message);
}

// The Group streams of the goaway subscription, in first-seen order.
std::vector<const LiteStreamRecord*> subscription_groups(const std::vector<const LiteStreamRecord*>& streams) {
    std::vector<const LiteStreamRecord*> out;
    for (const auto* record : streams) {
        if (record->kind != session::LiteStreamKind::Group) continue;
        const auto* header = group_header_of(*record);
        if (header && header->subscribe_id == kL06GoawaySubscribeId) out.push_back(record);
    }
    return out;
}

}  // namespace

std::vector<std::byte> l06_goaway_bytes(std::string_view new_session_uri) {
    wire::ByteWriter message(std::size_t{1} << 16);
    if (l06::encode_goaway(l06::GoawayMessage{std::string(new_session_uri)}, message)) return {};
    wire::ByteWriter out(std::size_t{1} << 16);
    if (!l06::write_stream_type(static_cast<std::uint64_t>(l06::BidiStreamType::Goaway), out)) return {};
    for (const auto byte : message.bytes()) {
        if (!out.append_byte(byte)) return {};
    }
    return {out.bytes().begin(), out.bytes().end()};
}

std::vector<std::byte> l06_goaway_oversize_bytes() {
    // New Session URI (s): a varint length of kL06GoawayOversizeLength then that many bytes.
    wire::ByteWriter body(std::size_t{1} << 16);
    bool ok = l06::write_varint(kL06GoawayOversizeLength, body);
    for (std::size_t i = 0; ok && i < kL06GoawayOversizeLength; ++i) ok = body.append_byte(std::byte{'a'});
    wire::ByteWriter out(std::size_t{1} << 17);
    ok = ok && l06::write_stream_type(static_cast<std::uint64_t>(l06::BidiStreamType::Goaway), out) &&
         l06::write_framed_message(body.bytes(), out);
    if (!ok) return {};
    return {out.bytes().begin(), out.bytes().end()};
}

LiteProbeDefinition l06_goaway_single_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                            std::string_view track_name, std::chrono::milliseconds window,
                                            std::chrono::milliseconds cadence_allowance,
                                            std::chrono::milliseconds answer_allowance) {
    if (l06_path_segments(broadcast_path).empty())
        throw std::invalid_argument("a moq-lite-06 goaway probe needs the track fixture's broadcast path");
    if (l06_path_segments(track_name).empty())
        throw std::invalid_argument("a moq-lite-06 goaway probe needs the track fixture's track name");
    if (answer_allowance.count() <= 0 || cadence_allowance.count() <= 0)
        throw std::invalid_argument("a moq-lite-06 goaway probe needs positive allowances");
    if (deadline <= answer_allowance + cadence_allowance + window)
        throw std::invalid_argument("a moq-lite-06 goaway probe needs a deadline beyond its windows");
    auto definition = lite06::allowance_probe(kL06GoawaySingle, deadline, window);
    definition.requires_track = true;
    definition.broadcast_path = std::string(broadcast_path);
    definition.track_name = std::string(track_name);
    l06_add_announce_exchange(definition, answer_allowance);
    const auto subscribe_index = definition.steps.size();
    definition.steps.push_back(lite_open_bidi(l06_learning_subscribe_bytes(broadcast_path, track_name), false,
                                              std::string(kL06GoawayLearnLabel)));
    definition.next_steps = [window, cadence_allowance, subscribe_index](
                                const session::LiteSession& session,
                                LiteProbeContext& context) -> std::vector<LiteStep> {
        if (subscription_groups(session::peer_streams(session)).size() >= kL06GoawayMinGroups) {
            context.finished = true;
            return {lite_open_bidi(l06_goaway_bytes(kL06GoawayUri), false, std::string(kL06GoawayLabel)),
                    allowance_step(window)};
        }
        // No cadence yet: give up once the subscription ended, the session closed or the allowance passed.
        const auto stream = context.stream_of(subscribe_index);
        const auto* record = stream ? session.find(*stream) : nullptr;
        if ((record && (record->fin_seen || record->reset_seen)) || session.peer_close() ||
            elapsed(context.now_ns, context.established_ns, cadence_allowance))
            context.finished = true;
        return {};
    };
    return definition;
}

LiteProbeDefinition l06_goaway_duplicate_probe(std::chrono::milliseconds deadline,
                                               std::chrono::milliseconds allowance) {
    auto definition = lite06::allowance_probe(kL06GoawayDuplicate, deadline, allowance + kL06GoawaySettle);
    definition.steps.push_back(
        lite_open_bidi(l06_goaway_bytes(kL06GoawayUri), false, std::string(kL06GoawayFirstLabel)));
    definition.steps.push_back(lite_wait(kL06GoawaySettle, std::string(kL06GoawaySettleLabel)));
    definition.steps.push_back(
        lite_open_bidi(l06_goaway_bytes(kL06GoawayUri), false, std::string(kL06GoawaySecondLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_goaway_oversize_probe(std::chrono::milliseconds deadline,
                                              std::chrono::milliseconds allowance) {
    auto definition = lite06::allowance_probe(kL06GoawayOversize, deadline, allowance);
    definition.steps.push_back(
        lite_open_bidi(l06_goaway_oversize_bytes(), false, std::string(kL06GoawayOversizeLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

std::optional<bool> evaluate_l06_goaway_no_new_streams(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06GoawaySingle || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript))
        return std::nullopt;
    if (!proved_stimulus(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes())) return std::nullopt;
    if (!proved_stimulus(transcript, kL06GoawayLearnLabel,
                         l06_learning_subscribe_bytes(transcript.broadcast_path, transcript.track_name)))
        return std::nullopt;
    const auto* goaway = proved_stimulus(transcript, kL06GoawayLabel, l06_goaway_bytes(kL06GoawayUri));
    if (!goaway || !goaway->executed_at_ns) return std::nullopt;  // no cadence was shown, so no GOAWAY was sent
    const auto sent = *goaway->executed_at_ns;
    const auto streams = lite06::peer_streams(transcript);

    // The cadence proof: Group streams opened up to the GOAWAY, and the longest gap between two of them.
    std::vector<std::uint64_t> before;
    for (const auto* record : subscription_groups(streams))
        if (record->opened_ns <= sent) before.push_back(record->opened_ns);
    std::sort(before.begin(), before.end());
    if (before.size() < kL06GoawayMinGroups) return std::nullopt;
    std::uint64_t longest_gap = 0;
    for (std::size_t i = 2; i < before.size(); ++i) longest_gap = std::max(longest_gap, before[i] - before[i - 1]);

    // The publisher must have been watched for longer than it needed to open its next stream, after the flight time
    // the draft leaves to the stream that crossed the GOAWAY.
    const auto settled = sent + to_ns(kL06GoawayNewStreamAllowance);
    if (transcript.ended_ns < settled || transcript.ended_ns - settled < longest_gap) return std::nullopt;
    // A session close after the GOAWAY is the graceful shutdown it asks for, not the rule's subject.
    if (transcript.peer_close) return std::nullopt;

    for (const auto* record : streams) {
        if (record->opened_ns > settled) return false;
    }
    return true;
}

std::optional<bool> evaluate_l06_goaway_second_closes(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06GoawayDuplicate) return std::nullopt;
    const auto* first = proved_stimulus(transcript, kL06GoawayFirstLabel, l06_goaway_bytes(kL06GoawayUri));
    if (!first) return std::nullopt;
    const auto* second = proved_stimulus(transcript, kL06GoawaySecondLabel, l06_goaway_bytes(kL06GoawayUri));
    return lite06::judge_close_probe(transcript, kL06GoawayDuplicate, second);
}

std::optional<bool> evaluate_l06_goaway_oversize_violation(const LiteTranscript& transcript) {
    const auto* stimulus = proved_stimulus(transcript, kL06GoawayOversizeLabel, l06_goaway_oversize_bytes());
    return lite06::judge_close_probe(transcript, kL06GoawayOversize, stimulus);
}

bool l06_goaway_single_inapplicable(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06GoawaySingle || !judgeable(transcript) || !fixture_present(transcript) ||
        !transcript.peer_close || transcript.runner_closed)
        return false;
    if (!proved_stimulus(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes())) return false;
    if (!proved_stimulus(transcript, kL06GoawayLearnLabel,
                         l06_learning_subscribe_bytes(transcript.broadcast_path, transcript.track_name)))
        return false;
    const auto* goaway = proved_stimulus(transcript, kL06GoawayLabel, l06_goaway_bytes(kL06GoawayUri));
    return goaway && goaway->executed_at_ns;
}

bool l06_goaway_duplicate_inapplicable(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06GoawayDuplicate || !judgeable(transcript) || !transcript.peer_close ||
        transcript.runner_closed)
        return false;
    const auto* first = proved_stimulus(transcript, kL06GoawayFirstLabel, l06_goaway_bytes(kL06GoawayUri));
    if (!first || !first->executed_at_ns) return false;
    // The peer ended the session before the second GOAWAY went out.
    const auto* second = proved_stimulus(transcript, kL06GoawaySecondLabel, l06_goaway_bytes(kL06GoawayUri));
    return second == nullptr;
}

}  // namespace moq::interop::scenarios
