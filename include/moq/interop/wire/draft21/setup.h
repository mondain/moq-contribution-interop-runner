#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {

struct SetupOption {
    std::uint64_t type;
    std::variant<std::uint64_t, std::vector<std::byte>> value;
};

struct SetupMessage {
    std::vector<SetupOption> options;
};

enum class SetupEncodeError {
    InvalidValue,
    OutputCapacity,
};

DecodeResult<SetupMessage> decode_setup(Cursor& input);
std::optional<SetupEncodeError> encode_setup(const SetupMessage& message,
                                             ByteWriter& output);

}  // namespace moq::interop::wire::draft21
