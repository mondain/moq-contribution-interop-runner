#pragma once

#include <cstdint>
#include <optional>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// PROBE (draft 7.17), sent on a Probe Stream by both sides: `Message Length (i)`, `Bitrate (i)`, `RTT (i)`.
// From the subscriber (stream opener) Bitrate is the target in bits per second; from the publisher it is the
// current estimate. 0 means unknown for both fields. The codec reports raw values.
struct ProbeMessage {
    std::uint64_t bitrate = 0;
    std::uint64_t rtt_ms = 0;
    bool operator==(const ProbeMessage&) const = default;
};

// Framing and body rules follow framing.h: a body shortfall inside a complete Message Length and trailing body
// bytes are ProtocolViolation; only short input is NeedMore. The cursor moves only on success.
DecodeResult<ProbeMessage> decode_probe(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Writes nothing on failure. InvalidValue: a value above kMaxVarint. OutputCapacity: no room in the writer.
std::optional<EncodeError> encode_probe(const ProbeMessage& message, ByteWriter& output,
                                        const DecodeLimits& limits = kDefaultLimits);

}  // namespace moq::interop::wire::moqlite06
