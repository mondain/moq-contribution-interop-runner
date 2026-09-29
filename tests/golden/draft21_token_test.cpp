#include "moq/interop/wire/draft21/token.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

TEST(Draft21Token, ParsesAndEncodesEveryAliasForm) {
    // draft-ietf-moq-transport-21 section 8.9, Figure 3.
    struct Case {
        std::vector<std::byte> wire;
        TokenAliasType alias_type;
        std::optional<std::uint64_t> alias;
        std::optional<std::uint64_t> token_type;
        std::vector<std::byte> value;
    };
    const std::vector<Case> cases{
        {bytes({0x00, 0x07}), TokenAliasType::Delete, 7, std::nullopt, {}},
        {bytes({0x01, 0x07, 0x00, 0xaa}), TokenAliasType::Register,
         7, 0, bytes({0xaa})},
        {bytes({0x02, 0x07}), TokenAliasType::UseAlias, 7, std::nullopt, {}},
        {bytes({0x03, 0x00, 0xbb}), TokenAliasType::UseValue,
         std::nullopt, 0, bytes({0xbb})},
    };
    for (const auto& item : cases) {
        const auto decoded = decode_token(item.wire);
        ASSERT_TRUE(std::holds_alternative<Token>(decoded));
        const auto& token = std::get<Token>(decoded);
        EXPECT_EQ(token.alias_type, item.alias_type);
        EXPECT_EQ(token.alias, item.alias);
        EXPECT_EQ(token.token_type, item.token_type);
        EXPECT_EQ(token.value, item.value);
        ByteWriter output(32);
        EXPECT_FALSE(encode_token(token, output).has_value());
        EXPECT_TRUE(std::ranges::equal(output.bytes(), item.wire));
    }
}

TEST(Draft21Token, MalformedBoundedTokensReportFormattingError) {
    // draft-ietf-moq-transport-21 section 8.9 requires this error for
    // undecodable Token structures.
    for (const auto& wire : {
             bytes({}), bytes({0x04}), bytes({0x01, 0x07}),
             bytes({0x02, 0x07, 0xff}), bytes({0x00})}) {
        const auto decoded = decode_token(wire, 20);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
        EXPECT_EQ(std::get<DecodeError>(decoded).code,
                  DecodeErrorCode::KeyValueFormattingError);
    }
}

TEST(Draft21Token, InvalidShapeAndCapacityDoNotMutateOutput) {
    ByteWriter output(3);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    EXPECT_EQ(encode_token(Token{TokenAliasType::UseAlias,
                                 7, std::nullopt, bytes({0xaa})}, output),
              TokenEncodeError::InvalidValue);
    EXPECT_EQ(encode_token(Token{TokenAliasType::Register,
                                 7, 0, bytes({0xaa})}, output),
              TokenEncodeError::OutputCapacity);
    EXPECT_EQ(output.size(), 1u);
    EXPECT_EQ(output.bytes()[0], std::byte{0xcc});
}

}  // namespace
}  // namespace moq::interop::wire::draft21
