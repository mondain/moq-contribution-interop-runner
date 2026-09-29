#include "moq/interop/wire/draft21/goaway.h"

#include <array>
#include <span>
#include <variant>

namespace moq::interop::wire::draft21 {
namespace {

constexpr std::uint64_t kGoawayType = 0x10;
constexpr std::size_t kMaximumUriLength = 8192;
constexpr std::size_t kMaximumBodyLength = 65535;

DecodeError violation(std::size_t offset, const char* detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, detail};
}

}  // namespace

DecodeResult<GoawayMessage> decode_goaway(Cursor& input,
                                          bool received_from_client) {
    Cursor working = input;
    const auto type = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&type)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type)) return *error;
    if (std::get<std::uint64_t>(type) != kGoawayType) {
        return violation(input.offset(), "expected draft-21 GOAWAY");
    }
    const auto length = read_bytes(working, 2);
    if (const auto* need = std::get_if<NeedMore>(&length)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length)) return *error;
    const auto length_bytes = std::get<std::span<const std::byte>>(length);
    const auto body_length =
        (static_cast<std::size_t>(std::to_integer<unsigned>(length_bytes[0])) << 8u) |
        std::to_integer<unsigned>(length_bytes[1]);
    const auto body = read_bytes(working, body_length);
    if (const auto* need = std::get_if<NeedMore>(&body)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&body)) return *error;

    Cursor payload(std::get<std::span<const std::byte>>(body),
                   working.offset() - body_length);
    const auto uri = read_length_prefixed_bytes(payload, kMaximumUriLength);
    if (std::holds_alternative<NeedMore>(uri)) {
        return violation(payload.offset(), "truncated GOAWAY URI");
    }
    if (const auto* error = std::get_if<DecodeError>(&uri)) {
        if (error->code == DecodeErrorCode::LengthExceedsLimit) {
            return violation(error->offset, "GOAWAY URI exceeds 8192 bytes");
        }
        return *error;
    }
    const auto uri_bytes = std::get<std::span<const std::byte>>(uri);
    if (received_from_client && !uri_bytes.empty()) {
        return violation(payload.offset(), "client GOAWAY URI must be empty");
    }
    const auto timeout = read_vi64(payload);
    if (std::holds_alternative<NeedMore>(timeout)) {
        return violation(payload.offset(), "truncated GOAWAY timeout");
    }
    if (const auto* error = std::get_if<DecodeError>(&timeout)) return *error;
    if (payload.remaining() != 0) {
        return violation(payload.offset(), "GOAWAY body length mismatch");
    }

    input = working;
    return GoawayMessage{std::vector<std::byte>(uri_bytes.begin(),
                                                uri_bytes.end()),
                          std::get<std::uint64_t>(timeout)};
}

std::optional<GoawayEncodeError> encode_goaway(
    const GoawayMessage& message, bool sending_as_client, ByteWriter& output) {
    if (message.new_session_uri.size() > kMaximumUriLength ||
        (sending_as_client && !message.new_session_uri.empty())) {
        return GoawayEncodeError::InvalidValue;
    }
    ByteWriter body(kMaximumBodyLength);
    if (!write_length_prefixed_bytes(message.new_session_uri, body) ||
        !write_vi64(message.timeout_ms, body)) {
        return GoawayEncodeError::OutputCapacity;
    }
    ByteWriter frame(kMaximumBodyLength + 11);
    const std::array<std::byte, 2> length{
        static_cast<std::byte>(body.size() >> 8u),
        static_cast<std::byte>(body.size())};
    if (!write_vi64(kGoawayType, frame) || !frame.append_bytes(length) ||
        !frame.append_bytes(body.bytes()) ||
        !output.append_bytes(frame.bytes())) {
        return GoawayEncodeError::OutputCapacity;
    }
    return std::nullopt;
}

}  // namespace moq::interop::wire::draft21
