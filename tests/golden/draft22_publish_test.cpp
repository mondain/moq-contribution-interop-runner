#include "moq/interop/wire/draft22/publish.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <initializer_list>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft22 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

// A PUBLISH for track "media"/"test", alias 2, with `count` parameters whose encoded bytes
// are `parameters`, followed by `tail` (Track Properties). The Length(16) is computed.
std::vector<std::byte> publish_with(unsigned count,
                                    std::initializer_list<unsigned> parameters,
                                    std::initializer_list<unsigned> tail = {}) {
    std::vector<unsigned> body = {0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                                  0x04, 't', 'e', 's', 't', 0x02, count};
    body.insert(body.end(), parameters.begin(), parameters.end());
    body.insert(body.end(), tail.begin(), tail.end());
    std::vector<unsigned> wire = {0x1d, static_cast<unsigned>(body.size() >> 8),
                                  static_cast<unsigned>(body.size() & 0xffu)};
    wire.insert(wire.end(), body.begin(), body.end());
    std::vector<std::byte> result;
    for (const auto value : wire) result.push_back(static_cast<std::byte>(value));
    return result;
}

const LocationFilter& filter_of(const PublishMessage& publish, std::size_t index) {
    return std::get<LocationFilter>(publish.parameters.at(index).value);
}

