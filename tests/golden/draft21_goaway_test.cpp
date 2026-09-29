#include "moq/interop/wire/draft21/goaway.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

TEST(Draft21Goaway, ClientZeroUriRoundTrips) {
    // draft-ietf-moq-transport-21 section 9.2, Figure 6.
    const auto wire = bytes({0x10, 0x00, 0x02, 0x00, 0x05});
    Cursor input(wire);
    const auto result = decode_goaway(input, true);
    ASSERT_TRUE(std::holds_alternative<GoawayMessage>(result));
    EXPECT_TRUE(std::get<GoawayMessage>(result).new_session_uri.empty());
    EXPECT_EQ(std::get<GoawayMessage>(result).timeout_ms, 5u);
    EXPECT_EQ(input.offset(), wire.size());
    ByteWriter output(16);
    EXPECT_FALSE(encode_goaway(std::get<GoawayMessage>(result), true,
                               output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21Goaway, ServerUriRoundTrips) {
    // draft-ietf-moq-transport-21 section 9.2 permits a server URI.
    const auto wire = bytes({0x10, 0x00, 0x04, 0x02, 'm', 'q', 0x01});
    Cursor input(wire);
    const auto result = decode_goaway(input, false);
    ASSERT_TRUE(std::holds_alternative<GoawayMessage>(result));
    EXPECT_EQ(std::get<GoawayMessage>(result).new_session_uri,
              bytes({'m', 'q'}));
    ByteWriter output(16);
    EXPECT_FALSE(encode_goaway(std::get<GoawayMessage>(result), false,
                               output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21Goaway, ClientNonzeroUriIsProtocolViolation) {
    const auto wire = bytes({0x10, 0x00, 0x04, 0x02, 'm', 'q', 0x01});
    Cursor input(wire);
    const auto result = decode_goaway(input, true);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
    ByteWriter output(16);
    EXPECT_EQ(encode_goaway(GoawayMessage{bytes({'m'}), 0}, true, output),
              GoawayEncodeError::InvalidValue);
    EXPECT_EQ(output.size(), 0u);
}

TEST(Draft21Goaway, RejectsLengthMismatchAndOversizedUri) {
    // The control frame's 16-bit length must match its body exactly.
    for (const auto& wire : {
             bytes({0x10, 0x00, 0x03, 0x00, 0x05, 0xff}),
             bytes({0x10, 0x00, 0x01, 0x00}),
             bytes({0x10, 0x00, 0x03, 0xc0, 0x20, 0x01})}) {
        Cursor input(wire);
        const auto result = decode_goaway(input, false);
        EXPECT_TRUE(std::holds_alternative<DecodeError>(result));
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft21Goaway, PartialFrameAndInvalidTypeAreAtomic) {
    const auto partial = bytes({0x10, 0x00, 0x02, 0x00});
    Cursor input(partial, 100);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_goaway(input, true)));
    EXPECT_EQ(input.offset(), 100u);
    const auto wrong_type = bytes({0x1e, 0x00, 0x02, 0x00, 0x05});
    Cursor wrong(wrong_type);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_goaway(wrong, true)));
    EXPECT_EQ(wrong.offset(), 0u);
}

TEST(Draft21Goaway, EncodeCapacityFailureIsAtomic) {
    ByteWriter output(4);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    EXPECT_EQ(encode_goaway(GoawayMessage{{}, 5}, true, output),
              GoawayEncodeError::OutputCapacity);
    EXPECT_EQ(output.size(), 1u);
    EXPECT_EQ(output.bytes()[0], std::byte{0xcc});
}

}  // namespace
}  // namespace moq::interop::wire::draft21
