#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {

struct KeyValue {
    std::uint64_t type;
    std::variant<std::uint64_t, std::vector<std::byte>> value;
};

using KeyValues = std::vector<KeyValue>;

enum class KeyValueEncodeError {
    InvalidValue,
    OutputCapacity,
};

DecodeResult<KeyValues> decode_key_values(Cursor& input,
                                          std::uint64_t count);
std::optional<KeyValueEncodeError> encode_key_values(
    const KeyValues& values, ByteWriter& output);

}  // namespace moq::interop::wire::draft21
