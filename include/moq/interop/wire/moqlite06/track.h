#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// TRACK (draft 7.11), the first message on a Track Stream: `Message Length (i)`, `Broadcast Path (s)`,
// `Track Name (s)`. The stream type (0x6) is written separately.
struct TrackRequest {
    std::string broadcast_path;
    std::string track_name;
    bool operator==(const TrackRequest&) const = default;
};

// TRACK_INFO (draft 7.12), the publisher's single reply: `Message Length (i)`, `Publisher Priority (8)` (one raw
// byte, never a varint), `Publisher Max Age (i)` in milliseconds, `Timescale (i)`. A zero timescale decodes: the
// codec reports raw values and the evaluator judges them.
struct TrackInfo {
    std::uint8_t publisher_priority = 0;
    std::uint64_t publisher_max_age_ms = 0;
    std::uint64_t timescale = 0;
    bool operator==(const TrackInfo&) const = default;
};

// Framing and body rules follow framing.h: a body shortfall inside a complete Message Length and trailing body
// bytes are ProtocolViolation; only short input is NeedMore. The cursor moves only on success.
DecodeResult<TrackRequest> decode_track_request(Cursor& input, const DecodeLimits& limits = kDefaultLimits);
DecodeResult<TrackInfo> decode_track_info(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Encoders write nothing on failure. InvalidValue: a varint above kMaxVarint. LimitExceeded: a string over
// limits.max_string_length or a body over limits.max_message_length. OutputCapacity: the writer has no room.
std::optional<EncodeError> encode_track_request(const TrackRequest& message, ByteWriter& output,
                                                const DecodeLimits& limits = kDefaultLimits);
std::optional<EncodeError> encode_track_info(const TrackInfo& message, ByteWriter& output,
                                             const DecodeLimits& limits = kDefaultLimits);

}  // namespace moq::interop::wire::moqlite06
