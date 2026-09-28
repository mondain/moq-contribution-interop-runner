#include "moq/interop/wire/draft18/objects.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft18 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

void append_vi64(std::vector<std::byte>& output, std::uint64_t value) {
    ByteWriter encoded(9);
    ASSERT_TRUE(write_vi64(value, encoded));
    output.insert(output.end(), encoded.bytes().begin(), encoded.bytes().end());
}

std::vector<std::byte> object_datagram(std::uint64_t type) {
    std::vector<std::byte> result;
    append_vi64(result, type);
    append_vi64(result, 17);
    append_vi64(result, 23);
    if ((type & 0x04u) == 0u) append_vi64(result, 29);
    if ((type & 0x08u) == 0u) result.push_back(std::byte{37});
    if ((type & 0x01u) != 0u) {
        append_vi64(result, 2);
        result.push_back(std::byte{0x3c});
        result.push_back(std::byte{0x07});
    }
    if ((type & 0x20u) != 0u) {
        append_vi64(result, 0);
    } else {
        result.push_back(std::byte{0xaa});
        result.push_back(std::byte{0xbb});
    }
    return result;
}

const ObjectEvent& require_object(const DatagramDecodeResult& result) {
    EXPECT_TRUE(std::holds_alternative<ObjectEvent>(result));
    return std::get<ObjectEvent>(result);
}

const DecodeError& require_error(const DatagramDecodeResult& result,
                                 DecodeErrorCode code) {
    EXPECT_TRUE(std::holds_alternative<DecodeError>(result));
    const auto& error = std::get<DecodeError>(result);
    EXPECT_EQ(error.code, code);
    EXPECT_FALSE(std::holds_alternative<DraftAmbiguity>(result));
    return error;
}

TEST(Draft18ObjectsTest, DecodesAllTwentyFourValidTypeForms) {
    constexpr std::array<std::uint64_t, 24> valid_types{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d,
    };
    for (const auto type : valid_types) {
        const auto encoded = object_datagram(type);
        const auto result = decode_datagram(encoded, {});
        const auto& object = require_object(result);
        EXPECT_EQ(object.datagram_type, type) << type;
        EXPECT_EQ(object.track_alias, 17u) << type;
        EXPECT_EQ(object.group_id, 23u) << type;
        EXPECT_EQ(object.object_id, (type & 0x04u) != 0u ? 0u : 29u) << type;
        EXPECT_EQ(object.publisher_priority, (type & 0x08u) != 0u
                                                 ? std::nullopt
                                                 : std::optional<std::uint8_t>{37}) << type;
        EXPECT_EQ(object.end_of_group, (type & 0x02u) != 0u) << type;
        EXPECT_EQ(object.properties.size(), (type & 0x01u) != 0u ? 1u : 0u) << type;
        EXPECT_EQ(object.status, (type & 0x20u) != 0u
                                     ? std::optional<std::uint64_t>{0}
                                     : std::nullopt) << type;
        EXPECT_EQ(object.payload_length, (type & 0x20u) != 0u ? 0u : 2u) << type;
        EXPECT_EQ(object.retained_payload.size(), object.payload_length) << type;
    }
}

TEST(Draft18ObjectsTest, PreservesOrderedPropertiesRawIntegerAndPayloadEvidence) {
    auto properties = bytes({
        0x3c, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
        0x3d, 0x02, 0xaa, 0xbb,
    });
    append_vi64(properties, 0x3f87);
    properties.push_back(std::byte{0x0b});
    auto encoded = bytes({0x01, 0x02, 0x03, 0x04, 0x05});
    append_vi64(encoded, properties.size());
    encoded.insert(encoded.end(), properties.begin(), properties.end());
    const auto payload = bytes({0x01, 0x02, 0x03});
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    Limits limits;
    limits.maximum_retained_payload_length = 2;
    const auto result = decode_datagram(encoded, limits);
    const auto& object = require_object(result);
    ASSERT_EQ(object.properties.size(), 3u);
    EXPECT_EQ(object.properties[0].type, 0x3cu);
    const auto& value = std::get<VarIntValue>(object.properties[0].value);
    EXPECT_EQ(value.value, 9u);
    EXPECT_TRUE(std::ranges::equal(
        value.raw_bytes,
        bytes({0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09})));
    EXPECT_EQ(object.properties[1].type, 0x79u);
    EXPECT_TRUE(std::ranges::equal(
        std::get<ByteValue>(object.properties[1].value).bytes,
        bytes({0xaa, 0xbb})));
    EXPECT_EQ(object.properties[2].type, 0x4000u);
    EXPECT_EQ(std::get<VarIntValue>(object.properties[2].value).value, 11u);
    EXPECT_EQ(object.payload_length, 3u);
    EXPECT_TRUE(std::ranges::equal(object.retained_payload, bytes({1, 2})));
}

