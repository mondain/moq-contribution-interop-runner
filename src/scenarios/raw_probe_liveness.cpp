#include "moq/interop/scenarios/raw_probe_liveness.h"
#include "moq/interop/wire/draft18/messages.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
// SOUNDNESS ARGUMENT
//
// Claim: after a stimulus for which the draft says the publisher MUST close the
// session, a publisher that goes on to answer a fresh, valid SUBSCRIBE with a
// well-formed SUBSCRIBE_OK, and that never closes, did not close in response to
// the stimulus. That is a violation the runner can score FAIL. It rests on:
//
//  1. The draft states an unconditional MUST close for the stimulus (no
//     "or reject", no deferral to a later event, not SHOULD). Rows that only
//     SHOULD close, or whose close hangs on a later message, are not listed.
//  2. The stimulus reached the publisher. Reliable streams only: a datagram can
//     be lost without any trace, so a publisher that never saw it would look
//     like one that ignored it. Datagram probes are excluded (and refused by
//     liveness_definition_eligible).
//  3. The publisher had the stimulus in hand before it saw the follow-up. The
//     follow-up is sent `delay` after the stimulus was fully accepted by the
//     transport and the publisher's SETUP was seen. QUIC orders bytes within a
//     stream, not across streams, and the transport offers no acknowledgement
//     signal, so this is a time bound, not a proof: the residual risk is a
//     publisher that leaves a stream unread for longer than `delay` while
//     serving another. `grace` then keeps the probe open after the answer, and
//     any close at all, in the delay, the answer or the grace, ends the claim
//     (the existing close rules apply unchanged).
//  4. The answer is unambiguously from after the stimulus and bound to the
//     follow-up: it is read from a stream the runner opened after the stimulus
//     and used for nothing else, and only a SUBSCRIBE_OK counts. A REQUEST_ERROR
//     also proves the session is open and processing requests, but it is
//     confounded: a publisher may refuse new requests after a GOAWAY (Section
//     9.2/10.4 "MAY reject new requests"), may refuse because the track is not
//     available, or may be refusing as part of a close it has not finished.
//     Refusal is therefore never scored; only service is.
//  5. A conforming publisher cannot produce that answer: if it had closed, the
//     transport would be gone; if it had not, it violated the MUST in 1.
//
// The follow-up never produces a PASS and never overrides a close, a
// PROTOCOL_VIOLATION or any other event the existing rules already score.
//
// Families deliberately NOT covered, and why:
//  - Datagram stimuli (point 2).
//  - SHOULD rows (D18-10-2-SHOULD-002, D21-9-20-SHOULD-401): not a MUST.
//  - Duplicate Request ID across streams (D18-10-1-MUST-002,
//    D21-6-4-2-1-MUST-155): the first request may be rejected before its ID is
//    recorded, so whether the repeat is a "duplicate" is not established.
//  - Data-stream header probes (D18-11-4-2-MUST-002/003): a publisher that has
//    no subscription may not parse data streams at all, so "receives an invalid
//    header" is not established.
//  - Multi-step stimuli gated on earlier responses (the D21 update, notify and
//    discovery-update rows), TRACK_STATUS plus REQUEST_UPDATE, and an unknown
//    message that trails a complete request (d21-unknown-request-stream-message):
//    the second message only matters if the first request is still open and being
//    read, which the publisher decides (it may refuse the first and stop reading).
//  - Peer-close probes, where the runner answers a request the publisher opened:
//    the publisher may already have cancelled that request, so the answer is
//    never "received" in a state the draft constrains.
//  - Native-only connection-close variants (D18-10-2-1-MUST-001): unchanged.
//  - Probes whose subject is a response rather than a close (REQUEST_ERROR rows).
struct Entry { unsigned draft; std::string_view scenario; };
constexpr Entry kSound[] = {
    // Control-stream GOAWAY (draft 18 Section 10.4, draft 21 Section 9.2): a second
    // GOAWAY, a New Session URI over 8192 bytes, and (draft 18) a cutoff Request ID
    // of the wrong parity are each "MUST close". A first, well-formed GOAWAY is
    // not a violation and is not used here.
    {18, "receive-two-goaways-on-control-stream"},
    {18, "receive-goaway-uri-length-8193"},
    {18, "receive-control-goaway-with-wrong-receiver-request-id-parity"},
    {21, "d21-duplicate-control-goaway"},
    {21, "d21-goaway-uri-length-boundary"},
    // The same duplicate on one request stream ("or on a single request stream").
    {18, "receive-two-goaways-on-same-request-stream"},
    {21, "d21-duplicate-request-goaway"},
    // SETUP content from the runner acting as the server: AUTHORITY or PATH from
    // a server (or over WebTransport), and malformed Setup Options. Each is
    // "MUST close" and is read before any request is.
    {18, "receive-server-setup-with-authority"},
    {18, "receive-webtransport-setup-with-authority"},
    {18, "receive-server-setup-with-path"},
    {18, "receive-webtransport-setup-with-path"},
    {18, "receive-key-value-type-overflow"},
    {18, "receive-key-value-length-over-65535"},
    {18, "receive-understood-key-value-invalid-serialization"},
    {21, "d21-setup-key-value-type-overflow"},
    {21, "d21-setup-key-value-declared-length-overflow"},
    {21, "d21-setup-known-key-value-malformed-value"},
    // One complete violating message on a new stream or the control stream.
    {18, "receive-unknown-unidirectional-stream-type"},
    {18, "receive-unknown-message-type"},
    {18, "receive-disallowed-initial-bidirectional-message"},
    {18, "receive-known-message-with-mismatched-payload-length"},
    {18, "receive-zero-length-namespace-field"},
    {18, "receive-namespace-with-33-fields"},
    {18, "receive-track-namespace-over-4096-bytes"},
    {18, "receive-full-track-name-over-4096-bytes"},
    {18, "receive-subscribe-namespace-with-33-prefix-fields"},
    {18, "receive-subscribe-tracks-with-33-prefix-fields"},
    {18, "receive-request-id-wrong-peer-parity"},
    {18, "receive-message-parameter-type-delta-overflow"},
    {18, "receive-unnegotiated-unknown-message-parameter"},
    {18, "receive-undecodable-authorization-token-structure"},
    {18, "receive-group-order-zero-or-greater-than-two"},
    {18, "receive-forward-outside-zero-one"},
    {18, "receive-undefined-subscription-filter-type"},
    {18, "absolute-range-end-group-overflow"},
    {18, "receive-fetch-with-unknown-type"},
    {18, "register-same-peer-token-alias-twice-without-delete"},
    {18, "register-request-token-exceeding-advertised-cache-size"},
    {21, "d21-invalid-bidirectional-request-stream-opener"},
    {21, "d21-unknown-unidirectional-stream-type"},
    {21, "d21-unknown-control-message"},
    {21, "d21-message-body-length-mismatch"},
    {21, "d21-request-id-wrong-sender-parity"},
    {21, "d21-subscribe-empty-namespace-field"},
    {21, "d21-subscribe-33-namespace-fields"},
    {21, "d21-subscribe-namespace-prefix-too-many-fields"},
    {21, "d21-subscribe-tracks-prefix-too-many-fields"},
    {21, "d21-subscribe-tracks-oversized-namespace"},
    {21, "d21-subscribe-oversized-full-track-name"},
    {21, "d21-parameter-type-delta-overflow"},
    {21, "d21-request-undecodable-authorization-token"},
    {21, "d21-unknown-message-parameter"},
    {21, "d21-parameter-invalid-message-scope"},
    {21, "d21-group-order-zero"},
    {21, "d21-group-order-above-two"},
    {21, "d21-location-filter-end-group-overflow"},
    {21, "d21-forward-value-two"},
    {21, "d21-forward-value-255"},
    {21, "d21-include-properties-value-two"},
    {21, "d21-include-properties-value-255"},
    {21, "d21-fill-forbidden-nested-authorization"},
    {21, "d21-fill-forbidden-track-property-filter"},
    {21, "d21-fill-recursive-parameter"},
    {21, "d21-fill-invalid-group-order"},
    {21, "d21-fill-location-filter-end-group-overflow"},
    {21, "d21-fill-timeout-outside-fill-or-fetch"},
    {21, "d21-request-message-truncated-at-fin"},
    {21, "d21-token-duplicate-registration"},
    {21, "d21-request-token-cache-overflow"},
    {21, "d21-request-alias-registration-with-default-zero-cache"},
};

