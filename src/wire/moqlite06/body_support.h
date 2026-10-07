#pragma once

// Internal helpers shared by the moq-lite-06 message codecs. Not a public header.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06::detail {

inline DecodeError violation(std::size_t offset, const char* detail) {
    return DecodeError{DecodeErrorCode::ProtocolViolation, offset, detail};
}

// A field read from inside a body: running out of body is a malformed message, not a short input.
template <class T>
std::optional<DecodeError> body_error(const DecodeResult<T>& result, std::size_t offset, const char* detail) {
    if (std::holds_alternative<NeedMore>(result)) return violation(offset, detail);
    if (const auto* error = std::get_if<DecodeError>(&result)) return *error;
    return std::nullopt;
}

// Reads one varint from a body; the value is only valid when no error is returned.
inline std::optional<DecodeError> body_varint(Cursor& cursor, std::uint64_t& value, const char* detail) {
    const auto result = read_varint(cursor);
    if (const auto error = body_error(result, cursor.offset(), detail)) return error;
    value = std::get<std::uint64_t>(result);
    return std::nullopt;
}

// Reads one string from a body. The string limit is checked before the bytes are looked at, so an oversized
// length prefix is LengthExceedsLimit even when the body is also truncated.
inline std::optional<DecodeError> body_string(Cursor& cursor, std::string& value, const DecodeLimits& limits,
                                              const char* detail) {
    const auto result = read_string(cursor, limits.max_string_length);
    if (const auto error = body_error(result, cursor.offset(), detail)) return error;
    value = std::get<std::string>(result);
    return std::nullopt;
}

// Frames the next message from `working` into a body span; `working` is advanced past it.
struct Framed {
    std::span<const std::byte> body;
    std::size_t body_offset;
};

inline DecodeResult<Framed> frame(Cursor& working, const DecodeLimits& limits) {
    const auto framed = read_framed_body(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto body = std::get<std::span<const std::byte>>(framed);
    return Framed{body, working.offset() - body.size()};
}

// Writes [Type (i)] Message Length (i) body, or nothing at all.
inline std::optional<EncodeError> emit(std::optional<std::uint64_t> type, const ByteWriter& body,
                                       ByteWriter& output) {
    const auto bytes = body.bytes();
    const std::size_t length_size = varint_size(bytes.size());
    if (length_size == 0) return EncodeError::LimitExceeded;
    const std::size_t total = (type ? varint_size(*type) : 0) + length_size + bytes.size();
    if (total > output.remaining()) return EncodeError::OutputCapacity;
    if (type && !write_varint(*type, output)) return EncodeError::OutputCapacity;
    if (!write_framed_message(bytes, output)) return EncodeError::OutputCapacity;
    return std::nullopt;
}

}  // namespace moq::interop::wire::moqlite06::detail
