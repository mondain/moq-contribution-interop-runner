#include "moq/interop/wire/draft21/setup.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

TEST(Draft21Setup, EmptySetupHasIndependentDraft21GoldenBytes) {
    // draft-ietf-moq-transport-21 sections 9.1 and 8.3.
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x00});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    EXPECT_TRUE(std::get<SetupMessage>(result).options.empty());
    EXPECT_EQ(input.offset(), wire.size());
    ByteWriter output(16);
    EXPECT_FALSE(encode_setup(SetupMessage{}, output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21Setup, ParsesNewRangeAndUpdateOptionsWithDeltaTypes) {
    // draft-ietf-moq-transport-21 sections 9.1.6 and 9.1.7.
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x04,
                             0x06, 0x02, 0x02, 0x03});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    const auto& options = std::get<SetupMessage>(result).options;
    ASSERT_EQ(options.size(), 2u);
    EXPECT_EQ(options[0].type, 6u);
    EXPECT_EQ(std::get<std::uint64_t>(options[0].value), 2u);
    EXPECT_EQ(options[1].type, 8u);
    EXPECT_EQ(std::get<std::uint64_t>(options[1].value), 3u);
    ByteWriter output(16);
    EXPECT_FALSE(encode_setup(std::get<SetupMessage>(result), output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21Setup, ParsesPathAndAuthorityAsOpaqueByteOptions) {
    // draft-ietf-moq-transport-21 sections 9.1.1 and 9.1.2.
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x11,
                             0x01, 0x04, '/', 'm', 'o', 'q',
                             0x04, 0x09, 'l', 'o', 'c', 'a', 'l',
                             'h', 'o', 's', 't'});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    const auto& options = std::get<SetupMessage>(result).options;
    ASSERT_EQ(options.size(), 2u);
    EXPECT_EQ(options[0].type, 1u);
    EXPECT_EQ(std::get<std::vector<std::byte>>(options[0].value),
              bytes({'/', 'm', 'o', 'q'}));
    EXPECT_EQ(options[1].type, 5u);
    EXPECT_EQ(std::get<std::vector<std::byte>>(options[1].value),
              bytes({'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't'}));
    ByteWriter output(32);
    EXPECT_FALSE(encode_setup(std::get<SetupMessage>(result), output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21Setup, UnknownDuplicateOptionsRemainDecodable) {
    // draft-ietf-moq-transport-21 section 9.1 requires receivers to allow
    // duplicate unknown options.
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x04,
                             0x20, 0x01, 0x00, 0x02});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    const auto& options = std::get<SetupMessage>(result).options;
    ASSERT_EQ(options.size(), 2u);
    EXPECT_EQ(options[0].type, 32u);
    EXPECT_EQ(options[1].type, 32u);
}

TEST(Draft21Setup, DuplicateKnownOptionIsRejected) {
    // draft-ietf-moq-transport-21 section 9.1 permits repetition only when
    // an individual known option defines it; MAX_FILTER_RANGES does not.
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x04,
                             0x06, 0x01, 0x00, 0x02});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft21Setup, PartialFrameNeedsMoreWithoutConsumingInput) {
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x04, 0x06, 0x01});
    Cursor input(wire, 100);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
    EXPECT_EQ(input.offset(), 100u);
}

TEST(Draft21Setup, TruncatedOptionWithinCompleteFrameIsViolation) {
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x01, 0x06});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
}

TEST(Draft21Setup, RejectsReservedLegacyMessageType) {
    // draft-ietf-moq-transport-21 Table 5 reserves 0x1e.
    const auto wire = bytes({0x1e, 0x00, 0x00});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
}

TEST(Draft21Setup, RejectsOversizedOddOptionValue) {
    // draft-ietf-moq-transport-21 section 8.3 caps an odd value at 65535.
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x05,
                             0x01, 0x80, 0x01, 0x00, 0x00});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
}

TEST(Draft21Setup, EncodeFailureDoesNotMutateOutput) {
    ByteWriter output(4);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    const auto before = std::vector<std::byte>(output.bytes().begin(),
                                               output.bytes().end());
    const SetupMessage setup{{SetupOption{
        6, std::uint64_t{2}}}};
    EXPECT_EQ(encode_setup(setup, output), SetupEncodeError::OutputCapacity);
    EXPECT_TRUE(std::ranges::equal(output.bytes(), before));
}

TEST(Draft21Setup, RejectsOutOfOrderEncodingAtomically) {
    ByteWriter output(64);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    const SetupMessage setup{{SetupOption{8, std::uint64_t{1}},
                              SetupOption{6, std::uint64_t{2}}}};
    EXPECT_EQ(encode_setup(setup, output), SetupEncodeError::InvalidValue);
    EXPECT_EQ(output.size(), 1u);
    EXPECT_EQ(output.bytes()[0], std::byte{0xcc});
}

}  // namespace
}  // namespace moq::interop::wire::draft21
