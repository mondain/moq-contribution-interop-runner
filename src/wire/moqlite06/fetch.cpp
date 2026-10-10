#include "moq/interop/wire/moqlite06/fetch.h"

#include <span>

#include "body_support.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

using namespace detail;

}  // namespace

bool fetch_range_inverted(const FetchRequest& request) {
    return request.frame_end != 0 && request.frame_end - 1 < request.frame_start;
}

DecodeResult<FetchRequest> decode_fetch_request(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    FetchRequest message;
    if (const auto error = body_string(cursor, message.broadcast_path, limits, "fetch broadcast path is truncated")) {
        return *error;
    }
    if (const auto error = body_string(cursor, message.track_name, limits, "fetch track name is truncated")) {
        return *error;
    }
    // Subscriber Priority (8): one raw byte, never a varint.
    const auto priority = read_bytes(cursor, 1);
    if (const auto error = body_error(priority, cursor.offset(), "fetch has no subscriber priority")) return *error;
    message.subscriber_priority = static_cast<std::uint8_t>(std::get<std::span<const std::byte>>(priority)[0]);
    if (const auto error = body_varint(cursor, message.group_sequence, "fetch has no group sequence")) return *error;
    if (const auto error = body_varint(cursor, message.frame_start, "fetch has no frame start")) return *error;
    if (const auto error = body_varint(cursor, message.frame_end, "fetch has no frame end")) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

std::optional<EncodeError> encode_fetch_request(const FetchRequest& message, ByteWriter& output,
                                                const DecodeLimits& limits) {
    if (message.group_sequence > kMaxVarint || message.frame_start > kMaxVarint || message.frame_end > kMaxVarint ||
        fetch_range_inverted(message)) {
        return EncodeError::InvalidValue;
    }
    if (message.broadcast_path.size() > limits.max_string_length ||
        message.track_name.size() > limits.max_string_length) {
        return EncodeError::LimitExceeded;
    }
    ByteWriter body(limits.max_message_length);
    if (!write_string(message.broadcast_path, body) || !write_string(message.track_name, body) ||
        !body.append_byte(static_cast<std::byte>(message.subscriber_priority)) ||
        !write_varint(message.group_sequence, body) || !write_varint(message.frame_start, body) ||
        !write_varint(message.frame_end, body)) {
        return EncodeError::LimitExceeded;
    }
    return emit(std::nullopt, body, output);
}

}  // namespace moq::interop::wire::moqlite06
