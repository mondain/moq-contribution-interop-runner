#include "moq/interop/wire/draft21/token.h"

#include <variant>

namespace moq::interop::wire::draft21 {
namespace {

DecodeError malformed(std::size_t offset) {
    return {DecodeErrorCode::KeyValueFormattingError, offset,
            "malformed draft-21 authorization Token"};
}

DecodeResult<std::uint64_t> required_vi64(Cursor& input) {
    const auto result = read_vi64(input);
    if (std::holds_alternative<NeedMore>(result)) {
        return malformed(input.offset());
    }
    return result;
}

}  // namespace

DecodeResult<Token> decode_token(std::span<const std::byte> value,
                                 std::size_t absolute_offset) {
    Cursor input(value, absolute_offset);
    const auto alias_type = required_vi64(input);
    if (const auto* error = std::get_if<DecodeError>(&alias_type)) return *error;
    const auto raw_type = std::get<std::uint64_t>(alias_type);
    if (raw_type > 3) return malformed(absolute_offset);

    Token token{static_cast<TokenAliasType>(raw_type), std::nullopt,
                std::nullopt, {}};
    if (token.alias_type != TokenAliasType::UseValue) {
        const auto alias = required_vi64(input);
        if (const auto* error = std::get_if<DecodeError>(&alias)) return *error;
        token.alias = std::get<std::uint64_t>(alias);
    }
    if (token.alias_type == TokenAliasType::Register ||
        token.alias_type == TokenAliasType::UseValue) {
        const auto type = required_vi64(input);
        if (const auto* error = std::get_if<DecodeError>(&type)) return *error;
        token.token_type = std::get<std::uint64_t>(type);
        const auto bytes = read_bytes(input, input.remaining());
        if (const auto* error = std::get_if<DecodeError>(&bytes)) return *error;
        const auto data = std::get<std::span<const std::byte>>(bytes);
        token.value.assign(data.begin(), data.end());
    } else if (input.remaining() != 0) {
        return malformed(input.offset());
    }
    return token;
}

std::optional<TokenEncodeError> encode_token(const Token& token,
                                             ByteWriter& output) {
    const bool has_alias = token.alias.has_value();
    const bool has_type = token.token_type.has_value();
    switch (token.alias_type) {
        case TokenAliasType::Delete:
        case TokenAliasType::UseAlias:
            if (!has_alias || has_type || !token.value.empty()) {
                return TokenEncodeError::InvalidValue;
            }
            break;
        case TokenAliasType::Register:
            if (!has_alias || !has_type) return TokenEncodeError::InvalidValue;
            break;
        case TokenAliasType::UseValue:
            if (has_alias || !has_type) return TokenEncodeError::InvalidValue;
            break;
        default:
            return TokenEncodeError::InvalidValue;
    }

    ByteWriter staged(output.remaining());
    if (!write_vi64(static_cast<std::uint64_t>(token.alias_type), staged) ||
        (has_alias && !write_vi64(*token.alias, staged)) ||
        (has_type && !write_vi64(*token.token_type, staged)) ||
        !staged.append_bytes(token.value)) {
        return TokenEncodeError::OutputCapacity;
    }
    if (!output.append_bytes(staged.bytes())) {
        return TokenEncodeError::OutputCapacity;
    }
    return std::nullopt;
}

}  // namespace moq::interop::wire::draft21
