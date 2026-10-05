#pragma once

#include "moq/interop/wire/draft22/location_filter.h"
#include "moq/interop/wire/draft22/shared.h"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft22 {

// draft-ietf-moq-transport-22 section 9.8 and 9.20. Identical to draft 21 except that
// LOCATION_FILTER (0x21) is a structured filter, not length-prefixed bytes.
struct PublishParameter {
    std::uint64_t type;
    std::variant<std::uint8_t, std::uint64_t, Location, Token, LocationFilter> value;
};

struct PublishMessage {
    std::uint64_t request_id;
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name;
    std::uint64_t track_alias;
    std::vector<PublishParameter> parameters;
    KeyValues track_properties;
};

// Structural decoding only; parameter scope and property semantics are validated by the
// draft-22 session layer.
DecodeResult<PublishMessage> decode_publish(Cursor& input);

}  // namespace moq::interop::wire::draft22
