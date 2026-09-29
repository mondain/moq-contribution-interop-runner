#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace moq::interop::wire::draft21 {

enum class TokenAliasType : std::uint64_t {
    Delete = 0,
    Register = 1,
    UseAlias = 2,
    UseValue = 3,
};

struct Token {
    TokenAliasType alias_type;
    std::optional<std::uint64_t> alias;
    std::optional<std::uint64_t> token_type;
    std::vector<std::byte> value;
};

enum class TokenEncodeError {
    InvalidValue,
    OutputCapacity,
};

// The span is exactly the length-delimited Token value from SETUP or a
// message parameter, not the containing control message body.
DecodeResult<Token> decode_token(std::span<const std::byte> value,
                                 std::size_t absolute_offset = 0);
std::optional<TokenEncodeError> encode_token(const Token& token,
                                             ByteWriter& output);

}  // namespace moq::interop::wire::draft21
