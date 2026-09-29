#pragma once

#include "moq/interop/wire/draft21/key_values.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace moq::interop::wire::draft21 {

struct PublishMessage {
    std::uint64_t request_id;
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name;
    std::uint64_t track_alias;
    KeyValues parameters;
    KeyValues track_properties;
};

// Structural decoding only; parameter scope and property semantics are
// validated by the draft-21 session layer.
DecodeResult<PublishMessage> decode_publish(Cursor& input);

}  // namespace moq::interop::wire::draft21
