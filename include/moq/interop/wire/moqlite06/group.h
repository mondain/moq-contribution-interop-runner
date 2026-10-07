#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// GROUP (draft 7.19): `Message Length (i)`, `Subscribe ID (i)`, `Group Sequence (i)`, `Frame Start (i)`. It
// follows STREAM_TYPE 0x0 on a Group Stream, which the caller reads first. No Type of its own.
struct GroupHeader {
    std::uint64_t subscribe_id = 0;
    std::uint64_t group_sequence = 0;
    std::uint64_t frame_start = 0;  // index of the first FRAME on this stream; 0 = from the group's beginning
    bool operator==(const GroupHeader&) const = default;
};

// FRAME (draft 7.20): `Timestamp Delta (i)` (zigzag), `Message Length (i)`, `Payload (b)`. There is no outer
// Message Length around the whole frame; the Message Length covers the payload only. The first frame of a
// group is delta-encoded from 0, so its delta is the absolute timestamp.
struct Frame {
    std::int64_t timestamp_delta = 0;
    std::vector<std::byte> payload;
    bool operator==(const Frame&) const = default;
};

// Zigzag (draft 7.20), computed in unsigned arithmetic so no signed shift or negation can overflow.
// encode: (v << 1) ^ (v >> 63) with an arithmetic right shift; decode: (u >> 1) ^ -(u & 1).
constexpr std::uint64_t zigzag_encode(std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    return (bits << 1) ^ (std::uint64_t{0} - (bits >> 63));
}
constexpr std::int64_t zigzag_decode(std::uint64_t value) {
    return static_cast<std::int64_t>((value >> 1) ^ (std::uint64_t{0} - (value & 1)));
}

// Largest/smallest deltas whose zigzag still fits a QUIC varint (2^62-1): 2^61-1 and -2^61.
inline constexpr std::int64_t kMaxFrameDelta = (std::int64_t{1} << 61) - 1;
inline constexpr std::int64_t kMinFrameDelta = -(std::int64_t{1} << 61);

// GROUP framing and body rules follow framing.h: a body shortfall inside a complete Message Length or trailing
// body bytes are ProtocolViolation; only short input is NeedMore. The cursor moves only on success.
DecodeResult<GroupHeader> decode_group_header(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Used on a Group Stream (after the GROUP message) or a Fetch stream. An incomplete frame is NeedMore so a
// streaming reader can wait for more bytes; a payload length above limits.max_message_length is
// LengthExceedsLimit, reported from the length alone before any payload is looked at or allocated. The cursor
// moves only on success.
DecodeResult<Frame> decode_frame(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Encoders write nothing on failure. InvalidValue: a value above kMaxVarint (for a FRAME, a timestamp delta
// whose zigzag exceeds kMaxVarint). LimitExceeded: a body/payload over limits.max_message_length.
// OutputCapacity: the writer has no room for the whole message.
std::optional<EncodeError> encode_group_header(const GroupHeader& header, ByteWriter& output,
                                               const DecodeLimits& limits = kDefaultLimits);
std::optional<EncodeError> encode_frame(const Frame& frame, ByteWriter& output,
                                        const DecodeLimits& limits = kDefaultLimits);

}  // namespace moq::interop::wire::moqlite06
