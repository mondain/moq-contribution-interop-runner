#include "moq/interop/wire/moqlite06/datagram.h"

#include <span>

#include "body_support.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {

DecodeResult<DatagramBody> decode_datagram_body(Cursor& input) {
    if (input.remaining() > kMaxDatagramBody) {
        return DecodeError{DecodeErrorCode::LengthExceedsLimit, input.offset(),
                           "datagram body is longer than 1200 bytes (draft 6.4)"};
    }
    Cursor working = input;
    DatagramBody body;
    // The whole datagram is here, so a field that runs out is malformed, not incomplete.
    if (const auto error = detail::body_varint(working, body.subscribe_id, "datagram has no subscribe id")) {
        return *error;
    }
    if (const auto error = detail::body_varint(working, body.group_sequence, "datagram has no group sequence")) {
        return *error;
    }
    if (const auto error = detail::body_varint(working, body.timestamp, "datagram has no timestamp")) return *error;
    const auto rest = read_bytes(working, working.remaining());
    if (const auto error = detail::body_error(rest, working.offset(), "datagram payload is truncated")) return *error;
    const auto payload = std::get<std::span<const std::byte>>(rest);
    body.payload.assign(payload.begin(), payload.end());
    input = working;
    return body;
}

std::optional<EncodeError> encode_datagram_body(const DatagramBody& body, ByteWriter& output) {
    if (body.subscribe_id > kMaxVarint || body.group_sequence > kMaxVarint || body.timestamp > kMaxVarint) {
        return EncodeError::InvalidValue;
    }
    ByteWriter staged(kMaxDatagramBody);
    if (!write_varint(body.subscribe_id, staged) || !write_varint(body.group_sequence, staged) ||
        !write_varint(body.timestamp, staged) || !staged.append_bytes(body.payload)) {
        return EncodeError::LimitExceeded;
    }
    const auto bytes = staged.bytes();
    if (bytes.size() > output.remaining()) return EncodeError::OutputCapacity;
    if (!output.append_bytes(bytes)) return EncodeError::OutputCapacity;
    return std::nullopt;
}

}  // namespace moq::interop::wire::moqlite06
