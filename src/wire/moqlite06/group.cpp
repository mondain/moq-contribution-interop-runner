#include "moq/interop/wire/moqlite06/group.h"

#include <span>
#include <variant>

#include "body_support.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {

using namespace detail;

DecodeResult<GroupHeader> decode_group_header(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    GroupHeader header;
    if (const auto error = body_varint(cursor, header.subscribe_id, "group has no subscribe id")) return *error;
    if (const auto error = body_varint(cursor, header.group_sequence, "group has no group sequence")) return *error;
    if (const auto error = body_varint(cursor, header.frame_start, "group has no frame start")) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return header;
}

DecodeResult<Frame> decode_frame(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto delta = read_varint(working);
    if (const auto* need = std::get_if<NeedMore>(&delta)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&delta)) return *error;

    // The payload length is judged before the payload is read, so a hostile length never allocates.
    const auto framed = read_framed_body(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto payload = std::get<std::span<const std::byte>>(framed);

    Frame result;
    result.timestamp_delta = zigzag_decode(std::get<std::uint64_t>(delta));
    result.payload.assign(payload.begin(), payload.end());
    input = working;
    return result;
}

std::optional<EncodeError> encode_group_header(const GroupHeader& header, ByteWriter& output,
                                               const DecodeLimits& limits) {
    if (header.subscribe_id > kMaxVarint || header.group_sequence > kMaxVarint || header.frame_start > kMaxVarint) {
        return EncodeError::InvalidValue;
    }
    ByteWriter body(limits.max_message_length);
    if (!write_varint(header.subscribe_id, body) || !write_varint(header.group_sequence, body) ||
        !write_varint(header.frame_start, body)) {
        return EncodeError::LimitExceeded;
    }
    return emit(std::nullopt, body, output);
}

std::optional<EncodeError> encode_frame(const Frame& message, ByteWriter& output, const DecodeLimits& limits) {
    const std::uint64_t zigzag = zigzag_encode(message.timestamp_delta);
    if (zigzag > kMaxVarint) return EncodeError::InvalidValue;
    if (message.payload.size() > limits.max_message_length) return EncodeError::LimitExceeded;

    const std::size_t total = varint_size(zigzag) + varint_size(message.payload.size()) + message.payload.size();
    if (total > output.remaining()) return EncodeError::OutputCapacity;
    if (!write_varint(zigzag, output) || !write_framed_message(message.payload, output)) {
        return EncodeError::OutputCapacity;
    }
    return std::nullopt;
}

}  // namespace moq::interop::wire::moqlite06