TEST(Draft18ObjectsTest, AcceptsEmptyPayloadAndPreservesKnownAndUnknownStatuses) {
    for (const auto status : {0u, 3u, 4u, 9u}) {
        auto encoded = bytes({0x20, 0x01, 0x02, 0x03, 0x04, status});
        const auto result = decode_datagram(encoded, {});
        const auto& object = require_object(result);
        EXPECT_EQ(object.status, status);
        EXPECT_EQ(object.payload_length, 0u);
    }
    const auto empty = bytes({0x00, 0x01, 0x02, 0x03, 0x04});
    EXPECT_EQ(require_object(decode_datagram(empty, {})).payload_length, 0u);
}

TEST(Draft18ObjectsTest, RejectsInvalidObjectAndUnknownDatagramTypes) {
    for (const auto type : {0x22u, 0x23u, 0x26u, 0x27u, 0x2au, 0x2bu, 0x2eu,
                            0x2fu, 0x10u, 0x30u, 0x40u, 0x132b3e28u, 0x132b3e2au}) {
        std::vector<std::byte> encoded;
        append_vi64(encoded, type);
        const auto result = decode_datagram(encoded, {});
        const auto& error = require_error(result, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(error.offset, 0u) << type;
    }
}

TEST(Draft18ObjectsTest, RejectsMalformedPropertiesAndStatusCombinations) {
    struct ProtocolErrorCase {
        std::vector<std::byte> encoded;
        std::size_t error_offset;
    };
    const std::array cases{
        ProtocolErrorCase{bytes({0x01, 0x01, 0x02, 0x03, 0x04, 0x00}), 5},
        ProtocolErrorCase{
            bytes({0x21, 0x01, 0x02, 0x03, 0x04, 0x02, 0x3c, 0x01, 0x03}),
            8},
        ProtocolErrorCase{
            bytes({0x20, 0x01, 0x02, 0x03, 0x04, 0x00, 0xff}), 6},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto result = decode_datagram(cases[index].encoded, {});
        ASSERT_TRUE(std::holds_alternative<DecodeError>(result)) << index;
        EXPECT_EQ(std::get<DecodeError>(result).code,
                  DecodeErrorCode::ProtocolViolation) << index;
        EXPECT_EQ(std::get<DecodeError>(result).offset,
                  cases[index].error_offset) << index;
    }

    const auto malformed = bytes({0x01, 0x01, 0x02, 0x03, 0x04, 0x01, 0x3c});
    const auto malformed_result = decode_datagram(malformed, {});
    const auto& malformed_error = require_error(
        malformed_result, DecodeErrorCode::KeyValueFormattingError);
    EXPECT_EQ(malformed_error.offset, 7u);

    auto too_large = bytes({0x01, 0x01, 0x02, 0x03, 0x04, 0x02, 0x3c, 0x01});
    Limits limits;
    limits.maximum_object_properties_length = 1;
    const auto limited_result = decode_datagram(too_large, limits);
    const auto& error =
        require_error(limited_result, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(error.offset, 5u);

    const auto odd_value =
        bytes({0x01, 0x01, 0x02, 0x03, 0x04, 0x05,
               0x79, 0x03, 0xaa, 0xbb, 0xcc});
    Limits odd_limits;
    odd_limits.maximum_odd_value_length = 2;
    const auto odd_limited_result = decode_datagram(odd_value, odd_limits);
    const auto& odd_error = require_error(
        odd_limited_result, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(odd_error.offset, 6u);
}

TEST(Draft18ObjectsTest, ReportsTruncationAtEveryHeaderAndPropertyBoundary) {
    const auto complete =
        bytes({0x21, 0x01, 0x02, 0x03, 0x04, 0x02, 0x3c, 0x01, 0x00});
    constexpr std::array<DecodeErrorCode, 9> expected_codes{
        DecodeErrorCode::ProtocolViolation,
        DecodeErrorCode::ProtocolViolation,
        DecodeErrorCode::ProtocolViolation,
        DecodeErrorCode::ProtocolViolation,
        DecodeErrorCode::ProtocolViolation,
        DecodeErrorCode::ProtocolViolation,
        DecodeErrorCode::KeyValueFormattingError,
        DecodeErrorCode::KeyValueFormattingError,
        DecodeErrorCode::ProtocolViolation,
    };
    constexpr std::array<std::size_t, 9> expected_offsets{
        0, 1, 2, 3, 4, 5, 6, 6, 8,
    };
    for (std::size_t size = 0; size < complete.size(); ++size) {
        const auto result = decode_datagram(
            std::span<const std::byte>(complete).first(size), {});
        ASSERT_TRUE(std::holds_alternative<DecodeError>(result)) << size;
        EXPECT_EQ(std::get<DecodeError>(result).code, expected_codes[size]) << size;
        EXPECT_EQ(std::get<DecodeError>(result).offset, expected_offsets[size]) << size;
        EXPECT_FALSE(std::holds_alternative<DraftAmbiguity>(result)) << size;
    }
}

TEST(Draft18ObjectsTest, ClassifiesPartialVi64FieldsInAtomicDatagrams) {
    struct TruncatedCase {
        std::vector<std::byte> encoded;
        DecodeErrorCode code;
        std::size_t offset;
    };
    const std::array cases{
        TruncatedCase{bytes({0x80}), DecodeErrorCode::ProtocolViolation, 0},
        TruncatedCase{bytes({0x00, 0x80}), DecodeErrorCode::ProtocolViolation, 1},
        TruncatedCase{bytes({0x20, 0x01, 0x02, 0x03, 0x04, 0x80}),
                      DecodeErrorCode::ProtocolViolation, 5},
        TruncatedCase{bytes({0x01, 0x01, 0x02, 0x03, 0x04, 0x80}),
                      DecodeErrorCode::ProtocolViolation, 5},
        TruncatedCase{bytes({0x01, 0x01, 0x02, 0x03, 0x04, 0x01, 0x80}),
                      DecodeErrorCode::KeyValueFormattingError, 6},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto result = decode_datagram(cases[index].encoded, {});
        ASSERT_TRUE(std::holds_alternative<DecodeError>(result)) << index;
        const auto& error = std::get<DecodeError>(result);
        EXPECT_EQ(error.code, cases[index].code) << index;
        EXPECT_EQ(error.offset, cases[index].offset) << index;
    }
}

TEST(Draft18ObjectsTest, AcceptsNonMinimalTypeAndFields) {
    const auto encoded = bytes({
        0x80, 0x00,
        0x80, 0x01,
        0x80, 0x02,
        0x80, 0x03,
        0x04,
        0xaa,
    });
    const auto result = decode_datagram(encoded, {});
    const auto& object = require_object(result);
    EXPECT_EQ(object.datagram_type, 0u);
    EXPECT_EQ(object.track_alias, 1u);
    EXPECT_EQ(object.group_id, 2u);
    EXPECT_EQ(object.object_id, 3u);
    EXPECT_EQ(object.publisher_priority, 4u);
    EXPECT_EQ(object.payload_length, 1u);

    const auto status_encoded = bytes({
        0x80, 0x21,
        0x80, 0x01,
        0x80, 0x02,
        0x80, 0x03,
        0x04,
        0x80, 0x04,
        0x80, 0x3c,
        0x80, 0x01,
        0x80, 0x00,
    });
    const auto status_result = decode_datagram(status_encoded, {});
    const auto& status_object = require_object(status_result);
    ASSERT_EQ(status_object.properties.size(), 1u);
    EXPECT_EQ(status_object.properties[0].type, 0x3cu);
    EXPECT_EQ(std::get<VarIntValue>(status_object.properties[0].value).value, 1u);
    EXPECT_TRUE(std::ranges::equal(
        std::get<VarIntValue>(status_object.properties[0].value).raw_bytes,
        bytes({0x80, 0x01})));
    EXPECT_EQ(status_object.status, 0u);
}

TEST(Draft18ObjectsTest, DiscardsOnlyValidPaddingDatagrams) {
    const auto prefix = bytes({0xf0, 0x13, 0x2b, 0x3e, 0x29});
    for (const auto& body : {bytes({}), bytes({0}), bytes({0, 0, 0, 0})}) {
        auto encoded = prefix;
        encoded.insert(encoded.end(), body.begin(), body.end());
        const auto result = decode_datagram(encoded, {});
        EXPECT_TRUE(std::holds_alternative<DiscardedPaddingDatagram>(result));
    }
    struct BadPadding {
        std::vector<std::byte> body;
        std::size_t error_offset;
    };
    for (const auto& bad : {BadPadding{bytes({1}), 5},
                            BadPadding{bytes({0, 0, 2}), 7}}) {
        auto encoded = prefix;
        encoded.insert(encoded.end(), bad.body.begin(), bad.body.end());
        const auto result = decode_datagram(encoded, {});
        const auto& error = require_error(result, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(error.offset, bad.error_offset);
    }

    const auto nonminimal_type = bytes({0xf8, 0x00, 0x13, 0x2b, 0x3e, 0x29, 0x00});
    EXPECT_TRUE(std::holds_alternative<DiscardedPaddingDatagram>(
        decode_datagram(nonminimal_type, {})));
}

}  // namespace
}  // namespace moq::interop::wire::draft18
