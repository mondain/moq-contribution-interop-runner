#pragma once

#include "moq/interop/wire/draft21/key_values.h"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {

struct Location {
    std::uint64_t group;
    std::uint64_t object;
};

struct PublishParameter {
    std::uint64_t type;
    std::variant<std::uint8_t, std::uint64_t, Location,
                 std::vector<std::byte>> value;
};

struct PublishMessage {
    std::uint64_t request_id;
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name;
    std::uint64_t track_alias;
    std::vector<PublishParameter> parameters;
    KeyValues track_properties;
};

// Structural decoding only; parameter scope and property semantics are
// validated by the draft-21 session layer.
DecodeResult<PublishMessage> decode_publish(Cursor& input);

}  // namespace moq::interop::wire::draft21
