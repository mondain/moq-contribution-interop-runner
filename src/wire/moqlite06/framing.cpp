#include "moq/interop/wire/moqlite06/framing.h"

#include <variant>

#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {

DecodeResult<std::span<const std::byte>> read_framed_body(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto length_result = read_varint(working);
    if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;

    const auto declared = std::get<std::uint64_t>(length_result);
    if (declared > limits.max_message_length) {
        return DecodeError{DecodeErrorCode::LengthExceedsLimit, input.offset(),
                           "message length exceeds configured limit"};
    }

    const auto body_result = read_bytes(working, static_cast<std::size_t>(declared));
    if (const auto* need = std::get_if<NeedMore>(&body_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&body_result)) return *error;

    input = working;
    return std::get<std::span<const std::byte>>(body_result);
}

bool write_framed_message(std::span<const std::byte> body, ByteWriter& output) {
    const auto prefix = varint_size(body.size());
    if (prefix == 0 || prefix > output.remaining()) return false;
    if (body.size() > output.remaining() - prefix) return false;
    if (!write_varint(body.size(), output)) return false;
    return output.append_bytes(body);
}

std::optional<DecodeError> expect_body_consumed(const Cursor& body_cursor) {
    if (body_cursor.remaining() == 0) return std::nullopt;
    return DecodeError{DecodeErrorCode::ProtocolViolation, body_cursor.offset(),
                       "message body has trailing bytes after its fields"};
}

DecodeResult<std::uint64_t> read_stream_type(Cursor& input) { return read_varint(input); }

std::optional<BidiStreamType> as_bidi_stream_type(std::uint64_t value) {
    switch (value) {
        case 0x1: return BidiStreamType::Announce;
        case 0x2: return BidiStreamType::Subscribe;
        case 0x3: return BidiStreamType::Fetch;
        case 0x4: return BidiStreamType::Probe;
        case 0x5: return BidiStreamType::Goaway;
        case 0x6: return BidiStreamType::Track;
        default: return std::nullopt;
    }
}

std::optional<UniStreamType> as_uni_stream_type(std::uint64_t value) {
    switch (value) {
        case 0x0: return UniStreamType::Group;
        case 0x1: return UniStreamType::Setup;
        default: return std::nullopt;
    }
}

bool write_stream_type(std::uint64_t value, ByteWriter& output) { return write_varint(value, output); }

}  // namespace moq::interop::wire::moqlite06
