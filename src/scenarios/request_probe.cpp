#include "moq/interop/scenarios/request_probe.h"
#include "moq/interop/wire/draft18/messages.h"
#include "moq/interop/wire/draft21/request_error.h"

namespace moq::interop::scenarios {
namespace {
constexpr std::size_t kMaximumResponseBytes = 65546;
enum class ResponseState { Pending, Frame, Invalid, Cancelled };
struct Response {
    ResponseState state{ResponseState::Pending};
    std::uint64_t type{0};
    std::vector<std::byte> bytes;
};

Response observe_response(const RawProbeTranscript& transcript) {
    Response result;
    if (!transcript.stimulus_delivered || !transcript.delivery_event_count ||
        transcript.writes.size() != 1 || !transcript.writes.front().stream_id ||
        transcript.writes.front().write.channel != RawProbeChannel::NewBidi)
        return result;
    const auto target = *transcript.writes.front().stream_id;
    std::vector<std::byte> received;
    bool fin = false;
    bool reset_received = false;
    for (std::size_t i = *transcript.delivery_event_count; i < transcript.events.size(); ++i) {
        const auto& event = transcript.events[i];
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && data->stream_id == target) {
            if (data->data.size() > kMaximumResponseBytes - received.size()) {
                result.state = ResponseState::Invalid;
                return result;
            }
            received.insert(received.end(), data->data.begin(), data->data.end());
            fin = fin || data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
                   reset && reset->stream_id == target) {
            reset_received = true;
            break;
        }
    }
    wire::Cursor cursor(received);
    const auto type = wire::read_vi64(cursor);
    if (std::holds_alternative<wire::DecodeError>(type)) {
        result.state = ResponseState::Invalid;
        return result;
    }
    const auto* message_type = std::get_if<std::uint64_t>(&type);
    if (!message_type || cursor.remaining() < 2) {
        if (reset_received) result.state = ResponseState::Cancelled;
        else if (fin) result.state = ResponseState::Invalid;
        return result;
    }
    result.type = *message_type;
    const auto offset = cursor.offset();
    const auto length = (std::to_integer<std::size_t>(received[offset]) << 8u) |
                         std::to_integer<std::size_t>(received[offset + 1]);
    if (received.size() - offset - 2 < length) {
        if (reset_received) result.state = ResponseState::Cancelled;
        else if (fin) result.state = ResponseState::Invalid;
        return result;
    }
    if (received.size() - offset - 2 != length) {
        result.state = ResponseState::Invalid;
        return result;
    }
    result.state = ResponseState::Frame;
    result.bytes = std::move(received);
    return result;
}
}  // namespace

bool request_probe_response_ready(const RawProbeTranscript& transcript) {
    return observe_response(transcript).state != ResponseState::Pending;
}

std::optional<bool> evaluate_raw_probe_request_error(
    const RawProbeTranscript& transcript, const RequestProbeProfile& profile) {
    if (!raw_probe_stimulus_valid(transcript, profile.definition)) return std::nullopt;
    const auto response = observe_response(transcript);
    if (response.state == ResponseState::Pending) {
        // A publisher that ends the session instead of answering fails the rule only when the
        // close is attributable to the request; a NO_ERROR close is not (see observe_close).
        const auto observed = observe_raw_probe_close(transcript, profile.definition);
        if (observed && observed->error_code != 0) return false;
        return std::nullopt;
    }
    if (response.state != ResponseState::Frame || response.type != 5) return false;
    const auto expected = profile.compatibility_error
        ? transcript.unknown_auth_token_alias_compatibility_code
        : std::optional<std::uint64_t>(profile.expected_error);
    wire::Cursor cursor(response.bytes);
    if (profile.draft == 18) {
        const auto message = wire::draft18::decode_message(
            wire::draft18::StreamRole::Request, cursor, {});
        const auto* frame = std::get_if<wire::draft18::Message>(&message);
        const auto* error = frame ? std::get_if<wire::draft18::RequestErrorMessage>(frame) : nullptr;
        if (!error || cursor.remaining() != 0) return false;
        if (!expected) return std::nullopt;
        return error->error_code == *expected;
    }
    if (profile.draft == 21) {
        const auto message = wire::draft21::decode_request_error(cursor, true, profile.namespace_scoped);
        const auto* error = std::get_if<wire::draft21::RequestErrorMessage>(&message);
        if (!error || cursor.remaining() != 0) return false;
        if (!expected) return std::nullopt;
        return error->error_code == *expected;
    }
    return std::nullopt;
}
}  // namespace moq::interop::scenarios
