#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "moq/interop/wire/cursor.h"

namespace moq::interop::wire::moqlite06 {

// Encoder failure causes shared by the typed moq-lite-06 encoders.
enum class EncodeError { InvalidValue, OutputCapacity, LimitExceeded };

// The draft is silent on size limits (its section 10.4 only warns about resource exhaustion); these defaults
// follow moq.dev and are overridable per call.
struct DecodeLimits {
    std::size_t max_message_length = 1u << 20;
    std::size_t max_string_length = 1u << 16;
    std::size_t max_parameters = 64;
    std::size_t max_hops = 1024;
};
inline constexpr DecodeLimits kDefaultLimits{};

// Reads `Message Length (i)` and returns the body bytes (a span into the input), advancing the cursor past the
// whole message on success. NeedMore when the body is not fully available; LengthExceedsLimit above
// limits.max_message_length (checked before the body is looked at). The cursor moves only on success.
DecodeResult<std::span<const std::byte>> read_framed_body(Cursor& input,
                                                          const DecodeLimits& limits = kDefaultLimits);

// Writes `Message Length (i)` then the body. Returns false and writes nothing when it does not fit or the
// body length exceeds kMaxVarint.
bool write_framed_message(std::span<const std::byte> body, ByteWriter& output);

// For typed decoders: after the fields are read from `body_cursor`, remaining() != 0 is a ProtocolViolation
// (draft 7.1: unexpected length).
std::optional<DecodeError> expect_body_consumed(const Cursor& body_cursor);

// STREAM_TYPE (draft 7.2).
enum class BidiStreamType : std::uint64_t {
    Announce = 0x1,
    Subscribe = 0x2,
    Fetch = 0x3,
    Probe = 0x4,
    Goaway = 0x5,
    Track = 0x6,
};
enum class UniStreamType : std::uint64_t { Group = 0x0, Setup = 0x1 };

// Returns the raw value; what to do with an unknown type is the caller's decision (draft 7.2: reset the
// stream, not a fatal error).
DecodeResult<std::uint64_t> read_stream_type(Cursor& input);
std::optional<BidiStreamType> as_bidi_stream_type(std::uint64_t value);
std::optional<UniStreamType> as_uni_stream_type(std::uint64_t value);
bool write_stream_type(std::uint64_t value, ByteWriter& output);

}  // namespace moq::interop::wire::moqlite06
