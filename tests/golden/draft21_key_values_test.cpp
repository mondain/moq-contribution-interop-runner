#include "moq/interop/wire/draft21/key_values.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

TEST(Draft21KeyValues, DeltaTypeAndOddEvenValuesRoundTrip) {
    // draft-ietf-moq-transport-21 section 8.3, Figure 2.
    const auto wire = bytes({0x02, 0x05, 0x01, 0x02, 'a', 'b', 0x00, 0x00});
    Cursor input(wire);
    const auto decoded = decode_key_values(input, 3);
    ASSERT_TRUE(std::holds_alternative<KeyValues>(decoded));
    const auto& pairs = std::get<KeyValues>(decoded);
    ASSERT_EQ(pairs.size(), 3u);
    EXPECT_EQ(pairs[0].type, 2u);
    EXPECT_EQ(std::get<std::uint64_t>(pairs[0].value), 5u);
    EXPECT_EQ(pairs[1].type, 3u);
    EXPECT_EQ(std::get<std::vector<std::byte>>(pairs[1].value), bytes({'a', 'b'}));
    EXPECT_EQ(pairs[2].type, 3u);
    EXPECT_TRUE(std::get<std::vector<std::byte>>(pairs[2].value).empty());
    EXPECT_EQ(input.offset(), wire.size());
    ByteWriter output(32);
    EXPECT_FALSE(encode_key_values(pairs, output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21KeyValues, RejectsDeltaOverflowAtomically) {
    // draft-ietf-moq-transport-21 section 8.3 requires PROTOCOL_VIOLATION.
    const auto wire = bytes({0xff, 0xff, 0xff, 0xff, 0xff,
                             0xff, 0xff, 0xff, 0xff, 0x00, 0x01});
    Cursor input(wire);
    const auto result = decode_key_values(input, 2);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft21KeyValues, RejectsOversizedOddValue) {
    // The maximum odd-type value length is 65535 bytes.
    const auto wire = bytes({0x01, 0xc1, 0x00, 0x00});
    Cursor input(wire);
    const auto result = decode_key_values(input, 1);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft21KeyValues, IncompletePairNeedsMoreWithoutConsumption) {
    const auto wire = bytes({0x01, 0x02, 'a'});
    Cursor input(wire, 40);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_key_values(input, 1)));
    EXPECT_EQ(input.offset(), 40u);
}

TEST(Draft21KeyValues, EncodingRejectsOrderAndValueTypeAtomically) {
    ByteWriter output(20);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    EXPECT_EQ(encode_key_values({KeyValue{3, bytes({'a'})},
                                 KeyValue{2, std::uint64_t{1}}}, output),
              KeyValueEncodeError::InvalidValue);
    EXPECT_EQ(encode_key_values({KeyValue{2, bytes({'a'})}}, output),
              KeyValueEncodeError::InvalidValue);
    EXPECT_EQ(output.size(), 1u);
}

TEST(Draft21KeyValues, CapacityFailureDoesNotMutateOutput) {
    ByteWriter output(2);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    EXPECT_EQ(encode_key_values({KeyValue{2, std::uint64_t{5}}}, output),
              KeyValueEncodeError::OutputCapacity);
    EXPECT_EQ(output.size(), 1u);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
