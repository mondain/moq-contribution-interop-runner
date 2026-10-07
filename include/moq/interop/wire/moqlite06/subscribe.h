#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// The fields shared by SUBSCRIBE and SUBSCRIBE_UPDATE (draft 7.9, 7.10), in wire order. The Subscriber Priority
// is "(8)": exactly one raw byte, NOT a varint. Values are raw wire values (0 means "absent" for the group and
// frame bounds); the codec checks only the frame_end/group_end coupling below.
struct SubscribeRange {
    std::uint8_t subscriber_priority = 0;
    std::uint64_t subscriber_max_age_ms = 0;
    std::uint64_t group_start = 0;  // 0 = no floor
    std::uint64_t group_end = 0;    // 0 = unbounded; else absolute group + 1
    std::uint64_t frame_start = 0;
    std::uint64_t frame_end = 0;    // 0 = whole group; else absolute frame + 1; MUST be 0 when group_end is 0
    bool operator==(const SubscribeRange&) const = default;
};

// `Message Length (i)`, `Subscribe ID (i)`, `Broadcast Path (s)`, `Track Name (s)`, then the range. No Type.
struct Subscribe {
    std::uint64_t subscribe_id = 0;
    std::string broadcast_path;
    std::string track_name;
    SubscribeRange range;
    bool operator==(const Subscribe&) const = default;
};

// `Message Length (i)`, then exactly the range: no Subscribe ID, no path. No Type.
struct SubscribeUpdate {
    SubscribeRange range;
    bool operator==(const SubscribeUpdate&) const = default;
};

// Type (i) = 0x0, then `Message Length (i)`, `Group (i)`.
struct SubscribeOk {
    std::uint64_t group = 0;
    bool operator==(const SubscribeOk&) const = default;
};

// Type (i) = 0x1, then `Message Length (i)`, `Group (i)` (exclusive end; 0 = the track ended with no groups).
struct SubscribeEnd {
    std::uint64_t group = 0;
    bool operator==(const SubscribeEnd&) const = default;
};

// Type (i) = 0x2, then `Message Length (i)`, `Group Start (i)`, `Group End (i)` (inclusive), `Error Code (i)`.
struct SubscribeDrop {
    std::uint64_t group_start = 0;
    std::uint64_t group_end = 0;
    std::uint64_t error_code = 0;
    bool operator==(const SubscribeDrop&) const = default;
};

// The publisher's messages on a Subscribe Stream.
using SubscribeResponse = std::variant<SubscribeOk, SubscribeEnd, SubscribeDrop>;

inline constexpr std::uint64_t kSubscribeTypeOk = 0x0;
inline constexpr std::uint64_t kSubscribeTypeEnd = 0x1;
inline constexpr std::uint64_t kSubscribeTypeDrop = 0x2;

// Framing and body rules follow framing.h: body shortfalls inside a complete Message Length and trailing body
// bytes are ProtocolViolation; only short input is NeedMore. The cursor moves only on success. Frame End != 0
// with Group End == 0 is a ProtocolViolation (draft 7.9). Nothing else is value-validated here: Subscribe ID
// reuse and range ordering are session-level judgements.
DecodeResult<Subscribe> decode_subscribe(Cursor& input, const DecodeLimits& limits = kDefaultLimits);
DecodeResult<SubscribeUpdate> decode_subscribe_update(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Reads Type (i) first, which comes BEFORE Message Length; an unknown Type is InvalidValue. This is a separate
// decoder from decode_announce_message even though both use Type 0x0 first: the two live on different streams
// (Subscribe Stream vs Announce Stream), and the Type spaces only coincide by numbering.
DecodeResult<SubscribeResponse> decode_subscribe_response(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Encoders write nothing on failure. InvalidValue: a value above kMaxVarint, or frame_end != 0 with
// group_end == 0. LimitExceeded: a string over limits.max_string_length or a body over limits.max_message_length.
// OutputCapacity: the writer has no room (for the whole message, Type included).
std::optional<EncodeError> encode_subscribe(const Subscribe& message, ByteWriter& output,
                                            const DecodeLimits& limits = kDefaultLimits);
std::optional<EncodeError> encode_subscribe_update(const SubscribeUpdate& message, ByteWriter& output,
                                                   const DecodeLimits& limits = kDefaultLimits);
std::optional<EncodeError> encode_subscribe_response(const SubscribeResponse& message, ByteWriter& output,
                                                     const DecodeLimits& limits = kDefaultLimits);

}  // namespace moq::interop::wire::moqlite06
