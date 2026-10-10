#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// Draft 6.4: the total datagram body MUST NOT exceed 1200 bytes (a publisher must not send a larger one, a receiver
// must silently drop it). This is a draft rule, not a defensive limit.
inline constexpr std::size_t kMaxDatagramBody = 1200;

// DATAGRAM Body (draft 6.4): `Subscribe ID (i)`, `Group Sequence (i)`, `Timestamp (i)`, `Payload (b)`. There is no
// Message Length and no Type: the QUIC datagram boundary delimits the payload, which extends to the end.
struct DatagramBody {
    std::uint64_t subscribe_id = 0;
    std::uint64_t group_sequence = 0;
    std::uint64_t timestamp = 0;
    std::vector<std::byte> payload;
    bool operator==(const DatagramBody&) const = default;
};

// The input is the whole datagram. More than kMaxDatagramBody bytes is LengthExceedsLimit, reported before anything
// is read. A header cut short is a ProtocolViolation, never NeedMore: a datagram cannot grow. The cursor moves only
// on success, to the end of the datagram.
DecodeResult<DatagramBody> decode_datagram_body(Cursor& input);

// Writes nothing on failure. InvalidValue: a field above kMaxVarint. LimitExceeded: an encoding above
// kMaxDatagramBody. OutputCapacity: the writer has no room.
std::optional<EncodeError> encode_datagram_body(const DatagramBody& body, ByteWriter& output);

}  // namespace moq::interop::wire::moqlite06
