#include "moq/interop/wire/draft21/publish.h"

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

TEST(Draft21Publish, ParsesMinimalPublisherTrack) {
    // draft-ietf-moq-transport-21 sections 8.7 and 9.8, Figure 12.
    const auto wire = bytes({0x1d, 0x00, 0x0f,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02, 0x00});
    Cursor input(wire);
    const auto decoded = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
    const auto& publish = std::get<PublishMessage>(decoded);
    EXPECT_EQ(publish.request_id, 1u);
    ASSERT_EQ(publish.track_namespace.size(), 1u);
    EXPECT_EQ(publish.track_namespace[0], bytes({'m', 'e', 'd', 'i', 'a'}));
    EXPECT_EQ(publish.track_name, bytes({'t', 'e', 's', 't'}));
    EXPECT_EQ(publish.track_alias, 2u);
    EXPECT_TRUE(publish.parameters.empty());
    EXPECT_TRUE(publish.track_properties.empty());
    EXPECT_EQ(input.offset(), wire.size());
}

TEST(Draft21Publish, ParsesParametersAndTailPropertiesSeparately) {
    // Parameters have a count; properties occupy the remaining frame body.
    const auto wire = bytes({0x1d, 0x00, 0x14,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02,
                             0x01, 0x02, 0x05, 0x03, 0x01, 'x'});
    Cursor input(wire);
    const auto decoded = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
    const auto& publish = std::get<PublishMessage>(decoded);
    ASSERT_EQ(publish.parameters.size(), 1u);
    EXPECT_EQ(publish.parameters[0].type, 2u);
    EXPECT_EQ(std::get<std::uint64_t>(publish.parameters[0].value), 5u);
    ASSERT_EQ(publish.track_properties.size(), 1u);
    EXPECT_EQ(publish.track_properties[0].type, 3u);
}

TEST(Draft21Publish, ParsesHighBitPriorityAsOneByteNotVarint) {
    // draft-ietf-moq-transport-21 section 9.20.8: 0x20 is uint8.
    const auto wire = bytes({0x1d, 0x00, 0x11,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02,
                             0x01, 0x20, 0xc8});
    Cursor input(wire);
    const auto decoded = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
    const auto& parameters = std::get<PublishMessage>(decoded).parameters;
    ASSERT_EQ(parameters.size(), 1u);
    EXPECT_EQ(parameters[0].type, 0x20u);
    EXPECT_EQ(std::get<std::uint8_t>(parameters[0].value), 200u);
}

TEST(Draft21Publish, RejectsOutOfScopeFillParameters) {
    // draft-ietf-moq-transport-21 sections 9.20.1 and 9.20.16.
    const auto wire = bytes({0x1d, 0x00, 0x11,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02,
                             0x01, 0x23, 0x00});
    Cursor input(wire);
    const auto decoded = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft21Publish, RejectsInvalidForwardValueAndDuplicate) {
    // draft-ietf-moq-transport-21 sections 9.20 and 9.20.19.
    for (const auto& wire : {
             bytes({0x1d, 0x00, 0x11,
                    0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                    0x04, 't', 'e', 's', 't', 0x02,
                    0x01, 0x10, 0x02}),
             bytes({0x1d, 0x00, 0x13,
                    0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                    0x04, 't', 'e', 's', 't', 0x02,
                    0x02, 0x10, 0x01, 0x00, 0x01})}) {
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
        EXPECT_EQ(std::get<DecodeError>(decoded).code,
                  DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft21Publish, RejectsEmptyNamespaceFieldAndExcessiveFieldCount) {
    // draft-ietf-moq-transport-21 section 8.7.
    for (const auto& wire : {
             bytes({0x1d, 0x00, 0x04, 0x01, 0x01, 0x00, 0x00}),
             bytes({0x1d, 0x00, 0x02, 0x01, 0x21})}) {
        Cursor input(wire);
        const auto result = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
        EXPECT_EQ(std::get<DecodeError>(result).code,
                  DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft21Publish, RejectsOversizedFullTrackName) {
    const auto wire = bytes({0x1d, 0x00, 0x05,
                             0x01, 0x00, 0x90, 0x01, 0x00});
    Cursor input(wire);
    const auto result = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft21Publish, PartialFrameIsNotConsumed) {
    const auto wire = bytes({0x1d, 0x00, 0x0f, 0x01});
    Cursor input(wire, 50);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_publish(input)));
    EXPECT_EQ(input.offset(), 50u);
}

TEST(Draft21Publish, RejectsIncompletePropertyInsideCompleteFrame) {
    const auto wire = bytes({0x1d, 0x00, 0x11,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02, 0x00,
                             0x01, 0x02});
    Cursor input(wire);
    const auto result = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
