#include "moq/interop/wire/draft21/location_filter.h"

#include <gtest/gtest.h>

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

TEST(Draft21LocationFilter, DistinguishesNoneRelativeAndNextObject) {
    // draft-ietf-moq-transport-21 section 9.20.10.
    const auto none = decode_location_filter(bytes({}));
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(none));
    EXPECT_EQ(std::get<LocationFilter>(none).kind,
              LocationFilterKind::None);

    const auto relative = decode_location_filter(bytes({0x02}));
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(relative));
    EXPECT_EQ(std::get<LocationFilter>(relative).kind,
              LocationFilterKind::RelativeGroup);
    EXPECT_EQ(std::get<LocationFilter>(relative).start_group, 2u);

    const auto next = decode_location_filter(bytes({0x00, 0x00}));
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(next));
    EXPECT_EQ(std::get<LocationFilter>(next).kind,
              LocationFilterKind::NextObject);
}

TEST(Draft21LocationFilter, DecodesAbsoluteOpenAndBoundedForms) {
    // draft-ietf-moq-transport-21 section 9.20.10, 2-4 vi64 fields.
    const auto open = decode_location_filter(bytes({0x01, 0x02}));
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(open));
    const auto& open_value = std::get<LocationFilter>(open);
    EXPECT_EQ(open_value.kind, LocationFilterKind::Absolute);
    EXPECT_EQ(open_value.start_group, 1u);
    EXPECT_EQ(open_value.start_object, 2u);
    EXPECT_FALSE(open_value.end_group_delta.has_value());

    const auto group_end = decode_location_filter(bytes({0x01, 0x02, 0x03}));
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(group_end));
    EXPECT_EQ(std::get<LocationFilter>(group_end).end_group_delta, 3u);
    EXPECT_FALSE(std::get<LocationFilter>(group_end).end_object.has_value());

    const auto exact_end = decode_location_filter(bytes({0x01, 0x02, 0x03, 0x04}));
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(exact_end));
    EXPECT_EQ(std::get<LocationFilter>(exact_end).end_object, 4u);
}

TEST(Draft21LocationFilter, RejectsPartialVarintAndTooManyFields) {
    // A filter's declared length bounds its complete vi64 sequence.
    for (const auto payload : {bytes({0x80}),
                               bytes({0, 0, 0, 0, 0})}) {
        const auto decoded = decode_location_filter(payload);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
        EXPECT_EQ(std::get<DecodeError>(decoded).code,
                  DecodeErrorCode::ProtocolViolation);
    }
}

TEST(Draft21LocationFilter, RejectsEndGroupOverflowAtFullVi64Width) {
    // draft-ietf-moq-transport-21 sections 8.1 and 9.20.10: vi64 spans
    // all 64 bits, so the absolute end-group sum can overflow.
    const auto payload = bytes({0xff, 0xff, 0xff, 0xff, 0xff,
                                0xff, 0xff, 0xff, 0xff, 0x00, 0x01});
    const auto decoded = decode_location_filter(payload);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code,
              DecodeErrorCode::ProtocolViolation);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