namespace d18 = wire::draft18;
namespace d21 = wire::draft21;

bool valid_vi(wire::Cursor& cursor, std::uint64_t& value) {
    const auto decoded = wire::read_vi64(cursor);
    const auto* number = std::get_if<std::uint64_t>(&decoded);
    if (!number) return false;
    value = *number;
    return true;
}
}  // namespace

bool liveness_follow_up_sound(unsigned draft, std::string_view scenario_id) {
    return std::any_of(std::begin(kSound), std::end(kSound), [&](const Entry& entry) {
        return entry.draft == draft && entry.scenario == scenario_id;
    });
}

bool liveness_definition_eligible(const RawProbeDefinition& definition) {
    if (definition.offer_replacement_session) return false;
    return std::all_of(definition.writes.begin(), definition.writes.end(), [](const RawProbeWrite& write) {
        return write.operation == RawProbeOperation::Write &&
               (write.channel == RawProbeChannel::NewUni || write.channel == RawProbeChannel::NewBidi ||
                write.channel == RawProbeChannel::Control) &&
               !write.select_peer_stream && !write.peer_response_ready;
    });
}

void apply_liveness_policy(RawProbeDefinition& definition, unsigned draft) {
    if ((draft != 18 && draft != 21) || !liveness_follow_up_sound(draft, definition.id) ||
        !liveness_definition_eligible(definition)) {
        definition.liveness.reset();
        return;
    }
    RawProbeLiveness policy;
    policy.draft = draft;
    definition.liveness = std::move(policy);
}

