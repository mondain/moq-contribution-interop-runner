#pragma once

#include "moq/interop/wire/draft21/key_values.h"

namespace moq::interop::wire::draft23 {

struct SetupMessage {
    draft21::KeyValues options;
    std::uint64_t maximum_control_message_size{65535};
};

// Decodes framing and generic options, plus MAX_CONTROL_MESSAGE_SIZE.
// Other known options still require their context-specific validation.
DecodeResult<SetupMessage> decode_setup(Cursor& input);

}  // namespace moq::interop::wire::draft23
