#include "moq/interop/wire/moqlite06/probe.h"

#include "body_support.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {

using namespace detail;

DecodeResult<ProbeMessage> decode_probe(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    ProbeMessage message;
    if (const auto error = body_varint(cursor, message.bitrate, "probe has no bitrate")) return *error;
    if (const auto error = body_varint(cursor, message.rtt_ms, "probe has no rtt")) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

std::optional<EncodeError> encode_probe(const ProbeMessage& message, ByteWriter& output,
                                        const DecodeLimits& limits) {
    if (message.bitrate > kMaxVarint || message.rtt_ms > kMaxVarint) return EncodeError::InvalidValue;
    ByteWriter body(limits.max_message_length);
    if (!write_varint(message.bitrate, body) || !write_varint(message.rtt_ms, body)) return EncodeError::LimitExceeded;
    return emit(std::nullopt, body, output);
}

}  // namespace moq::interop::wire::moqlite06
