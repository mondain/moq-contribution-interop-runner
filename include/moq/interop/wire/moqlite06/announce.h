#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// ANNOUNCE_REQUEST (draft 7.4): `Message Length (i)`, `Broadcast Path Prefix (s)`.
struct AnnounceRequest {
    std::string prefix;
    bool operator==(const AnnounceRequest&) const = default;
};

// ANNOUNCE_OK (draft 7.5): `Message Length (i)`, `Hop ID (i)`, `Active Count (i)`.
struct AnnounceOk {
    std::uint64_t hop_id = 0;
    std::uint64_t active_count = 0;
    bool operator==(const AnnounceOk&) const = default;
};

// The route metadata shared by ANNOUNCE_START and ANNOUNCE_UPDATE (draft 7.6, 7.8): `Hop Count (i)`,
// `Hop ID (i) ...`, `Warm Route Cost (i)`, `Cold Route Cost (i)`. The Hop Count is implied by hop_ids.size().
// Costs are the raw wire values; saturation is routing logic, not codec.
struct RouteMetadata {
    std::vector<std::uint64_t> hop_ids;
    std::uint64_t warm_cost = 0;
    std::uint64_t cold_cost = 0;
    bool operator==(const RouteMetadata&) const = default;
};

// Type (i) = 0x0, then `Message Length (i)`, `Route Prefix Suffix (s)`, route metadata.
struct AnnounceStart {
    std::string suffix;
    RouteMetadata route;
    bool operator==(const AnnounceStart&) const = default;
};

// Type (i) = 0x1, then `Message Length (i)`, `Announce ID (i)`.
struct AnnounceEnd {
    std::uint64_t announce_id = 0;
    bool operator==(const AnnounceEnd&) const = default;
};

// Type (i) = 0x2, then `Message Length (i)`, `Announce ID (i)`, route metadata.
struct AnnounceUpdate {
    std::uint64_t announce_id = 0;
    RouteMetadata route;
    bool operator==(const AnnounceUpdate&) const = default;
};

// The publisher's messages after ANNOUNCE_OK on an Announce Stream.
using AnnounceMessage = std::variant<AnnounceStart, AnnounceEnd, AnnounceUpdate>;

inline constexpr std::uint64_t kAnnounceTypeStart = 0x0;
inline constexpr std::uint64_t kAnnounceTypeEnd = 0x1;
inline constexpr std::uint64_t kAnnounceTypeUpdate = 0x2;

// Framing and body rules follow framing.h: body shortfalls inside a complete Message Length and trailing body
// bytes are ProtocolViolation; only short input is NeedMore. The cursor moves only on success.
DecodeResult<AnnounceRequest> decode_announce_request(Cursor& input, const DecodeLimits& limits = kDefaultLimits);
DecodeResult<AnnounceOk> decode_announce_ok(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Reads Type (i) first, which comes BEFORE Message Length; an unknown Type is InvalidValue. A Hop Count above
// limits.max_hops is LengthExceedsLimit before anything is sized by it; a Hop Count that disagrees with the
// body and a non-zero Hop ID appearing twice (draft 7.6) are ProtocolViolation. Duplicate zeros are legal.
DecodeResult<AnnounceMessage> decode_announce_message(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Encoders write nothing on failure. InvalidValue: a value above kMaxVarint or a duplicate non-zero Hop ID.
// LimitExceeded: a string over limits.max_string_length, more than limits.max_hops hops or a body over
// limits.max_message_length. OutputCapacity: the writer has no room.
std::optional<EncodeError> encode_announce_request(const AnnounceRequest& message, ByteWriter& output,
                                                   const DecodeLimits& limits = kDefaultLimits);
std::optional<EncodeError> encode_announce_ok(const AnnounceOk& message, ByteWriter& output,
                                              const DecodeLimits& limits = kDefaultLimits);
std::optional<EncodeError> encode_announce_message(const AnnounceMessage& message, ByteWriter& output,
                                                   const DecodeLimits& limits = kDefaultLimits);

}  // namespace moq::interop::wire::moqlite06
