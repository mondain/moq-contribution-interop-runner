#pragma once

#include "moq/interop/wire/cursor.h"

namespace moq::interop::wire::draft23 {

struct Frame {
    std::uint64_t type;
    std::vector<std::byte> body;
};

// Framing only; callers must validate the message type and body for the stream.
DecodeResult<Frame> decode_frame(Cursor& input, std::uint64_t maximum_body_size);

}  // namespace moq::interop::wire::draft23
