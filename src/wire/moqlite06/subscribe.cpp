#include "moq/interop/wire/moqlite06/subscribe.h"

#include <span>
#include <utility>

#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

DecodeError violation(std::size_t offset, const char* detail) {
    return DecodeError{DecodeErrorCode::ProtocolViolation, offset, detail};
}

// A field read from inside a body: running out of body is a malformed message, not a short input.
template <class T>
std::optional<DecodeError> body_error(const DecodeResult<T>& result, std::size_t offset, const char* detail) {
    if (std::holds_alternative<NeedMore>(result)) return violation(offset, detail);
    if (const auto* error = std::get_if<DecodeError>(&result)) return *error;
    return std::nullopt;
}

std::optional<DecodeError> body_varint(Cursor& cursor, std::uint64_t& value, const char* detail) {
    const auto result = read_varint(cursor);
    if (const auto error = body_error(result, cursor.offset(), detail)) return error;
    value = std::get<std::uint64_t>(result);
    return std::nullopt;
}

std::optional<DecodeError> body_string(Cursor& cursor, std::string& value, const DecodeLimits& limits,
                                       const char* detail) {
    const auto result = read_string(cursor, limits.max_string_length);
    if (const auto error = body_error(result, cursor.offset(), detail)) return error;
    value = std::get<std::string>(result);
    return std::nullopt;
}

// Subscriber Priority (8): one raw byte, never a varint.
std::optional<DecodeError> body_priority(Cursor& cursor, std::uint8_t& value) {
    const auto result = read_bytes(cursor, 1);
    if (const auto error = body_error(result, cursor.offset(), "subscribe has no subscriber priority")) return error;
    value = static_cast<std::uint8_t>(std::get<std::span<const std::byte>>(result)[0]);
    return std::nullopt;
}

// Subscriber Priority (8), Subscriber Max Age, Group Start, Group End, Frame Start, Frame End.
std::optional<DecodeError> read_range(Cursor& cursor, SubscribeRange& range) {
    if (const auto error = body_priority(cursor, range.subscriber_priority)) return error;
    if (const auto error = body_varint(cursor, range.subscriber_max_age_ms, "subscribe has no max age")) return error;
    if (const auto error = body_varint(cursor, range.group_start, "subscribe has no group start")) return error;
    if (const auto error = body_varint(cursor, range.group_end, "subscribe has no group end")) return error;
    if (const auto error = body_varint(cursor, range.frame_start, "subscribe has no frame start")) return error;
    if (const auto error = body_varint(cursor, range.frame_end, "subscribe has no frame end")) return error;
    return std::nullopt;
}

// Draft 7.9: Frame End MUST be 0 when Group End is 0.
std::optional<DecodeError> check_range_coupling(const SubscribeRange& range, std::size_t offset) {
    if (range.frame_end != 0 && range.group_end == 0) {
        return violation(offset, "frame end is set but group end is unbounded");
    }
    return std::nullopt;
}

std::optional<EncodeError> check_range(const SubscribeRange& range) {
    if (range.subscriber_max_age_ms > kMaxVarint || range.group_start > kMaxVarint || range.group_end > kMaxVarint ||
        range.frame_start > kMaxVarint || range.frame_end > kMaxVarint) {
        return EncodeError::InvalidValue;
    }
    if (range.frame_end != 0 && range.group_end == 0) return EncodeError::InvalidValue;
    return std::nullopt;
}

bool write_range(const SubscribeRange& range, ByteWriter& body) {
    return body.append_byte(static_cast<std::byte>(range.subscriber_priority)) &&
           write_varint(range.subscriber_max_age_ms, body) && write_varint(range.group_start, body) &&
           write_varint(range.group_end, body) && write_varint(range.frame_start, body) &&
           write_varint(range.frame_end, body);
}

// Frames the next message from `working` into a body span; `working` is advanced past it.
struct Framed {
    std::span<const std::byte> body;
    std::size_t body_offset;
};

DecodeResult<Framed> frame(Cursor& working, const DecodeLimits& limits) {
    const auto framed = read_framed_body(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto body = std::get<std::span<const std::byte>>(framed);
    return Framed{body, working.offset() - body.size()};
}

// Writes [Type (i)] Message Length (i) body, or nothing at all.
std::optional<EncodeError> emit(std::optional<std::uint64_t> type, const ByteWriter& body, ByteWriter& output) {
    const auto bytes = body.bytes();
    const std::size_t length_size = varint_size(bytes.size());
    if (length_size == 0) return EncodeError::LimitExceeded;
    const std::size_t total = (type ? varint_size(*type) : 0) + length_size + bytes.size();
    if (total > output.remaining()) return EncodeError::OutputCapacity;
    if (type && !write_varint(*type, output)) return EncodeError::OutputCapacity;
    if (!write_framed_message(bytes, output)) return EncodeError::OutputCapacity;
    return std::nullopt;
}

}  // namespace