void bind_liveness_track(RawProbeDefinition& definition,
                         const std::vector<std::vector<std::byte>>& track_namespace,
                         const std::vector<std::byte>& track_name) {
    if (!definition.liveness || track_namespace.empty() || track_namespace.size() > 32 || track_name.empty()) return;
    std::size_t total = track_name.size();
    for (const auto& field : track_namespace) {
        if (field.empty()) return;
        total += field.size();
    }
    if (total > 4096) return;
    // Figure for SUBSCRIBE in both drafts: Request ID, Track Namespace, Track Name,
    // Number of Parameters (zero here).
    wire::ByteWriter body(65535);
    bool ok = wire::write_vi64(definition.liveness->request_id, body) &&
              wire::write_vi64(track_namespace.size(), body);
    for (const auto& field : track_namespace)
        ok = ok && wire::write_vi64(field.size(), body) && body.append_bytes(field);
    ok = ok && wire::write_vi64(track_name.size(), body) && body.append_bytes(track_name) &&
         wire::write_vi64(0, body);
    wire::ByteWriter frame(65546);
    ok = ok && wire::write_vi64(3, frame) &&
         frame.append_byte(static_cast<std::byte>(body.size() >> 8u)) &&
         frame.append_byte(static_cast<std::byte>(body.size() & 255u)) && frame.append_bytes(body.bytes());
    if (!ok) return;
    definition.liveness->request.assign(frame.bytes().begin(), frame.bytes().end());
}

bool liveness_request_valid(const RawProbeLiveness& policy) {
    if ((policy.draft != 18 && policy.draft != 21) || (policy.request_id & 1u) == 0 ||
        policy.request.empty() || policy.request.size() > 65535) return false;
    wire::Cursor cursor(policy.request);
    std::uint64_t type = 0;
    if (!valid_vi(cursor, type) || type != 3) return false;
    const auto length = wire::read_bytes(cursor, 2);
    const auto* prefix = std::get_if<std::span<const std::byte>>(&length);
    if (!prefix) return false;
    const std::size_t size = (std::to_integer<std::size_t>((*prefix)[0]) << 8u) |
                             std::to_integer<std::size_t>((*prefix)[1]);
    if (size != cursor.remaining()) return false;
    std::uint64_t id = 0, count = 0, parameters = 0;
    if (!valid_vi(cursor, id) || id != policy.request_id || !valid_vi(cursor, count) || count == 0 || count > 32)
        return false;
    std::size_t total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t field = 0;
        if (!valid_vi(cursor, field) || field == 0 || field > cursor.remaining()) return false;
        total += static_cast<std::size_t>(field);
        if (!std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor, static_cast<std::size_t>(field))))
            return false;
    }
    std::uint64_t name = 0;
    if (!valid_vi(cursor, name) || name == 0 || name > cursor.remaining()) return false;
    total += static_cast<std::size_t>(name);
    if (!std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor, static_cast<std::size_t>(name))))
        return false;
    return total <= 4096 && valid_vi(cursor, parameters) && parameters == 0 && cursor.remaining() == 0;
}

