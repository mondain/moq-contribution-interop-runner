#include "moq/interop/wire/draft22/location_filter.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft22 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

// Decodes a complete filter and checks that it consumed all of `wire`.
LocationFilter decoded(std::initializer_list<unsigned> wire) {
    const auto data = bytes(wire);
    Cursor input(data);
    const auto result = decode_location_filter(input);
    EXPECT_TRUE(std::holds_alternative<LocationFilter>(result));
    EXPECT_EQ(input.offset(), data.size());
    return std::holds_alternative<LocationFilter>(result) ? std::get<LocationFilter>(result)
                                                          : LocationFilter{};
}

void expect_violation(std::initializer_list<unsigned> wire) {
    const auto data = bytes(wire);
    Cursor input(data);
    const auto result = decode_location_filter(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

std::vector<std::byte> encoded(const LocationFilter& filter) {
    ByteWriter writer(64);
    const auto error = encode_location_filter(filter, writer);
    EXPECT_FALSE(error.has_value());
    const auto written = writer.bytes();
    return std::vector<std::byte>(written.begin(), written.end());
}

TEST(Draft22LocationFilter, DecodesNoFilterAndNextObject) {
    // draft-ietf-moq-transport-22 section 9.20.9: Types 0x00 and 0x05 carry no fields.
    const auto none = decoded({0x00});
    EXPECT_EQ(none.type, LocationFilterType::None);
    EXPECT_EQ(none.start_group, 0u);
    EXPECT_FALSE(none.end_group_delta.has_value());

    const auto next = decoded({0x05});
    EXPECT_EQ(next.type, LocationFilterType::NextObject);
    EXPECT_EQ(next.start_group, 0u);
    EXPECT_EQ(next.start_object, 0u);
    EXPECT_FALSE(next.end_group_delta.has_value());
    EXPECT_FALSE(next.end_object.has_value());
}

TEST(Draft22LocationFilter, DecodesRelativeStartGroup) {
    // Type 0x01: a relative StartGroup follows.
    const auto filter = decoded({0x01, 0x02});
    EXPECT_EQ(filter.type, LocationFilterType::RelativeGroup);
    EXPECT_EQ(filter.start_group, 2u);
    EXPECT_EQ(filter.start_object, 0u);
    EXPECT_FALSE(filter.end_group_delta.has_value());
}

TEST(Draft22LocationFilter, DecodesAbsoluteStartAndKeepsZeroZeroAbsolute) {
    // Type 0x02: StartGroup and StartObject. {0, 0} is an absolute start in draft 22; the
    // draft 21 two-zero encoding of Next Object no longer exists.
    const auto filter = decoded({0x02, 0x03, 0x04});
    EXPECT_EQ(filter.type, LocationFilterType::Absolute);
    EXPECT_EQ(filter.start_group, 3u);
    EXPECT_EQ(filter.start_object, 4u);

    const auto zero = decoded({0x02, 0x00, 0x00});
    EXPECT_EQ(zero.type, LocationFilterType::Absolute);
    EXPECT_NE(zero.type, LocationFilterType::NextObject);
}

TEST(Draft22LocationFilter, DecodesBoundedAndRangeForms) {
    // Types 0x03 (+EndGroupDelta) and 0x04 (+EndObject).
    const auto bounded = decoded({0x03, 0x01, 0x00, 0x05});
    EXPECT_EQ(bounded.type, LocationFilterType::AbsoluteBounded);
    EXPECT_EQ(bounded.start_group, 1u);
    EXPECT_EQ(bounded.start_object, 0u);
    ASSERT_TRUE(bounded.end_group_delta.has_value());
    EXPECT_EQ(*bounded.end_group_delta, 5u);
    EXPECT_FALSE(bounded.end_object.has_value());

    const auto range = decoded({0x04, 0x01, 0x00, 0x05, 0x09});
    EXPECT_EQ(range.type, LocationFilterType::AbsoluteRange);
    ASSERT_TRUE(range.end_group_delta.has_value());
    EXPECT_EQ(*range.end_group_delta, 5u);
    ASSERT_TRUE(range.end_object.has_value());
    EXPECT_EQ(*range.end_object, 9u);
}

TEST(Draft22LocationFilter, AcceptsNonMinimalVarints) {
    // draft-ietf-moq-transport-22 section 8.1: any encoding length that can represent the
    // value is valid; 0x8005 is 5.
    EXPECT_EQ(decoded({0x80, 0x05}).type, LocationFilterType::NextObject);
    const auto filter = decoded({0x02, 0x80, 0x03, 0x00});
    EXPECT_EQ(filter.type, LocationFilterType::Absolute);
    EXPECT_EQ(filter.start_group, 3u);
}

TEST(Draft22LocationFilter, DoesNotConsumeBytesAfterTheFilter) {
    const auto data = bytes({0x05, 0x99});
    Cursor input(data);
    const auto result = decode_location_filter(input);
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(result));
    EXPECT_EQ(input.offset(), 1u);
}

TEST(Draft22LocationFilter, RejectsUnknownTypes) {
    // "Any other Location Filter Type is a PROTOCOL_VIOLATION."
    expect_violation({0x06});
    expect_violation({0x7f});
    expect_violation({0x80, 0x06});  // non-minimal 6
    expect_violation({0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07});  // 9-byte 7
}

TEST(Draft22LocationFilter, RejectsTruncationAtEveryOffset) {
    const std::vector<unsigned> full = {0x04, 0x01, 0x00, 0x05, 0x09};
    for (std::size_t length = 0; length < full.size(); ++length) {
        const auto prefix = std::vector<unsigned>(full.begin(), full.begin() + static_cast<long>(length));
        const auto data = [&] {
            std::vector<std::byte> out;
            for (const auto value : prefix) out.push_back(static_cast<std::byte>(value));
            return out;
        }();
        Cursor input(data);
        const auto result = decode_location_filter(input);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(result)) << "length " << length;
        EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
    expect_violation({0x02, 0x80});  // second byte of a two-byte varint missing
}

TEST(Draft22LocationFilter, RejectsEndGroupOverflowAndAcceptsTheBoundary) {
    // "If StartGroup + EndGroupDelta exceeds 2^64 - 1, the endpoint MUST close the session
    // with a PROTOCOL_VIOLATION."
    expect_violation({0x03, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01});
    const auto boundary = decoded(
        {0x03, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00});
    EXPECT_EQ(boundary.start_group, std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(boundary.end_group_delta.has_value());
    EXPECT_EQ(*boundary.end_group_delta, 0u);
}

TEST(Draft22LocationFilter, EncodesEveryTypeAndRoundTrips) {
    const std::vector<std::pair<LocationFilter, std::vector<std::byte>>> cases = {
        {{LocationFilterType::None, 0, 0, std::nullopt, std::nullopt}, bytes({0x00})},
        {{LocationFilterType::RelativeGroup, 2, 0, std::nullopt, std::nullopt}, bytes({0x01, 0x02})},
        {{LocationFilterType::Absolute, 3, 4, std::nullopt, std::nullopt}, bytes({0x02, 0x03, 0x04})},
        {{LocationFilterType::AbsoluteBounded, 1, 0, 5, std::nullopt}, bytes({0x03, 0x01, 0x00, 0x05})},
        {{LocationFilterType::AbsoluteRange, 1, 0, 5, 9}, bytes({0x04, 0x01, 0x00, 0x05, 0x09})},
        {{LocationFilterType::NextObject, 0, 0, std::nullopt, std::nullopt}, bytes({0x05})},
    };
    for (const auto& [filter, wire] : cases) {
        EXPECT_EQ(encoded(filter), wire);
        Cursor input(wire);
        const auto result = decode_location_filter(input);
        ASSERT_TRUE(std::holds_alternative<LocationFilter>(result));
        const auto& back = std::get<LocationFilter>(result);
        EXPECT_EQ(back.type, filter.type);
        EXPECT_EQ(back.start_group, filter.start_group);
        EXPECT_EQ(back.start_object, filter.start_object);
        EXPECT_EQ(back.end_group_delta, filter.end_group_delta);
        EXPECT_EQ(back.end_object, filter.end_object);
        EXPECT_EQ(input.offset(), wire.size());
    }
}

TEST(Draft22LocationFilter, EncoderRejectsInconsistentFilters) {
    const auto invalid = [](const LocationFilter& filter) {
        ByteWriter writer(64);
        const auto error = encode_location_filter(filter, writer);
        EXPECT_TRUE(error.has_value());
        if (error) EXPECT_EQ(*error, LocationFilterEncodeError::InvalidValue);
        EXPECT_EQ(writer.size(), 0u);
    };
    invalid({LocationFilterType::NextObject, 1, 0, std::nullopt, std::nullopt});
    invalid({LocationFilterType::None, 0, 1, std::nullopt, std::nullopt});
    invalid({LocationFilterType::RelativeGroup, 1, 1, std::nullopt, std::nullopt});
    invalid({LocationFilterType::Absolute, 1, 1, 5, std::nullopt});
    invalid({LocationFilterType::AbsoluteBounded, 1, 0, std::nullopt, std::nullopt});
    invalid({LocationFilterType::AbsoluteBounded, 1, 0, 5, 9});
    invalid({LocationFilterType::AbsoluteRange, 1, 0, 5, std::nullopt});
    invalid({static_cast<LocationFilterType>(6), 0, 0, std::nullopt, std::nullopt});
    invalid({LocationFilterType::AbsoluteBounded, std::numeric_limits<std::uint64_t>::max(), 0, 1,
             std::nullopt});
}

TEST(Draft22LocationFilter, EncoderIsTransactionalOnOutputCapacity) {
    ByteWriter writer(1);
    const auto error = encode_location_filter(
        {LocationFilterType::AbsoluteRange, 1, 0, 5, 9}, writer);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, LocationFilterEncodeError::OutputCapacity);
    EXPECT_EQ(writer.size(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::draft22
