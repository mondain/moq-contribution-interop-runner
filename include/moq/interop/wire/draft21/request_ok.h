#pragma once

#include "moq/interop/wire/cursor.h"

namespace moq::interop::wire::draft21 {

// Encodes a PUBLISH response with no Parameters and no Track Properties.
// Other legal REQUEST_OK forms require their own typed encoder.
bool encode_empty_publish_ok(ByteWriter& output);

}  // namespace moq::interop::wire::draft21
