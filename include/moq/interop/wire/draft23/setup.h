#pragma once

#include "moq/interop/wire/draft21/key_values.h"

namespace moq::interop::wire::draft23 {

struct SetupMessage {
    draft21::KeyValues options;
    std::uint64_t maximum_control_message_size{65535};
};

// What the peer actually sent in SETUP, before any option-specific validation. Used as conformance evidence,
// so a SETUP that decode_setup rejects (duplicates, an undersized MAX_CONTROL_MESSAGE_SIZE) is still observable.
struct SetupObservation {
    draft21::KeyValues options;
    std::uint64_t frame_length{0};
};

// Decodes only the SETUP frame and its generic Key-Value-Pairs; malformed framing is still an error.
DecodeResult<SetupObservation> decode_setup_observation(Cursor& input);

// Decodes framing and generic options, plus MAX_CONTROL_MESSAGE_SIZE.
// Other known options still require their context-specific validation.
bool encode_setup(const SetupMessage& message, ByteWriter& output);

DecodeResult<SetupMessage> decode_setup(Cursor& input);

}  // namespace moq::interop::wire::draft23
