#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// FETCH (draft 7.16), the first message on a Fetch Stream: `Message Length (i)`, `Broadcast Path (s)`,
// `Track Name (s)`, `Subscriber Priority (8)` (one raw byte), `Group Sequence (i)`, `Frame Start (i)`,
// `Frame End (i)`. Frame End is the encoded index + 1; 0 means "to the end of the group". The reply has no header:
// bare FRAMEs until FIN.
struct FetchRequest {
    std::string broadcast_path;
    std::string track_name;
    std::uint8_t subscriber_priority = 0;
    std::uint64_t group_sequence = 0;
    std::uint64_t frame_start = 0;
    std::uint64_t frame_end = 0;
    bool operator==(const FetchRequest&) const = default;
};

// True when Frame End is set and names a frame (end - 1) below Frame Start. Equal bounds are a legal single
// frame. Draft 7.16 calls an inverted range a protocol violation; judging it is a session-level decision, so the
// decoder reports the values as they are.
bool fetch_range_inverted(const FetchRequest& request);

// Framing and body rules follow framing.h; the cursor moves only on success. An inverted range is NOT an error here.
DecodeResult<FetchRequest> decode_fetch_request(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Encoders write nothing on failure. InvalidValue: a varint above kMaxVarint, or an inverted range (the raw
// helpers can build one for a violation probe). LimitExceeded and OutputCapacity as for the other encoders.
std::optional<EncodeError> encode_fetch_request(const FetchRequest& message, ByteWriter& output,
                                                const DecodeLimits& limits = kDefaultLimits);

}  // namespace moq::interop::wire::moqlite06
