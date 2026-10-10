#include "moq/interop/wire/moqlite06/goaway.h"

#include <algorithm>

#include "body_support.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {

using namespace detail;

DecodeResult<GoawayMessage> decode_goaway(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    // The draft's 8,192-byte URI cap applies on top of the configured string limit; whichever is smaller wins.
    DecodeLimits uri_limits = limits;
    uri_limits.max_string_length = std::min(limits.max_string_length, kMaxGoawayUri);

    Cursor cursor(body, body_offset);
    GoawayMessage message;
    if (auto error = body_string(cursor, message.new_session_uri, uri_limits, "goaway uri is truncated")) {
        if (error->code == DecodeErrorCode::LengthExceedsLimit) {
            error->detail = "goaway new session uri is longer than the allowed length (draft 7.18)";
        }
        return *error;
    }
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

std::optional<EncodeError> encode_goaway(const GoawayMessage& message, ByteWriter& output,
                                         const DecodeLimits& limits) {
    if (message.new_session_uri.size() > kMaxGoawayUri || message.new_session_uri.size() > limits.max_string_length) {
        return EncodeError::LimitExceeded;
    }
    ByteWriter body(limits.max_message_length);
    if (!write_string(message.new_session_uri, body)) return EncodeError::LimitExceeded;
    return emit(std::nullopt, body, output);
}

}  // namespace moq::interop::wire::moqlite06
