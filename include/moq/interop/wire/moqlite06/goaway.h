#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// GOAWAY (draft 7.18): `Message Length (i)`, `New Session URI (s)`. An empty URI means no redirect.
struct GoawayMessage {
    std::string new_session_uri;
    bool operator==(const GoawayMessage&) const = default;
};

// Draft 7.18: the URI MUST NOT exceed 8,192 bytes and a receiver MAY reject a longer one from the length prefix
// alone. This is a draft rule, not a defensive limit.
inline constexpr std::size_t kMaxGoawayUri = 8192;

// A URI length prefix above kMaxGoawayUri (or above limits.max_string_length, when that is smaller) is
// LengthExceedsLimit, reported before the URI bytes are looked at; the detail names rule 7.18 for the draft limit.
// The cursor moves only on success.
DecodeResult<GoawayMessage> decode_goaway(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Writes nothing on failure. LimitExceeded: a URI above kMaxGoawayUri (the raw helpers can build an oversize one
// for a violation probe) or above limits.max_string_length.
std::optional<EncodeError> encode_goaway(const GoawayMessage& message, ByteWriter& output,
                                         const DecodeLimits& limits = kDefaultLimits);

}  // namespace moq::interop::wire::moqlite06