LivenessAnswer liveness_answer(const RawProbeTranscript& transcript, unsigned draft) {
    if (!transcript.liveness || !transcript.liveness->write.stream_id ||
        !transcript.liveness->write.delivery_event_count ||
        *transcript.liveness->write.delivery_event_count > transcript.events.size()) return LivenessAnswer::None;
    const auto stream = *transcript.liveness->write.stream_id;
    std::vector<std::byte> bytes;
    bool finished = false;
    for (std::size_t i = *transcript.liveness->write.delivery_event_count; i < transcript.events.size(); ++i) {
        const auto& event = transcript.events[i];
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event); data && data->stream_id == stream) {
            if (data->data.size() > 65546 - bytes.size()) return LivenessAnswer::Other;
            bytes.insert(bytes.end(), data->data.begin(), data->data.end());
            finished = finished || data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
                   reset && reset->stream_id == stream) {
            return LivenessAnswer::Other;
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event);
                   stop && stop->stream_id == stream) {
            return LivenessAnswer::Other;
        }
    }
    if (bytes.empty()) return finished ? LivenessAnswer::Other : LivenessAnswer::None;
    wire::Cursor cursor(bytes);
    if (draft == 21) {
        const auto decoded = d21::decode_successful_response(cursor, d21::ResponseContext::Subscribe);
        if (std::holds_alternative<d21::SuccessfulResponse>(decoded)) return LivenessAnswer::Serving;
        return std::holds_alternative<wire::NeedMore>(decoded) && !finished ? LivenessAnswer::None
                                                                             : LivenessAnswer::Other;
    }
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    if (const auto* message = std::get_if<d18::Message>(&decoded))
        return std::holds_alternative<d18::SubscribeOkMessage>(*message) ? LivenessAnswer::Serving
                                                                         : LivenessAnswer::Other;
    return std::holds_alternative<wire::NeedMore>(decoded) && !finished ? LivenessAnswer::None
                                                                         : LivenessAnswer::Other;
}

void RawProbeController::step_liveness(RawProbeClock::time_point now) {
    const auto& policy = definition_.liveness;
    if (!policy || policy->request.empty() || transcript_.harness_failed || session_closed_ ||
        liveness_abandoned_ || transcript_.complete) return;
    if (transcript_.peer_setup_received && !peer_setup_at_) peer_setup_at_ = now;
    auto& record = transcript_.liveness;
    if (!record) {
        if (!transcript_.stimulus_delivered || !delivered_at_ || !peer_setup_at_) return;
        const auto anchor = std::max(*delivered_at_, *peer_setup_at_);
        if (now - anchor < policy->delay) return;
        const auto opened = transport_.open_bidi();
        if (opened.status == transport::TransportStatus::WouldBlock ||
            opened.status == transport::TransportStatus::StreamLimit) return;
        // A closing session is not a harness fault: the close, if any, is the evidence.
        if (opened.status != transport::TransportStatus::Success) { liveness_abandoned_ = true; return; }
        record.emplace();
        record->write.write = {RawProbeChannel::NewBidi, policy->request, false};
        record->write.stream_id = opened.stream_id;
        record->anchor_at = anchor;
        record->anchor_event_count = transcript_.events.size();
    }
    auto& write = record->write;
    if (!write.delivery_event_count) {
        const auto remaining = std::span<const std::byte>(write.write.bytes).subspan(write.accepted);
        const auto result = transport_.write(*write.stream_id, remaining, false);
        if (result.status == transport::TransportStatus::WouldBlock && result.accepted == 0) return;
        if ((result.status != transport::TransportStatus::Success &&
             result.status != transport::TransportStatus::Partial) || result.accepted > remaining.size()) {
            liveness_abandoned_ = true;
            return;
        }
        write.accepted += result.accepted;
        if (write.accepted != write.write.bytes.size()) return;
        write.delivery_event_count = transcript_.events.size();
        write.accepted_at = now;
        return;
    }
    if (!record->answered_at) {
        if (liveness_answer(transcript_, policy->draft) == LivenessAnswer::Serving) record->answered_at = now;
        return;
    }
    if (now - *record->answered_at >= policy->grace) {
        record->settled_at = now;
        transcript_.complete = true;
    }
}

}  // namespace moq::interop::scenarios
