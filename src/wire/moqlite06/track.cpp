#include "moq/interop/wire/moqlite06/track.h"

#include <span>

#include "body_support.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

using namespace detail;

}  // namespace

DecodeResult<TrackRequest> decode_track_request(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    TrackRequest message;
    if (const auto error = body_string(cursor, message.broadcast_path, limits, "track broadcast path is truncated")) {
        return *error;
    }
    if (const auto error = body_string(cursor, message.track_name, limits, "track name is truncated")) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

DecodeResult<TrackInfo> decode_track_info(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    TrackInfo message;
    // Publisher Priority (8): one raw byte, never a varint.
    const auto priority = read_bytes(cursor, 1);
    if (const auto error = body_error(priority, cursor.offset(), "track info has no publisher priority")) {
        return *error;
    }
    message.publisher_priority = static_cast<std::uint8_t>(std::get<std::span<const std::byte>>(priority)[0]);
    if (const auto error = body_varint(cursor, message.publisher_max_age_ms, "track info has no max age")) {
        return *error;
    }
    if (const auto error = body_varint(cursor, message.timescale, "track info has no timescale")) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

std::optional<EncodeError> encode_track_request(const TrackRequest& message, ByteWriter& output,
                                                const DecodeLimits& limits) {
    if (message.broadcast_path.size() > limits.max_string_length ||
        message.track_name.size() > limits.max_string_length) {
        return EncodeError::LimitExceeded;
    }
    ByteWriter body(limits.max_message_length);
    if (!write_string(message.broadcast_path, body) || !write_string(message.track_name, body)) {
        return EncodeError::LimitExceeded;
    }
    return emit(std::nullopt, body, output);
}

std::optional<EncodeError> encode_track_info(const TrackInfo& message, ByteWriter& output,
                                             const DecodeLimits& limits) {
    if (message.publisher_max_age_ms > kMaxVarint || message.timescale > kMaxVarint) return EncodeError::InvalidValue;
    ByteWriter body(limits.max_message_length);
    if (!body.append_byte(static_cast<std::byte>(message.publisher_priority)) ||
        !write_varint(message.publisher_max_age_ms, body) || !write_varint(message.timescale, body)) {
        return EncodeError::LimitExceeded;
    }
    return emit(std::nullopt, body, output);
}

}  // namespace moq::interop::wire::moqlite06
