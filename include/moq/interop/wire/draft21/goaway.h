#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace moq::interop::wire::draft21 {

struct GoawayMessage {
    std::vector<std::byte> new_session_uri;
    std::uint64_t timeout_ms;
};

enum class GoawayEncodeError {
    InvalidValue,
    OutputCapacity,
};

DecodeResult<GoawayMessage> decode_goaway(Cursor& input,
                                          bool received_from_client);
std::optional<GoawayEncodeError> encode_goaway(
    const GoawayMessage& message, bool sending_as_client, ByteWriter& output);

}  // namespace moq::interop::wire::draft21