TEST(Draft22Publish, ParsesMinimalPublisherTrack) {
    // draft-ietf-moq-transport-22 sections 8.7 and 9.8, Figure 13.
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

TEST(Draft22Publish, ParsesParametersAndTailPropertiesSeparately) {
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

TEST(Draft22Publish, ParsesHighBitPriorityAsOneByteNotVarint) {
    // draft-ietf-moq-transport-22 section 9.20.7: 0x20 is uint8.
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

TEST(Draft22Publish, ParsesAuthorizationTokenStructure) {
    // draft-ietf-moq-transport-22 sections 8.9 and 9.20.2.
    const auto wire = bytes({0x1d, 0x00, 0x14,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02,
                             0x01, 0x03, 0x03, 0x03, 0x00, 0xaa});
    Cursor input(wire);
    const auto decoded = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
    const auto& parameters = std::get<PublishMessage>(decoded).parameters;
    ASSERT_EQ(parameters.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<Token>(parameters[0].value));
    const auto& token = std::get<Token>(parameters[0].value);
    EXPECT_EQ(token.alias_type, TokenAliasType::UseValue);
    EXPECT_EQ(token.token_type, 0u);
    EXPECT_EQ(token.value, bytes({0xaa}));
}

TEST(Draft22Publish, MalformedTokenUsesFormattingError) {
    const auto wire = bytes({0x1d, 0x00, 0x12,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02,
                             0x01, 0x03, 0x01, 0x01});
    Cursor input(wire);
    const auto decoded = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code, DecodeErrorCode::KeyValueFormattingError);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft22Publish, RejectsOutOfScopeFillParameters) {
    // draft-ietf-moq-transport-22 sections 9.8 and 9.20.15: FILL_PARAMETERS (0x23) is not in
    // the PUBLISH parameter list.
    const auto wire = bytes({0x1d, 0x00, 0x11,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02,
                             0x01, 0x23, 0x00});
    Cursor input(wire);
    const auto decoded = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code, DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft22Publish, RejectsInvalidForwardValueAndDuplicate) {
    // draft-ietf-moq-transport-22 sections 9.20 and 9.20.18.
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
        EXPECT_EQ(std::get<DecodeError>(decoded).code, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft22Publish, RejectsEmptyNamespaceFieldAndExcessiveFieldCount) {
    // draft-ietf-moq-transport-22 section 8.7.
    for (const auto& wire : {
             bytes({0x1d, 0x00, 0x04, 0x01, 0x01, 0x00, 0x00}),
             bytes({0x1d, 0x00, 0x02, 0x01, 0x21})}) {
        Cursor input(wire);
        const auto result = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
        EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft22Publish, RejectsOversizedFullTrackName) {
    const auto wire = bytes({0x1d, 0x00, 0x05, 0x01, 0x00, 0x90, 0x01, 0x00});
    Cursor input(wire);
    const auto result = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft22Publish, PartialFrameIsNotConsumed) {
    const auto wire = bytes({0x1d, 0x00, 0x0f, 0x01});
    Cursor input(wire, 50);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_publish(input)));
    EXPECT_EQ(input.offset(), 50u);
}

TEST(Draft22Publish, RejectsIncompletePropertyInsideCompleteFrame) {
    const auto wire = bytes({0x1d, 0x00, 0x11,
                             0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02, 0x00,
                             0x01, 0x02});
    Cursor input(wire);
    const auto result = decode_publish(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft22Publish, ParsesEveryLocationFilterTypeInsideAPublish) {
    // draft-ietf-moq-transport-22 sections 9.8 and 9.20.9: LOCATION_FILTER may appear in PUBLISH.
    {
        const auto wire = publish_with(1, {0x21, 0x00});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& publish = std::get<PublishMessage>(decoded);
        ASSERT_EQ(publish.parameters.size(), 1u);
        EXPECT_EQ(publish.parameters[0].type, 0x21u);
        EXPECT_EQ(filter_of(publish, 0).type, LocationFilterType::None);
        EXPECT_EQ(input.offset(), wire.size());
    }
    {
        const auto wire = publish_with(1, {0x21, 0x01, 0x02});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& filter = filter_of(std::get<PublishMessage>(decoded), 0);
        EXPECT_EQ(filter.type, LocationFilterType::RelativeGroup);
        EXPECT_EQ(filter.start_group, 2u);
        EXPECT_EQ(input.offset(), wire.size());
    }
    {
        const auto wire = publish_with(1, {0x21, 0x02, 0x03, 0x04});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& filter = filter_of(std::get<PublishMessage>(decoded), 0);
        EXPECT_EQ(filter.type, LocationFilterType::Absolute);
        EXPECT_EQ(filter.start_group, 3u);
        EXPECT_EQ(filter.start_object, 4u);
    }
    {
        const auto wire = publish_with(1, {0x21, 0x03, 0x01, 0x00, 0x05});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& filter = filter_of(std::get<PublishMessage>(decoded), 0);
        EXPECT_EQ(filter.type, LocationFilterType::AbsoluteBounded);
        EXPECT_EQ(filter.end_group_delta, std::optional<std::uint64_t>{5});
    }
    {
        const auto wire = publish_with(1, {0x21, 0x04, 0x01, 0x00, 0x05, 0x09});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& filter = filter_of(std::get<PublishMessage>(decoded), 0);
        EXPECT_EQ(filter.type, LocationFilterType::AbsoluteRange);
        EXPECT_EQ(filter.end_object, std::optional<std::uint64_t>{9});
    }
    {
        const auto wire = publish_with(1, {0x21, 0x05});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        EXPECT_EQ(filter_of(std::get<PublishMessage>(decoded), 0).type,
                  LocationFilterType::NextObject);
        EXPECT_EQ(input.offset(), wire.size());
    }
}

TEST(Draft22Publish, FilterConsumesExactlyItsFieldsBeforeTheNextParameter) {
    // With no Length the parser has nothing but the Type to bound the filter.
    {
        // SUBSCRIBER_PRIORITY (0x20, uint8 200), then LOCATION_FILTER (delta 1) Next Object.
        const auto wire = publish_with(2, {0x20, 0xc8, 0x01, 0x05});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& publish = std::get<PublishMessage>(decoded);
        ASSERT_EQ(publish.parameters.size(), 2u);
        EXPECT_EQ(std::get<std::uint8_t>(publish.parameters[0].value), 200u);
        EXPECT_EQ(publish.parameters[1].type, 0x21u);
        EXPECT_EQ(filter_of(publish, 1).type, LocationFilterType::NextObject);
    }
    {
        // LOCATION_FILTER Next Object, then GROUP_ORDER (0x22, delta 1) Ascending.
        const auto wire = publish_with(2, {0x21, 0x05, 0x01, 0x01});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& publish = std::get<PublishMessage>(decoded);
        ASSERT_EQ(publish.parameters.size(), 2u);
        EXPECT_EQ(publish.parameters[1].type, 0x22u);
        EXPECT_EQ(std::get<std::uint8_t>(publish.parameters[1].value), 1u);
    }
    {
        // A four-field filter followed by Track Properties.
        const auto wire = publish_with(1, {0x21, 0x04, 0x01, 0x00, 0x05, 0x09},
                                       {0x03, 0x01, 'x'});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& publish = std::get<PublishMessage>(decoded);
        EXPECT_EQ(filter_of(publish, 0).end_object, std::optional<std::uint64_t>{9});
        ASSERT_EQ(publish.track_properties.size(), 1u);
        EXPECT_EQ(publish.track_properties[0].type, 3u);
    }
}

TEST(Draft22Publish, RejectsMalformedFiltersWithoutAdvancing) {
    // draft-ietf-moq-transport-22 section 9.20.9.
    const std::vector<std::vector<std::byte>> wires = {
        publish_with(1, {0x21, 0x06}),                      // unknown Location Filter Type
        publish_with(1, {0x21, 0x04, 0x01}),                // truncated inside a complete frame
        publish_with(1, {0x21, 0x03, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                         0x00, 0x01}),                      // StartGroup + EndGroupDelta overflows
        publish_with(1, {0x21}),                            // Type missing
    };
    for (const auto& wire : wires) {
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
        EXPECT_EQ(std::get<DecodeError>(decoded).code, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft22Publish, Draft21EncodingsOfTheSameBytesDivergeWhereTheTypeIsNotTheLength) {
    // Draft 21 sent a Length (21 02 00 00 was Next Object). The draft 22 Type 2 is Absolute, so
    // the same bytes are Absolute {0, 0}: a complete, valid filter that consumes all four bytes
    // and is not read as Next Object.
    {
        const auto wire = publish_with(1, {0x21, 0x02, 0x00, 0x00});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        const auto& publish = std::get<PublishMessage>(decoded);
        ASSERT_EQ(publish.parameters.size(), 1u);
        const auto& filter = filter_of(publish, 0);
        EXPECT_EQ(filter.type, LocationFilterType::Absolute);
        EXPECT_NE(filter.type, LocationFilterType::NextObject);
        EXPECT_EQ(filter.start_group, 0u);
        EXPECT_EQ(filter.start_object, 0u);
        EXPECT_TRUE(publish.track_properties.empty());
        EXPECT_EQ(input.offset(), wire.size());
    }
    // A draft 21 two-byte start (21 02 80 01 = Length 2, relative StartGroup 1) is a truncated
    // Absolute filter in draft 22: Type 2, StartGroup 1, StartObject missing.
    {
        const auto wire = publish_with(1, {0x21, 0x02, 0x80, 0x01});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
        EXPECT_EQ(std::get<DecodeError>(decoded).code, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
    // Where the two coincide the draft 22 reading is the same as the draft 21 reading:
    // 21 01 02 was a relative start of 2 in draft 21 and still is.
    {
        const auto wire = publish_with(1, {0x21, 0x01, 0x02});
        Cursor input(wire);
        const auto decoded = decode_publish(input);
        ASSERT_TRUE(std::holds_alternative<PublishMessage>(decoded));
        EXPECT_EQ(filter_of(std::get<PublishMessage>(decoded), 0).start_group, 2u);
    }
}

}  // namespace
}  // namespace moq::interop::wire::draft22
