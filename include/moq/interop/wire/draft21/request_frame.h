#pragma once

#include "moq/interop/wire/draft21/message_types.h"

#include <cstddef>
#include <vector>

namespace moq::interop::wire::draft21 {

struct RequestFrame {
    MessageTypeInfo type;
    std::vector<std::byte> body;
};

// Framing only: callers must decode and validate the selected message body.
DecodeResult<RequestFrame> decode_request_frame(Cursor& input,
                                                bool first_on_stream);

}  // namespace moq::interop::wire::draft21