DecodeResult<Subscribe> decode_subscribe(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    Subscribe message;
    if (const auto error = body_varint(cursor, message.subscribe_id, "subscribe has no subscribe id")) return *error;
    if (const auto error = body_string(cursor, message.broadcast_path, limits, "subscribe broadcast path is truncated")) {
        return *error;
    }
    if (const auto error = body_string(cursor, message.track_name, limits, "subscribe track name is truncated")) {
        return *error;
    }
    if (const auto error = read_range(cursor, message.range)) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;
    if (const auto error = check_range_coupling(message.range, body_offset)) return *error;

    input = working;
    return message;
}

DecodeResult<SubscribeUpdate> decode_subscribe_update(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    SubscribeUpdate message;
    if (const auto error = read_range(cursor, message.range)) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;
    if (const auto error = check_range_coupling(message.range, body_offset)) return *error;

    input = working;
    return message;
}

DecodeResult<SubscribeResponse> decode_subscribe_response(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto type_result = read_varint(working);
    if (const auto* need = std::get_if<NeedMore>(&type_result)) return *need;
    const auto type = std::get<std::uint64_t>(type_result);
    if (type > kSubscribeTypeDrop) {
        return DecodeError{DecodeErrorCode::InvalidValue, input.offset(), "unknown subscribe response type"};
    }

    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);
    Cursor cursor(body, body_offset);

    SubscribeResponse message;
    if (type == kSubscribeTypeOk) {
        SubscribeOk ok;
        if (const auto error = body_varint(cursor, ok.group, "subscribe ok has no group")) return *error;
        message = ok;
    } else if (type == kSubscribeTypeEnd) {
        SubscribeEnd end;
        if (const auto error = body_varint(cursor, end.group, "subscribe end has no group")) return *error;
        message = end;
    } else {
        SubscribeDrop drop;
        if (const auto error = body_varint(cursor, drop.group_start, "subscribe drop has no group start")) {
            return *error;
        }
        if (const auto error = body_varint(cursor, drop.group_end, "subscribe drop has no group end")) return *error;
        if (const auto error = body_varint(cursor, drop.error_code, "subscribe drop has no error code")) return *error;
        message = drop;
    }
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

std::optional<EncodeError> encode_subscribe(const Subscribe& message, ByteWriter& output, const DecodeLimits& limits) {
    if (message.subscribe_id > kMaxVarint) return EncodeError::InvalidValue;
    if (const auto error = check_range(message.range)) return error;
    if (message.broadcast_path.size() > limits.max_string_length ||
        message.track_name.size() > limits.max_string_length) {
        return EncodeError::LimitExceeded;
    }
    ByteWriter body(limits.max_message_length);
    if (!write_varint(message.subscribe_id, body) || !write_string(message.broadcast_path, body) ||
        !write_string(message.track_name, body) || !write_range(message.range, body)) {
        return EncodeError::LimitExceeded;
    }
    return emit(std::nullopt, body, output);
}

std::optional<EncodeError> encode_subscribe_update(const SubscribeUpdate& message, ByteWriter& output,
                                                   const DecodeLimits& limits) {
    if (const auto error = check_range(message.range)) return error;
    ByteWriter body(limits.max_message_length);
    if (!write_range(message.range, body)) return EncodeError::LimitExceeded;
    return emit(std::nullopt, body, output);
}

std::optional<EncodeError> encode_subscribe_response(const SubscribeResponse& message, ByteWriter& output,
                                                     const DecodeLimits& limits) {
    ByteWriter body(limits.max_message_length);
    std::uint64_t type = 0;
    bool fits = true;

    if (const auto* ok = std::get_if<SubscribeOk>(&message)) {
        if (ok->group > kMaxVarint) return EncodeError::InvalidValue;
        type = kSubscribeTypeOk;
        fits = write_varint(ok->group, body);
    } else if (const auto* end = std::get_if<SubscribeEnd>(&message)) {
        if (end->group > kMaxVarint) return EncodeError::InvalidValue;
        type = kSubscribeTypeEnd;
        fits = write_varint(end->group, body);
    } else {
        const auto& drop = std::get<SubscribeDrop>(message);
        if (drop.group_start > kMaxVarint || drop.group_end > kMaxVarint || drop.error_code > kMaxVarint) {
            return EncodeError::InvalidValue;
        }
        type = kSubscribeTypeDrop;
        fits = write_varint(drop.group_start, body) && write_varint(drop.group_end, body) &&
               write_varint(drop.error_code, body);
    }
    if (!fits) return EncodeError::LimitExceeded;
    return emit(type, body, output);
}

}  // namespace moq::interop::wire::moqlite06
