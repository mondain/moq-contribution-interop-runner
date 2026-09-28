#include "moq/interop/wire/draft18/objects.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
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

std::vector<std::byte> subgroup_header(std::uint64_t type) {
    std::vector<std::byte> result;
    append_vi64(result, type);
    append_vi64(result, 17);
    append_vi64(result, 23);
    if ((type & 0x06u) == 0x04u) append_vi64(result, 29);
    if ((type & 0x20u) == 0u) result.push_back(std::byte{37});
    return result;
}

void append_subgroup_object(std::vector<std::byte>& output,
                            std::uint64_t delta,
                            std::span<const std::byte> properties,
                            std::uint64_t payload_length,
                            std::optional<std::uint64_t> status,
                            std::span<const std::byte> payload) {
    append_vi64(output, delta);
    if (!properties.empty()) {
        append_vi64(output, properties.size());
        output.insert(output.end(), properties.begin(), properties.end());
    }
    append_vi64(output, payload_length);
    if (status) append_vi64(output, *status);
    output.insert(output.end(), payload.begin(), payload.end());
}

std::vector<std::byte> fetch_header(std::uint64_t request_id = 7) {
    std::vector<std::byte> result;
    append_vi64(result, 0x05);
    append_vi64(result, request_id);
    return result;
}

void append_safe_fetch_object(std::vector<std::byte>& output,
                              std::uint64_t flags,
                              std::uint64_t group = 2,
                              std::uint64_t object = 4,
                              std::uint8_t priority = 5) {
    append_vi64(output, flags);
    if ((flags & 0x08u) != 0u) append_vi64(output, group);
    if ((flags & 0x40u) == 0u && (flags & 0x03u) == 3u) {
        append_vi64(output, 3);
    }
    if ((flags & 0x04u) != 0u) append_vi64(output, object);
    if ((flags & 0x10u) != 0u) output.push_back(static_cast<std::byte>(priority));
    if ((flags & 0x20u) != 0u) append_vi64(output, 0);
    append_vi64(output, 0);
}

FetchGroupOrderResolver ascending_order() {
    return [](std::uint64_t) {
        return std::optional{FetchGroupOrder::Ascending};
    };
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

struct AccumulatedSubgroupResult {
    std::optional<SubgroupHeader> header;
    std::vector<ObjectEvent> objects;
    std::vector<DecoderObservation> observations;
    std::optional<DecodeError> error;
    std::optional<std::uint64_t> final_object_id;
    bool clean_fin{false};
    bool local_api_misuse{false};
};

void accumulate(const SubgroupPushResult& source,
                AccumulatedSubgroupResult& target) {
    if (source.header) {
        EXPECT_FALSE(target.header.has_value());
        target.header = source.header;
    }
    target.objects.insert(target.objects.end(), source.objects.begin(),
                          source.objects.end());
    target.observations.insert(target.observations.end(),
                               source.observations.begin(),
                               source.observations.end());
    if (source.error) {
        EXPECT_FALSE(target.error.has_value());
        target.error = source.error;
    }
    if (source.final_object_id) {
        EXPECT_FALSE(target.final_object_id.has_value());
        target.final_object_id = source.final_object_id;
    }
    if (source.clean_fin) {
        EXPECT_FALSE(target.clean_fin);
    }
    target.clean_fin = target.clean_fin || source.clean_fin;
    target.local_api_misuse =
        target.local_api_misuse || source.local_api_misuse;
}

void expect_same_properties(const KeyValuePairs& actual,
                            const KeyValuePairs& expected,
                            std::size_t split) {
    ASSERT_EQ(actual.size(), expected.size()) << split;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        EXPECT_EQ(actual[index].type, expected[index].type) << split << ':' << index;
        ASSERT_EQ(actual[index].value.index(), expected[index].value.index())
            << split << ':' << index;
        if (const auto* integer = std::get_if<VarIntValue>(&actual[index].value)) {
            const auto& expected_integer =
                std::get<VarIntValue>(expected[index].value);
            EXPECT_EQ(integer->value, expected_integer.value) << split << ':' << index;
            EXPECT_TRUE(std::ranges::equal(integer->raw_bytes,
                                           expected_integer.raw_bytes))
                << split << ':' << index;
        } else {
            EXPECT_TRUE(std::ranges::equal(
                std::get<ByteValue>(actual[index].value).bytes,
                std::get<ByteValue>(expected[index].value).bytes))
                << split << ':' << index;
        }
    }
}

void expect_same_event(const ObjectEvent& actual, const ObjectEvent& expected,
                       std::size_t split) {
    EXPECT_EQ(actual.datagram_type, expected.datagram_type) << split;
    EXPECT_EQ(actual.track_alias, expected.track_alias) << split;
    EXPECT_EQ(actual.group_id, expected.group_id) << split;
    EXPECT_EQ(actual.object_id, expected.object_id) << split;
    EXPECT_EQ(actual.publisher_priority, expected.publisher_priority) << split;
    EXPECT_EQ(actual.end_of_group, expected.end_of_group) << split;
    expect_same_properties(actual.properties, expected.properties, split);
    EXPECT_EQ(actual.status, expected.status) << split;
    EXPECT_EQ(actual.payload_length, expected.payload_length) << split;
    EXPECT_TRUE(std::ranges::equal(actual.retained_payload,
                                   expected.retained_payload)) << split;
    EXPECT_EQ(actual.forwarding_preference, expected.forwarding_preference) << split;
    EXPECT_EQ(actual.subgroup_id, expected.subgroup_id) << split;
    EXPECT_EQ(actual.subgroup_header_type, expected.subgroup_header_type) << split;
    EXPECT_EQ(actual.first_object, expected.first_object) << split;
    EXPECT_EQ(actual.priority_inherited, expected.priority_inherited) << split;
    EXPECT_EQ(actual.stream_offset, expected.stream_offset) << split;
    EXPECT_EQ(actual.stream_end_offset, expected.stream_end_offset) << split;
    EXPECT_EQ(actual.request_id, expected.request_id) << split;
    EXPECT_EQ(actual.serialization_flags, expected.serialization_flags) << split;
}

struct AccumulatedFetchResult {
    std::optional<FetchHeader> header;
    std::vector<FetchEvent> events;
    std::vector<FetchDecoderObservation> observations;
    std::optional<DecodeError> error;
    bool clean_fin{false};
    bool local_api_misuse{false};
};

void accumulate(const FetchPushResult& source, AccumulatedFetchResult& target) {
    if (source.header) {
        EXPECT_FALSE(target.header.has_value());
        target.header = source.header;
    }
    target.events.insert(target.events.end(), source.events.begin(),
                         source.events.end());
    target.observations.insert(target.observations.end(),
                               source.observations.begin(),
                               source.observations.end());
    if (source.error) {
        EXPECT_FALSE(target.error.has_value());
        target.error = source.error;
    }
    target.clean_fin = target.clean_fin || source.clean_fin;
    target.local_api_misuse =
        target.local_api_misuse || source.local_api_misuse;
}

void expect_same_fetch_result(const AccumulatedFetchResult& actual,
                              const AccumulatedFetchResult& expected,
                              std::size_t split) {
    ASSERT_EQ(actual.header.has_value(), expected.header.has_value()) << split;
    if (actual.header) {
        EXPECT_EQ(actual.header->raw_type, expected.header->raw_type) << split;
        EXPECT_EQ(actual.header->request_id, expected.header->request_id) << split;
        EXPECT_EQ(actual.header->stream_offset, expected.header->stream_offset)
            << split;
        EXPECT_EQ(actual.header->stream_end_offset,
                  expected.header->stream_end_offset) << split;
    }
    ASSERT_EQ(actual.events.size(), expected.events.size()) << split;
    for (std::size_t index = 0; index < actual.events.size(); ++index) {
        ASSERT_EQ(actual.events[index].index(), expected.events[index].index())
            << split << ':' << index;
        if (const auto* object = std::get_if<ObjectEvent>(&actual.events[index])) {
            expect_same_event(*object,
                              std::get<ObjectEvent>(expected.events[index]),
                              split);
        } else {
            const auto& range = std::get<FetchRangeEvent>(actual.events[index]);
            const auto& expected_range =
                std::get<FetchRangeEvent>(expected.events[index]);
            EXPECT_EQ(range.kind, expected_range.kind) << split << ':' << index;
            EXPECT_EQ(range.serialization_flags,
                      expected_range.serialization_flags) << split << ':' << index;
            EXPECT_EQ(range.group_id, expected_range.group_id) << split << ':' << index;
            EXPECT_EQ(range.object_id, expected_range.object_id) << split << ':' << index;
            EXPECT_EQ(range.stream_offset, expected_range.stream_offset)
                << split << ':' << index;
            EXPECT_EQ(range.stream_end_offset, expected_range.stream_end_offset)
                << split << ':' << index;
        }
    }
    ASSERT_EQ(actual.observations.size(), expected.observations.size()) << split;
    for (std::size_t index = 0; index < actual.observations.size(); ++index) {
        EXPECT_EQ(actual.observations[index].kind,
                  expected.observations[index].kind) << split << ':' << index;
        EXPECT_EQ(actual.observations[index].phase,
                  expected.observations[index].phase) << split << ':' << index;
        EXPECT_EQ(actual.observations[index].offset,
                  expected.observations[index].offset) << split << ':' << index;
        EXPECT_EQ(actual.observations[index].detail,
                  expected.observations[index].detail) << split << ':' << index;
    }
    ASSERT_EQ(actual.error.has_value(), expected.error.has_value()) << split;
    if (actual.error) {
        EXPECT_EQ(actual.error->code, expected.error->code) << split;
        EXPECT_EQ(actual.error->offset, expected.error->offset) << split;
        EXPECT_EQ(actual.error->detail, expected.error->detail) << split;
    }
    EXPECT_EQ(actual.clean_fin, expected.clean_fin) << split;
    EXPECT_EQ(actual.local_api_misuse, expected.local_api_misuse) << split;
}

void expect_same_result(const AccumulatedSubgroupResult& actual,
                        const AccumulatedSubgroupResult& expected,
                        std::size_t split) {
    ASSERT_EQ(actual.header.has_value(), expected.header.has_value()) << split;
    if (actual.header) {
        EXPECT_EQ(actual.header->raw_type, expected.header->raw_type) << split;
        EXPECT_EQ(actual.header->track_alias, expected.header->track_alias) << split;
        EXPECT_EQ(actual.header->group_id, expected.header->group_id) << split;
        EXPECT_EQ(actual.header->subgroup_id, expected.header->subgroup_id) << split;
        EXPECT_EQ(actual.header->publisher_priority,
                  expected.header->publisher_priority) << split;
        EXPECT_EQ(actual.header->properties_present,
                  expected.header->properties_present) << split;
        EXPECT_EQ(actual.header->end_of_group, expected.header->end_of_group) << split;
        EXPECT_EQ(actual.header->first_object, expected.header->first_object) << split;
        EXPECT_EQ(actual.header->priority_inherited,
                  expected.header->priority_inherited) << split;
        EXPECT_EQ(actual.header->stream_offset, expected.header->stream_offset) << split;
        EXPECT_EQ(actual.header->stream_end_offset,
                  expected.header->stream_end_offset) << split;
    }
    ASSERT_EQ(actual.objects.size(), expected.objects.size()) << split;
    for (std::size_t index = 0; index < actual.objects.size(); ++index) {
        expect_same_event(actual.objects[index], expected.objects[index], split);
    }
    ASSERT_EQ(actual.observations.size(), expected.observations.size()) << split;
    for (std::size_t index = 0; index < actual.observations.size(); ++index) {
        EXPECT_EQ(actual.observations[index].kind,
                  expected.observations[index].kind) << split << ':' << index;
        EXPECT_EQ(actual.observations[index].phase,
                  expected.observations[index].phase) << split << ':' << index;
        EXPECT_EQ(actual.observations[index].offset,
                  expected.observations[index].offset) << split << ':' << index;
        EXPECT_EQ(actual.observations[index].detail,
                  expected.observations[index].detail) << split << ':' << index;
    }
    ASSERT_EQ(actual.error.has_value(), expected.error.has_value()) << split;
    if (actual.error) {
        EXPECT_EQ(actual.error->code, expected.error->code) << split;
        EXPECT_EQ(actual.error->offset, expected.error->offset) << split;
        EXPECT_EQ(actual.error->detail, expected.error->detail) << split;
    }
    EXPECT_EQ(actual.final_object_id, expected.final_object_id) << split;
    EXPECT_EQ(actual.clean_fin, expected.clean_fin) << split;
    EXPECT_EQ(actual.local_api_misuse, expected.local_api_misuse) << split;
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
        ASSERT_TRUE(std::holds_alternative<DiscardedPaddingDatagram>(result));
        EXPECT_FALSE(std::get<DiscardedPaddingDatagram>(result)
                         .first_nonzero_offset.has_value());
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
        ASSERT_TRUE(std::holds_alternative<DiscardedPaddingDatagram>(result));
        EXPECT_EQ(std::get<DiscardedPaddingDatagram>(result).first_nonzero_offset,
                  bad.error_offset);
    }

    const auto nonminimal_type = bytes({0xf8, 0x00, 0x13, 0x2b, 0x3e, 0x29, 0x00});
    EXPECT_TRUE(std::holds_alternative<DiscardedPaddingDatagram>(
        decode_datagram(nonminimal_type, {})));
}

TEST(Draft18SubgroupTest, DecodesAllLegalHeadersAndRejectsReservedModes) {
    for (const auto properties : {0u, 1u}) {
        for (const auto mode : {0u, 1u, 2u}) {
            for (const auto end_of_group : {0u, 1u}) {
                for (const auto default_priority : {0u, 1u}) {
                    for (const auto first_object : {0u, 1u}) {
                        const auto type = 0x10u | properties | (mode << 1u) |
                                          (end_of_group << 3u) |
                                          (default_priority << 5u) |
                                          (first_object << 6u);
                        SubgroupDecoder decoder;
                        const auto encoded = subgroup_header(type);
                        const auto result = decoder.push(encoded, false);
                        ASSERT_TRUE(result.header.has_value()) << type;
                        EXPECT_EQ(result.header->raw_type, type) << type;
                        EXPECT_EQ(result.header->track_alias, 17u) << type;
                        EXPECT_EQ(result.header->group_id, 23u) << type;
                        EXPECT_EQ(result.header->subgroup_id,
                                  mode == 0u ? std::optional<std::uint64_t>{0}
                                  : mode == 2u ? std::optional<std::uint64_t>{29}
                                               : std::nullopt) << type;
                        EXPECT_EQ(result.header->publisher_priority,
                                  default_priority != 0u
                                      ? std::nullopt
                                      : std::optional<std::uint8_t>{37}) << type;
                        EXPECT_EQ(result.header->properties_present,
                                  properties != 0u) << type;
                        EXPECT_EQ(result.header->end_of_group,
                                  end_of_group != 0u) << type;
                        EXPECT_EQ(result.header->first_object,
                                  first_object != 0u) << type;
                        EXPECT_EQ(result.header->priority_inherited,
                                  default_priority != 0u) << type;
                        EXPECT_FALSE(result.error.has_value()) << type;
                    }
                }
            }
        }
    }

    for (const auto high : {0x10u, 0x30u, 0x50u, 0x70u}) {
        for (const auto low : {0x06u, 0x07u, 0x0eu, 0x0fu}) {
            SubgroupDecoder decoder;
            const auto encoded = bytes({high | low});
            const auto result = decoder.push(encoded, false);
            ASSERT_TRUE(result.error.has_value()) << (high | low);
            EXPECT_EQ(result.error->code, DecodeErrorCode::ProtocolViolation);
            EXPECT_EQ(result.error->offset, 0u);
        }
    }

    for (const auto type : {0x05u, 0x0fu, 0x80u, 0x132b3e28u}) {
        SubgroupDecoder decoder;
        std::vector<std::byte> encoded;
        append_vi64(encoded, type);
        const auto result = decoder.push(encoded, false);
        ASSERT_TRUE(result.error.has_value()) << type;
        EXPECT_EQ(result.error->code, DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(result.error->offset, 0u);
    }
}

TEST(Draft18SubgroupTest, PreservesExampleHeadersAndDecodesObjectIds) {
    auto encoded = subgroup_header(0x14);
    const auto first_offset = encoded.size();
    append_subgroup_object(encoded, 0, {}, 1, std::nullopt, bytes({0xaa}));
    const auto second_offset = encoded.size();
    append_subgroup_object(encoded, 0, {}, 1, std::nullopt, bytes({0xbb}));
    SubgroupDecoder decoder;
    const auto result = decoder.push(encoded, true);
    ASSERT_TRUE(result.header.has_value());
    EXPECT_EQ(result.header->raw_type, 0x14u);
    EXPECT_EQ(result.header->subgroup_id, 29u);
    ASSERT_EQ(result.objects.size(), 2u);
    EXPECT_EQ(result.objects[0].object_id, 0u);
    EXPECT_EQ(result.objects[1].object_id, 1u);
    EXPECT_EQ(result.objects[0].stream_offset, first_offset);
    EXPECT_EQ(result.objects[1].stream_offset, second_offset);
    EXPECT_EQ(result.objects[1].stream_end_offset, encoded.size());
    EXPECT_EQ(result.objects[0].forwarding_preference,
              ObjectForwardingPreference::Subgroup);
    EXPECT_EQ(result.objects[0].subgroup_id, 29u);
    EXPECT_EQ(result.objects[0].subgroup_header_type, 0x14u);
    EXPECT_TRUE(result.clean_fin);
}

TEST(Draft18SubgroupTest, DerivesModeOneSubgroupAndHandlesHeaderOnlyFin) {
    auto encoded = subgroup_header(0x12);
    append_subgroup_object(encoded, 41, {}, 0, 0, {});
    SubgroupDecoder decoder;
    const auto result = decoder.push(encoded, true);
    ASSERT_TRUE(result.header.has_value());
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(result.objects[0].subgroup_id, 41u);

    SubgroupDecoder no_object;
    const auto header = subgroup_header(0x12);
    const auto no_object_result = no_object.push(header, true);
    ASSERT_EQ(no_object_result.observations.size(), 1u);
    EXPECT_EQ(no_object_result.observations[0].kind,
              DecoderObservationKind::DraftAmbiguity);
    EXPECT_EQ(no_object_result.observations[0].offset, header.size());
    EXPECT_TRUE(no_object_result.clean_fin);
}

TEST(Draft18SubgroupTest, PreservesPropertiesStatusAndRecommendationEvidence) {
    auto properties = bytes({0x3c, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x09, 0x3d, 0x02, 0xaa, 0xbb});
    auto encoded = subgroup_header(0x35);
    append_subgroup_object(encoded, 0, properties, 0, 0, {});
    append_vi64(encoded, 0);
    append_vi64(encoded, 0);
    append_vi64(encoded, 0);
    append_vi64(encoded, 9);
    SubgroupDecoder decoder;
    const auto result = decoder.push(encoded, true);
    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.objects.size(), 2u);
    ASSERT_EQ(result.objects[0].properties.size(), 2u);
    const auto& raw = std::get<VarIntValue>(result.objects[0].properties[0].value);
    EXPECT_TRUE(std::ranges::equal(
        raw.raw_bytes,
        bytes({0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09})));
    EXPECT_EQ(result.objects[1].status, 9u);
    ASSERT_EQ(result.observations.size(), 2u);
    EXPECT_EQ(result.observations[0].kind,
              DecoderObservationKind::DraftAmbiguity);
    EXPECT_EQ(result.observations[1].kind,
              DecoderObservationKind::ShouldClose);
    EXPECT_TRUE(result.clean_fin);

    AccumulatedSubgroupResult one_shot;
    accumulate(result, one_shot);
    SubgroupDecoder split_decoder;
    AccumulatedSubgroupResult split;
    accumulate(split_decoder.push(encoded, false), split);
    const auto split_fin = split_decoder.push({}, true);
    EXPECT_TRUE(split_fin.clean_fin);
    accumulate(split_fin, split);
    expect_same_result(split, one_shot, encoded.size());
}

TEST(Draft18SubgroupTest, DistinguishesStatusPropertyRules) {
    auto nonempty = subgroup_header(0x11);
    append_subgroup_object(nonempty, 0, bytes({0x3c, 0x01}), 0, 3, {});
    SubgroupDecoder hard_decoder;
    const auto hard = hard_decoder.push(nonempty, true);
    ASSERT_TRUE(hard.error.has_value());
    EXPECT_EQ(hard.error->code, DecodeErrorCode::ProtocolViolation);

    auto empty = subgroup_header(0x11);
    append_vi64(empty, 0);
    append_vi64(empty, 0);
    append_vi64(empty, 0);
    append_vi64(empty, 3);
    SubgroupDecoder ambiguous_decoder;
    const auto ambiguous = ambiguous_decoder.push(empty, true);
    ASSERT_FALSE(ambiguous.error.has_value());
    ASSERT_EQ(ambiguous.objects.size(), 1u);
    ASSERT_EQ(ambiguous.observations.size(), 1u);
    EXPECT_EQ(ambiguous.observations[0].kind,
              DecoderObservationKind::DraftAmbiguity);
}

TEST(Draft18SubgroupTest, EnforcesObjectIdOverflowAndTerminalStatus) {
    auto overflow = subgroup_header(0x10);
    append_subgroup_object(overflow, std::numeric_limits<std::uint64_t>::max(),
                           {}, 0, 0, {});
    const auto overflow_offset = overflow.size();
    append_subgroup_object(overflow, 0, {}, 0, 0, {});
    SubgroupDecoder overflow_decoder;
    const auto overflow_result = overflow_decoder.push(overflow, true);
    ASSERT_TRUE(overflow_result.error.has_value());
    EXPECT_EQ(overflow_result.error->code, DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(overflow_result.error->offset, overflow_offset);

    for (const auto status : {3u, 4u}) {
        auto terminal = subgroup_header(0x10);
        append_subgroup_object(terminal, 0, {}, 0, status, {});
        const auto next_offset = terminal.size();
        append_subgroup_object(terminal, 0, {}, 0, 0, {});
        SubgroupDecoder decoder;
        const auto result = decoder.push(terminal, true);
        ASSERT_TRUE(result.error.has_value()) << status;
        EXPECT_EQ(result.error->offset, next_offset) << status;
    }
}

TEST(Draft18SubgroupTest, RetainsOnlyConfiguredPayloadPrefixAcrossChunks) {
    auto encoded = subgroup_header(0x10);
    append_vi64(encoded, 0);
    append_vi64(encoded, 1000);
    encoded.insert(encoded.end(), 1000, std::byte{0x5a});
    Limits limits;
    limits.maximum_retained_payload_length = 3;
    SubgroupDecoder decoder(limits);
    SubgroupPushResult final;
    for (const auto octet : encoded) {
        const std::array one{octet};
        auto result = decoder.push(one, false);
        final.objects.insert(final.objects.end(), result.objects.begin(),
                             result.objects.end());
    }
    auto fin = decoder.push({}, true);
    final.objects.insert(final.objects.end(), fin.objects.begin(), fin.objects.end());
    ASSERT_EQ(final.objects.size(), 1u);
    EXPECT_EQ(final.objects[0].payload_length, 1000u);
    EXPECT_TRUE(std::ranges::equal(final.objects[0].retained_payload,
                                   bytes({0x5a, 0x5a, 0x5a})));
    EXPECT_LE(decoder.buffered_byte_count(),
              limits.maximum_object_properties_length + 3u + 8u);
}

TEST(Draft18SubgroupTest, IsStableAcrossEveryChunkSplit) {
    auto encoded = subgroup_header(0x59);
    append_vi64(encoded, 7);
    append_vi64(encoded, 0);
    append_vi64(encoded, 2);
    const auto payload = bytes({0xaa, 0xbb});
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    append_vi64(encoded, 0);
    append_vi64(encoded, 0);
    append_vi64(encoded, 0);
    append_vi64(encoded, 4);

    SubgroupDecoder one_shot;
    AccumulatedSubgroupResult expected;
    accumulate(one_shot.push(encoded, true), expected);
    ASSERT_FALSE(expected.error.has_value());
    ASSERT_EQ(expected.objects.size(), 2u);

    for (std::size_t split = 0; split <= encoded.size(); ++split) {
        SubgroupDecoder split_decoder;
        AccumulatedSubgroupResult actual;
        accumulate(split_decoder.push(
                       std::span<const std::byte>(encoded).first(split), false),
                   actual);
        accumulate(split_decoder.push(
                       std::span<const std::byte>(encoded).subspan(split), true),
                   actual);
        expect_same_result(actual, expected, split);
    }
}

TEST(Draft18SubgroupTest, ClassifiesFinByParserPhaseAndIsTerminal) {
    const auto header = subgroup_header(0x10);
    constexpr std::array header_phases{
        SubgroupDecodePhase::Type,
        SubgroupDecodePhase::TrackAlias,
        SubgroupDecodePhase::GroupId,
        SubgroupDecodePhase::Priority,
    };
    for (std::size_t split = 0; split < header.size(); ++split) {
        SubgroupDecoder decoder;
        const auto result = decoder.push(
            std::span<const std::byte>(header).first(split), true);
        EXPECT_FALSE(result.error.has_value()) << split;
        ASSERT_EQ(result.observations.size(), 1u) << split;
        EXPECT_EQ(result.observations[0].kind,
                  DecoderObservationKind::DraftAmbiguity) << split;
        EXPECT_EQ(result.observations[0].phase, header_phases[split]) << split;
        EXPECT_EQ(result.observations[0].offset, split) << split;
    }

    auto mid_payload = header;
    append_vi64(mid_payload, 0);
    append_vi64(mid_payload, 3);
    mid_payload.push_back(std::byte{0xaa});
    SubgroupDecoder decoder;
    const auto partial = decoder.push(mid_payload, true);
    ASSERT_FALSE(partial.error.has_value());
    ASSERT_EQ(partial.observations.size(), 1u);
    EXPECT_EQ(partial.observations[0].kind,
              DecoderObservationKind::ShouldClose);
    EXPECT_EQ(partial.observations[0].offset, mid_payload.size());
    EXPECT_FALSE(partial.clean_fin);

    const auto misuse = decoder.push({}, false);
    EXPECT_TRUE(misuse.local_api_misuse);
    EXPECT_FALSE(misuse.error.has_value());
}

TEST(Draft18SubgroupTest, AcceptsNonMinimalStructuralIntegersAndEmptyPushes) {
    const auto encoded = bytes({
        0x80, 0x10, 0x80, 0x01, 0x80, 0x02, 0x03,
        0x80, 0x00, 0x80, 0x01, 0xaa,
    });
    SubgroupDecoder decoder;
    const auto empty = decoder.push({}, false);
    EXPECT_FALSE(empty.header.has_value());
    EXPECT_TRUE(empty.objects.empty());
    const auto first = decoder.push(encoded, false);
    ASSERT_TRUE(first.header.has_value());
    ASSERT_EQ(first.objects.size(), 1u);
    EXPECT_EQ(first.objects[0].payload_length, 1u);
    const auto repeated = decoder.push({}, false);
    EXPECT_FALSE(repeated.header.has_value());
    EXPECT_TRUE(repeated.objects.empty());
    EXPECT_TRUE(repeated.observations.empty());
    const auto fin = decoder.push({}, true);
    EXPECT_TRUE(fin.clean_fin);
}

TEST(Draft18SubgroupTest, EnforcesPropertyBoundsAndFormatting) {
    auto malformed = subgroup_header(0x11);
    append_vi64(malformed, 0);
    append_vi64(malformed, 1);
    malformed.push_back(std::byte{0x3c});
    SubgroupDecoder malformed_decoder;
    const auto malformed_result = malformed_decoder.push(malformed, false);
    ASSERT_TRUE(malformed_result.error.has_value());
    EXPECT_EQ(malformed_result.error->code,
              DecodeErrorCode::KeyValueFormattingError);

    auto too_large = subgroup_header(0x11);
    append_vi64(too_large, 0);
    const auto length_offset = too_large.size();
    append_vi64(too_large, 2);
    Limits length_limits;
    length_limits.maximum_object_properties_length = 1;
    SubgroupDecoder length_decoder(length_limits);
    const auto length_result = length_decoder.push(too_large, false);
    ASSERT_TRUE(length_result.error.has_value());
    EXPECT_EQ(length_result.error->code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(length_result.error->offset, length_offset);

    auto odd_too_large = subgroup_header(0x11);
    append_vi64(odd_too_large, 0);
    append_vi64(odd_too_large, 5);
    const auto odd_property = bytes({0x79, 0x03, 0xaa, 0xbb, 0xcc});
    odd_too_large.insert(odd_too_large.end(), odd_property.begin(),
                         odd_property.end());
    Limits odd_limits;
    odd_limits.maximum_odd_value_length = 2;
    SubgroupDecoder odd_decoder(odd_limits);
    const auto odd_result = odd_decoder.push(odd_too_large, false);
    ASSERT_TRUE(odd_result.error.has_value());
    EXPECT_EQ(odd_result.error->code, DecodeErrorCode::LengthExceedsLimit);
}

TEST(Draft18SubgroupTest, ReportsEveryMidObjectFinAsRecommendation) {
    std::vector<std::vector<std::byte>> partials;
    constexpr std::array expected_phases{
        SubgroupDecodePhase::ObjectIdDelta,
        SubgroupDecodePhase::PropertiesLength,
        SubgroupDecodePhase::Properties,
        SubgroupDecodePhase::PayloadLength,
        SubgroupDecodePhase::Status,
    };
    const auto plain_header = subgroup_header(0x10);
    const auto properties_header = subgroup_header(0x11);

    auto partial_delta = plain_header;
    partial_delta.push_back(std::byte{0x80});
    partials.push_back(partial_delta);

    auto partial_properties_length = properties_header;
    append_vi64(partial_properties_length, 0);
    partial_properties_length.push_back(std::byte{0x80});
    partials.push_back(partial_properties_length);

    auto partial_properties = properties_header;
    append_vi64(partial_properties, 0);
    append_vi64(partial_properties, 2);
    partial_properties.push_back(std::byte{0x3c});
    partials.push_back(partial_properties);

    auto partial_payload_length = plain_header;
    append_vi64(partial_payload_length, 0);
    partial_payload_length.push_back(std::byte{0x80});
    partials.push_back(partial_payload_length);

    auto partial_status = plain_header;
    append_vi64(partial_status, 0);
    append_vi64(partial_status, 0);
    partial_status.push_back(std::byte{0x80});
    partials.push_back(partial_status);

    for (std::size_t index = 0; index < partials.size(); ++index) {
        SubgroupDecoder decoder;
        const auto result = decoder.push(partials[index], true);
        ASSERT_FALSE(result.error.has_value()) << index;
        ASSERT_EQ(result.observations.size(), 1u) << index;
        EXPECT_EQ(result.observations[0].kind,
                  DecoderObservationKind::ShouldClose) << index;
        EXPECT_EQ(result.observations[0].offset, partials[index].size()) << index;
        EXPECT_EQ(result.observations[0].phase, expected_phases[index]) << index;
        EXPECT_FALSE(result.clean_fin) << index;
    }
}

TEST(Draft18SubgroupTest, InfersFinalObjectOnlyFromEndOfGroupCleanFin) {
    auto encoded = subgroup_header(0x18);
    append_subgroup_object(encoded, 8, {}, 1, std::nullopt, bytes({0xaa}));
    SubgroupDecoder decoder;
    const auto result = decoder.push(encoded, true);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_TRUE(result.clean_fin);
    EXPECT_EQ(result.final_object_id, 8u);

    auto ordinary = subgroup_header(0x10);
    append_subgroup_object(ordinary, 8, {}, 1, std::nullopt, bytes({0xaa}));
    SubgroupDecoder ordinary_decoder;
    const auto ordinary_result = ordinary_decoder.push(ordinary, true);
    EXPECT_TRUE(ordinary_result.clean_fin);
    EXPECT_FALSE(ordinary_result.final_object_id.has_value());
}

TEST(Draft18SubgroupTest, AppliesFirstObjectBitOnlyToFirstStreamObject) {
    auto encoded = subgroup_header(0x50);
    append_subgroup_object(encoded, 0, {}, 1, std::nullopt, bytes({0xaa}));
    append_subgroup_object(encoded, 0, {}, 1, std::nullopt, bytes({0xbb}));
    SubgroupDecoder decoder;
    const auto result = decoder.push(encoded, true);
    ASSERT_EQ(result.objects.size(), 2u);
    EXPECT_TRUE(result.objects[0].first_object);
    EXPECT_FALSE(result.objects[1].first_object);
}

TEST(Draft18SubgroupTest, AcceptsNonMinimalOptionalSubgroupPropertyAndStatusFields) {
    auto encoded = bytes({
        0x80, 0x15,
        0x80, 0x01,
        0x80, 0x02,
        0x80, 0x03,
        0x04,
        0x80, 0x00,
        0x80, 0x04,
        0x80, 0x3c, 0x80, 0x07,
        0x80, 0x00,
        0x80, 0x00,
    });
    SubgroupDecoder decoder;
    const auto result = decoder.push(encoded, true);
    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(result.objects[0].subgroup_id, 3u);
    ASSERT_EQ(result.objects[0].properties.size(), 1u);
    EXPECT_TRUE(std::ranges::equal(
        std::get<VarIntValue>(result.objects[0].properties[0].value).raw_bytes,
        bytes({0x80, 0x07})));
    EXPECT_EQ(result.objects[0].status, 0u);
}

TEST(Draft18SubgroupTest, SupportsZeroPayloadEvidenceLimitAndErrorStickiness) {
    auto encoded = subgroup_header(0x10);
    append_subgroup_object(encoded, 0, {}, 2, std::nullopt,
                           bytes({0xaa, 0xbb}));
    Limits limits;
    limits.maximum_retained_payload_length = 0;
    SubgroupDecoder decoder(limits);
    const auto result = decoder.push(encoded, true);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_TRUE(result.objects[0].retained_payload.empty());
    EXPECT_EQ(result.objects[0].payload_length, 2u);

    SubgroupDecoder invalid;
    const auto failed = invalid.push(bytes({0x05}), false);
    ASSERT_TRUE(failed.error.has_value());
    const auto misuse = invalid.push(bytes({0x10}), true);
    EXPECT_TRUE(misuse.local_api_misuse);
    EXPECT_FALSE(misuse.error.has_value());
}

TEST(Draft18SubgroupTest, AccountsForDecodedPropertiesWithoutRetainingRawBlock) {
    std::vector<std::byte> property;
    append_vi64(property, 0x3c);
    const auto raw_integer =
        bytes({0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09});
    property.insert(property.end(), raw_integer.begin(), raw_integer.end());
    append_vi64(property, 0x79 - 0x3c);
    append_vi64(property, 100);
    property.insert(property.end(), 100, std::byte{0x5a});

    auto prefix = subgroup_header(0x11);
    append_vi64(prefix, 0);
    append_vi64(prefix, property.size());
    prefix.insert(prefix.end(), property.begin(), property.end());
    append_vi64(prefix, 1000);

    Limits limits;
    limits.maximum_retained_payload_length = 3;
    SubgroupDecoder decoder(limits);
    const auto prefix_result = decoder.push(prefix, false);
    ASSERT_FALSE(prefix_result.error.has_value());

    const auto decoded_property_storage = 2u * sizeof(KeyValuePair) + 108u;
    EXPECT_GE(decoder.buffered_byte_count(), decoded_property_storage);
    EXPECT_LE(decoder.buffered_byte_count(),
              decoded_property_storage + limits.maximum_retained_payload_length + 8u);
    for (std::size_t index = 0; index < 999u; ++index) {
        const std::array payload_byte{std::byte{0xaa}};
        const auto result = decoder.push(payload_byte, false);
        ASSERT_FALSE(result.error.has_value()) << index;
        EXPECT_GE(decoder.buffered_byte_count(), decoded_property_storage) << index;
        EXPECT_LE(decoder.buffered_byte_count(),
                  decoded_property_storage +
                      limits.maximum_retained_payload_length + 8u) << index;
    }
    const std::array final_byte{std::byte{0xbb}};
    const auto completed = decoder.push(final_byte, true);
    ASSERT_EQ(completed.objects.size(), 1u);
    ASSERT_EQ(completed.objects[0].properties.size(), 2u);
    EXPECT_TRUE(std::ranges::equal(
        std::get<VarIntValue>(completed.objects[0].properties[0].value).raw_bytes,
        raw_integer));
    EXPECT_EQ(std::get<ByteValue>(completed.objects[0].properties[1].value)
                  .bytes.size(),
              100u);
}

TEST(Draft18SubgroupTest, ResolvesSparseSecondObjectAndPreservesOffsets) {
    auto encoded = subgroup_header(0x10);
    const auto first_offset = encoded.size();
    append_subgroup_object(encoded, 10, {}, 1, std::nullopt, bytes({0xaa}));
    const auto second_offset = encoded.size();
    append_subgroup_object(encoded, 5, {}, 1, std::nullopt, bytes({0xbb}));
    SubgroupDecoder decoder;
    const auto result = decoder.push(encoded, true);
    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.objects.size(), 2u);
    EXPECT_EQ(result.objects[0].object_id, 10u);
    EXPECT_EQ(result.objects[1].object_id, 16u);
    EXPECT_EQ(result.objects[0].stream_offset, first_offset);
    EXPECT_EQ(result.objects[0].stream_end_offset, second_offset);
    EXPECT_EQ(result.objects[1].stream_offset, second_offset);
    EXPECT_EQ(result.objects[1].stream_end_offset, encoded.size());
}

TEST(Draft18FetchTest, DecodesHeaderAndFirstObject) {
    const auto encoded = bytes({0x05, 0x07, 0x1f, 0x02, 0x03, 0x04,
                                0x05, 0x02, 0xaa, 0xbb});
    FetchDecoder decoder([](std::uint64_t request_id) {
        EXPECT_EQ(request_id, 7u);
        return std::optional{FetchGroupOrder::Ascending};
    });
    const auto result = decoder.push(encoded, true);
    ASSERT_TRUE(result.header.has_value());
    EXPECT_EQ(result.header->request_id, 7u);
    ASSERT_EQ(result.events.size(), 1u);
    const auto& object = std::get<ObjectEvent>(result.events[0]);
    EXPECT_EQ(object.request_id, 7u);
    EXPECT_EQ(object.serialization_flags, 0x1fu);
    EXPECT_FALSE(object.track_alias.has_value());
    EXPECT_EQ(object.group_id, 2u);
    EXPECT_EQ(object.subgroup_id, 3u);
    EXPECT_EQ(object.object_id, 4u);
    EXPECT_EQ(object.publisher_priority, 5u);
    EXPECT_EQ(object.payload_length, 2u);
    EXPECT_TRUE(std::ranges::equal(object.retained_payload,
                                   bytes({0xaa, 0xbb})));
    EXPECT_TRUE(result.clean_fin);
}

TEST(Draft18PaddingStreamTest, DiscardsBodyAndReportsNonzeroEvidence) {
    PaddingStreamDecoder decoder;
    const auto result = decoder.push(
        bytes({0xf0, 0x13, 0x2b, 0x3e, 0x28, 0x00, 0x01, 0x00}), true);
    EXPECT_EQ(result.discarded_byte_count, 3u);
    EXPECT_EQ(result.first_nonzero_offset, 6u);
    EXPECT_EQ(decoder.buffered_byte_count(), 0u);
    EXPECT_TRUE(result.clean_fin);
}

TEST(Draft18PaddingStreamTest, CoversEmptyZeroAndFirstByteNonzeroBodies) {
    const auto header = bytes({0xf0, 0x13, 0x2b, 0x3e, 0x28});

    PaddingStreamDecoder empty_decoder;
    const auto empty = empty_decoder.push(header, true);
    EXPECT_EQ(empty.discarded_byte_count, 0u);
    EXPECT_FALSE(empty.first_nonzero_offset.has_value());
    EXPECT_EQ(empty_decoder.buffered_byte_count(), 0u);
    EXPECT_TRUE(empty.clean_fin);

    auto all_zero = header;
    all_zero.insert(all_zero.end(), 4, std::byte{0});
    PaddingStreamDecoder zero_decoder;
    const auto zero = zero_decoder.push(all_zero, true);
    EXPECT_EQ(zero.discarded_byte_count, 4u);
    EXPECT_FALSE(zero.first_nonzero_offset.has_value());
    EXPECT_EQ(zero_decoder.buffered_byte_count(), 0u);
    EXPECT_TRUE(zero.clean_fin);

    auto first_nonzero = header;
    first_nonzero.push_back(std::byte{1});
    first_nonzero.push_back(std::byte{0});
    PaddingStreamDecoder nonzero_decoder;
    const auto nonzero = nonzero_decoder.push(first_nonzero, true);
    EXPECT_EQ(nonzero.discarded_byte_count, 2u);
    EXPECT_EQ(nonzero.first_nonzero_offset, header.size());
    EXPECT_EQ(nonzero_decoder.buffered_byte_count(), 0u);
    EXPECT_TRUE(nonzero.clean_fin);
}

TEST(Draft18FetchTest, AcceptsNonMinimalHeaderAndEveryHeaderSplit) {
    const auto encoded = bytes({0x80, 0x05, 0x80, 0x07});
    for (std::size_t split = 0; split <= encoded.size(); ++split) {
        FetchDecoder decoder(ascending_order());
        const auto first = decoder.push(
            std::span<const std::byte>(encoded).first(split), false);
        EXPECT_FALSE(first.error.has_value()) << split;
        const auto second = decoder.push(
            std::span<const std::byte>(encoded).subspan(split), true);
        EXPECT_FALSE(second.error.has_value()) << split;
        const auto& header = first.header ? *first.header : *second.header;
        EXPECT_EQ(header.raw_type, 5u) << split;
        EXPECT_EQ(header.request_id, 7u) << split;
        EXPECT_TRUE(second.clean_fin) << split;
    }

    for (std::size_t split = 0; split < encoded.size(); ++split) {
        FetchDecoder decoder(ascending_order());
        const auto result = decoder.push(
            std::span<const std::byte>(encoded).first(split), true);
        ASSERT_EQ(result.observations.size(), 1u) << split;
        EXPECT_EQ(result.observations[0].kind,
                  DecoderObservationKind::DraftAmbiguity) << split;
        EXPECT_FALSE(result.clean_fin) << split;
    }
}

TEST(Draft18FetchTest, CoversAllNormalFlagsAndFirstObjectPredicate) {
    for (std::uint64_t flags = 0; flags < 128; ++flags) {
        auto encoded = fetch_header();
        append_safe_fetch_object(encoded, 0x1cu, 10, 20, 30);
        append_safe_fetch_object(encoded, flags, 0, 0, 31);
        FetchDecoder decoder(ascending_order());
        const auto result = decoder.push(encoded, true);
        ASSERT_FALSE(result.error.has_value()) << flags;
        ASSERT_EQ(result.events.size(), 2u) << flags;
        const auto& object = std::get<ObjectEvent>(result.events[1]);
        EXPECT_EQ(object.serialization_flags, flags) << flags;
        EXPECT_EQ(object.forwarding_preference,
                  (flags & 0x40u) != 0u
                      ? ObjectForwardingPreference::Datagram
                      : ObjectForwardingPreference::Subgroup) << flags;
    }

    for (std::uint64_t flags = 0; flags < 128; ++flags) {
        auto encoded = fetch_header();
        append_safe_fetch_object(encoded, flags);
        FetchDecoder decoder(ascending_order());
        const auto result = decoder.push(encoded, true);
        const bool valid = (flags & 0x1cu) == 0x1cu &&
                           ((flags & 0x40u) != 0u ||
                            (flags & 0x03u) == 0u ||
                            (flags & 0x03u) == 3u);
        EXPECT_EQ(!result.error.has_value(), valid) << flags;
        if (valid) {
            EXPECT_EQ(result.events.size(), 1u) << flags;
        }
    }
}

TEST(Draft18FetchTest, DatagramIgnoresAndPreservesEveryLowMode) {
    for (std::uint64_t mode = 0; mode < 4; ++mode) {
        auto encoded = fetch_header();
        append_safe_fetch_object(encoded, 0x5cu | mode, 7, 9, 11);
        FetchDecoder decoder(ascending_order());
        const auto result = decoder.push(encoded, true);
        ASSERT_FALSE(result.error.has_value()) << mode;
        ASSERT_EQ(result.events.size(), 1u) << mode;
        const auto& object = std::get<ObjectEvent>(result.events[0]);
        EXPECT_EQ(object.serialization_flags, 0x5cu | mode) << mode;
        EXPECT_FALSE(object.subgroup_id.has_value()) << mode;
        EXPECT_EQ(object.object_id, 9u) << mode;
        EXPECT_EQ(result.observations.size(), mode == 0u ? 0u : 1u) << mode;
        if (mode != 0u) {
            EXPECT_EQ(result.observations[0].kind,
                      DecoderObservationKind::ShouldViolation) << mode;
        }
    }
}

TEST(Draft18FetchTest, ResolvesEverySubgroupModeFromLastActualObject) {
    auto encoded = fetch_header();
    append_vi64(encoded, 0x1fu);
    append_vi64(encoded, 1);
    append_vi64(encoded, 7);
    append_vi64(encoded, 10);
    encoded.push_back(std::byte{9});
    append_vi64(encoded, 0);
    append_safe_fetch_object(encoded, 0x01u);
    append_safe_fetch_object(encoded, 0x02u);
    append_safe_fetch_object(encoded, 0x00u);
    append_safe_fetch_object(encoded, 0x03u);

    FetchDecoder decoder(ascending_order());
    const auto result = decoder.push(encoded, true);
    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.events.size(), 5u);
    EXPECT_EQ(std::get<ObjectEvent>(result.events[0]).subgroup_id, 7u);
    EXPECT_EQ(std::get<ObjectEvent>(result.events[1]).subgroup_id, 7u);
    EXPECT_EQ(std::get<ObjectEvent>(result.events[2]).subgroup_id, 8u);
    EXPECT_EQ(std::get<ObjectEvent>(result.events[3]).subgroup_id, 0u);
    EXPECT_EQ(std::get<ObjectEvent>(result.events[4]).subgroup_id, 3u);
    EXPECT_TRUE(result.clean_fin);
}

TEST(Draft18FetchTest, AppliesGroupAndObjectDeltaArithmeticForBothOrders) {
    for (const auto order : {FetchGroupOrder::Ascending,
                             FetchGroupOrder::Descending}) {
        auto encoded = fetch_header();
        append_safe_fetch_object(encoded, 0x1cu, 10, 20, 30);
        append_safe_fetch_object(encoded, 0x00u);
        append_safe_fetch_object(encoded, 0x0cu, 1, 4);
        append_safe_fetch_object(encoded, 0x08u, 0, 0);
        append_safe_fetch_object(encoded, 0x04u, 0, 7);
        FetchDecoder decoder([order](std::uint64_t) {
            return std::optional{order};
        });
        const auto result = decoder.push(encoded, true);
        ASSERT_FALSE(result.error.has_value());
        ASSERT_EQ(result.events.size(), 5u);
        const auto& same_group = std::get<ObjectEvent>(result.events[1]);
        EXPECT_EQ(same_group.group_id, 10u);
        EXPECT_EQ(same_group.object_id, 21u);
        const auto& new_group = std::get<ObjectEvent>(result.events[2]);
        EXPECT_EQ(new_group.group_id,
                  order == FetchGroupOrder::Ascending ? 12u : 8u);
        EXPECT_EQ(new_group.object_id, 4u);
        const auto& new_group_implicit_object =
            std::get<ObjectEvent>(result.events[3]);
        EXPECT_EQ(new_group_implicit_object.group_id,
                  order == FetchGroupOrder::Ascending ? 13u : 7u);
        EXPECT_EQ(new_group_implicit_object.object_id, 5u);
        const auto& same_group_explicit_object =
            std::get<ObjectEvent>(result.events[4]);
        EXPECT_EQ(same_group_explicit_object.group_id,
                  new_group_implicit_object.group_id);
        EXPECT_EQ(same_group_explicit_object.object_id, 12u);
    }
}

TEST(Draft18FetchTest, ClassifiesLocationAndSubgroupArithmeticFailures) {
    auto ascending = fetch_header();
    append_safe_fetch_object(ascending, 0x1cu,
                             std::numeric_limits<std::uint64_t>::max(), 0, 1);
    append_safe_fetch_object(ascending, 0x0cu, 0, 0);
    FetchDecoder ascending_decoder(ascending_order());
    const auto ascending_result = ascending_decoder.push(ascending, true);
    ASSERT_TRUE(ascending_result.error.has_value());
    EXPECT_EQ(ascending_result.error->code, DecodeErrorCode::ProtocolViolation);

    auto descending = fetch_header();
    append_safe_fetch_object(descending, 0x1cu, 0, 0, 1);
    append_safe_fetch_object(descending, 0x0cu, 0, 0);
    FetchDecoder descending_decoder([](std::uint64_t) {
        return std::optional{FetchGroupOrder::Descending};
    });
    const auto descending_result = descending_decoder.push(descending, true);
    ASSERT_TRUE(descending_result.error.has_value());
    EXPECT_EQ(descending_result.error->code, DecodeErrorCode::ProtocolViolation);

    auto object = fetch_header();
    append_safe_fetch_object(object, 0x1cu, 1,
                             std::numeric_limits<std::uint64_t>::max(), 1);
    append_safe_fetch_object(object, 0x04u, 0, 1);
    FetchDecoder object_decoder(ascending_order());
    const auto object_result = object_decoder.push(object, true);
    ASSERT_TRUE(object_result.error.has_value());
    EXPECT_EQ(object_result.error->code, DecodeErrorCode::ProtocolViolation);

    auto subgroup = fetch_header();
    append_vi64(subgroup, 0x1fu);
    append_vi64(subgroup, 1);
    append_vi64(subgroup, std::numeric_limits<std::uint64_t>::max());
    append_vi64(subgroup, 1);
    subgroup.push_back(std::byte{1});
    append_vi64(subgroup, 0);
    append_safe_fetch_object(subgroup, 0x02u);
    FetchDecoder subgroup_decoder(ascending_order());
    const auto subgroup_result = subgroup_decoder.push(subgroup, true);
    EXPECT_FALSE(subgroup_result.error.has_value());
    ASSERT_EQ(subgroup_result.observations.size(), 1u);
    EXPECT_EQ(subgroup_result.observations[0].kind,
              DecoderObservationKind::DraftAmbiguity);
}

TEST(Draft18FetchTest, MissingGroupOrderIsOnlyAmbiguousWhenNeeded) {
    auto no_delta = fetch_header();
    append_safe_fetch_object(no_delta, 0x1cu, 1, 1, 1);
    append_safe_fetch_object(no_delta, 0x00u);
    FetchDecoder no_delta_decoder(
        [](std::uint64_t) -> std::optional<FetchGroupOrder> { return std::nullopt; });
    const auto no_delta_result = no_delta_decoder.push(no_delta, true);
    EXPECT_FALSE(no_delta_result.error.has_value());
    EXPECT_TRUE(no_delta_result.clean_fin);

    auto delta = fetch_header();
    append_safe_fetch_object(delta, 0x1cu, 1, 1, 1);
    append_safe_fetch_object(delta, 0x0cu, 0, 0);
    FetchDecoder delta_decoder(
        [](std::uint64_t) -> std::optional<FetchGroupOrder> { return std::nullopt; });
    const auto delta_result = delta_decoder.push(delta, true);
    EXPECT_FALSE(delta_result.error.has_value());
    ASSERT_EQ(delta_result.observations.size(), 1u);
    EXPECT_EQ(delta_result.observations[0].kind,
              DecoderObservationKind::DraftAmbiguity);
    EXPECT_FALSE(delta_result.clean_fin);
}

TEST(Draft18FetchTest, PreservesOrderedRangeEventsAndPostRangeState) {
    auto encoded = fetch_header();
    append_vi64(encoded, 0x8c);
    append_vi64(encoded, 4);
    append_vi64(encoded, 5);
    append_safe_fetch_object(encoded, 0x1fu, 10, 12, 7);
    append_vi64(encoded, 0x10c);
    append_vi64(encoded, 20);
    append_vi64(encoded, 30);
    append_safe_fetch_object(encoded, 0x01u);
    FetchDecoder decoder(ascending_order());
    const auto result = decoder.push(encoded, true);
    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.events.size(), 4u);
    EXPECT_EQ(std::get<FetchRangeEvent>(result.events[0]).kind,
              FetchRangeKind::NonExistent);
    const auto& first = std::get<ObjectEvent>(result.events[1]);
    EXPECT_EQ(first.group_id, 10u);
    EXPECT_EQ(first.object_id, 12u);
    EXPECT_EQ(std::get<FetchRangeEvent>(result.events[2]).kind,
              FetchRangeKind::Unknown);
    const auto& after = std::get<ObjectEvent>(result.events[3]);
    EXPECT_EQ(after.group_id, 20u);
    EXPECT_EQ(after.object_id, 31u);
    EXPECT_EQ(after.subgroup_id, 3u);
    EXPECT_EQ(after.publisher_priority, 7u);
}

TEST(Draft18FetchTest, RejectsEveryUnsupportedSpecialFlag) {
    for (const auto flags : {0x80u, 0x8bu, 0x8du, 0x10bu, 0x10du,
                             0x1ffu, 0x132b3e28u}) {
        auto encoded = fetch_header();
        append_vi64(encoded, flags);
        FetchDecoder decoder(ascending_order());
        const auto result = decoder.push(encoded, false);
        ASSERT_TRUE(result.error.has_value()) << flags;
        EXPECT_EQ(result.error->code, DecodeErrorCode::ProtocolViolation)
            << flags;
    }
}

TEST(Draft18FetchTest, BoundsPropertiesAndPayloadEvidence) {
    auto encoded = fetch_header();
    append_vi64(encoded, 0x3cu);
    append_vi64(encoded, 1);
    append_vi64(encoded, 2);
    encoded.push_back(std::byte{3});
    const auto property = bytes({0x3c, 0x80, 0x09, 0x3d, 0x02, 0xaa, 0xbb});
    append_vi64(encoded, property.size());
    encoded.insert(encoded.end(), property.begin(), property.end());
    append_vi64(encoded, 1000);
    encoded.insert(encoded.end(), 1000, std::byte{0x5a});
    Limits limits;
    limits.maximum_retained_payload_length = 3;
    FetchDecoder decoder(ascending_order(), limits);
    FetchPushResult final;
    for (const auto octet : encoded) {
        const std::array one{octet};
        auto part = decoder.push(one, false);
        final.events.insert(final.events.end(), part.events.begin(),
                            part.events.end());
        ASSERT_FALSE(part.error.has_value());
    }
    const auto fin = decoder.push({}, true);
    ASSERT_EQ(final.events.size(), 1u);
    const auto& object = std::get<ObjectEvent>(final.events[0]);
    ASSERT_EQ(object.properties.size(), 2u);
    EXPECT_TRUE(std::ranges::equal(
        std::get<VarIntValue>(object.properties[0].value).raw_bytes,
        bytes({0x80, 0x09})));
    EXPECT_EQ(object.payload_length, 1000u);
    EXPECT_EQ(object.retained_payload.size(), 3u);
    EXPECT_TRUE(fin.clean_fin);
    EXPECT_LT(decoder.buffered_byte_count(), 256u);

    auto malformed = fetch_header();
    append_vi64(malformed, 0x3cu);
    append_vi64(malformed, 1);
    append_vi64(malformed, 2);
    malformed.push_back(std::byte{3});
    append_vi64(malformed, 1);
    malformed.push_back(std::byte{0x3c});
    FetchDecoder malformed_decoder(ascending_order());
    const auto malformed_result = malformed_decoder.push(malformed, false);
    ASSERT_TRUE(malformed_result.error.has_value());
    EXPECT_EQ(malformed_result.error->code,
              DecodeErrorCode::KeyValueFormattingError);
}

TEST(Draft18FetchTest, AccountsForDecodedPropertiesDuringIncompletePayload) {
    std::vector<std::byte> property;
    append_vi64(property, 0x3c);
    const auto raw_integer =
        bytes({0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09});
    property.insert(property.end(), raw_integer.begin(), raw_integer.end());
    append_vi64(property, 0x79 - 0x3c);
    append_vi64(property, 100);
    property.insert(property.end(), 100, std::byte{0x5a});

    auto prefix = fetch_header();
    append_vi64(prefix, 0x3cu);
    append_vi64(prefix, 1);
    append_vi64(prefix, 2);
    prefix.push_back(std::byte{3});
    append_vi64(prefix, property.size());
    prefix.insert(prefix.end(), property.begin(), property.end());
    append_vi64(prefix, 1000);
    prefix.push_back(std::byte{0xaa});

    Limits limits;
    limits.maximum_retained_payload_length = 3;
    FetchDecoder decoder(ascending_order(), limits);
    const auto prefix_result = decoder.push(prefix, false);
    ASSERT_FALSE(prefix_result.error.has_value());
    EXPECT_TRUE(prefix_result.events.empty());

    const auto decoded_property_storage = 2u * sizeof(KeyValuePair) + 108u;
    EXPECT_GE(decoder.buffered_byte_count(), decoded_property_storage);
    EXPECT_LE(decoder.buffered_byte_count(),
              decoded_property_storage +
                  limits.maximum_retained_payload_length + 8u);

    const auto payload_chunk = bytes({0xbb, 0xcc, 0xdd, 0xee});
    const auto payload_result = decoder.push(payload_chunk, false);
    ASSERT_FALSE(payload_result.error.has_value());
    EXPECT_TRUE(payload_result.events.empty());
    EXPECT_GE(decoder.buffered_byte_count(), decoded_property_storage);
    EXPECT_LE(decoder.buffered_byte_count(),
              decoded_property_storage +
                  limits.maximum_retained_payload_length + 8u);
}

TEST(Draft18FetchTest, EnforcesPropertyLengthAndOddValueLimits) {
    auto excessive = fetch_header();
    append_vi64(excessive, 0x3cu);
    append_vi64(excessive, 1);
    append_vi64(excessive, 2);
    excessive.push_back(std::byte{3});
    append_vi64(excessive, 2);
    Limits length_limits;
    length_limits.maximum_object_properties_length = 1;
    FetchDecoder length_decoder(ascending_order(), length_limits);
    const auto length_result = length_decoder.push(excessive, false);
    ASSERT_TRUE(length_result.error.has_value());
    EXPECT_EQ(length_result.error->code, DecodeErrorCode::LengthExceedsLimit);

    auto odd = fetch_header();
    append_vi64(odd, 0x3cu);
    append_vi64(odd, 1);
    append_vi64(odd, 2);
    odd.push_back(std::byte{3});
    const auto property = bytes({0x79, 0x03, 0xaa, 0xbb, 0xcc});
    append_vi64(odd, property.size());
    odd.insert(odd.end(), property.begin(), property.end());
    Limits odd_limits;
    odd_limits.maximum_odd_value_length = 2;
    FetchDecoder odd_decoder(ascending_order(), odd_limits);
    const auto odd_result = odd_decoder.push(odd, false);
    ASSERT_TRUE(odd_result.error.has_value());
    EXPECT_EQ(odd_result.error->code, DecodeErrorCode::LengthExceedsLimit);
}

TEST(Draft18FetchTest, ClassifiesFinInEveryRecordPhase) {
    std::vector<std::vector<std::byte>> partials;

    auto flags = fetch_header();
    flags.push_back(std::byte{0x80});
    partials.push_back(flags);

    auto group = fetch_header();
    append_vi64(group, 0x1cu);
    group.push_back(std::byte{0x80});
    partials.push_back(group);

    auto subgroup = fetch_header();
    append_vi64(subgroup, 0x1fu);
    append_vi64(subgroup, 1);
    subgroup.push_back(std::byte{0x80});
    partials.push_back(subgroup);

    auto object = fetch_header();
    append_vi64(object, 0x1cu);
    append_vi64(object, 1);
    object.push_back(std::byte{0x80});
    partials.push_back(object);

    auto priority = fetch_header();
    append_vi64(priority, 0x1cu);
    append_vi64(priority, 1);
    append_vi64(priority, 2);
    partials.push_back(priority);

    auto properties_length = priority;
    properties_length[2] = std::byte{0x3c};
    properties_length.push_back(std::byte{3});
    properties_length.push_back(std::byte{0x80});
    partials.push_back(properties_length);

    auto properties = priority;
    properties[2] = std::byte{0x3c};
    properties.push_back(std::byte{3});
    append_vi64(properties, 2);
    properties.push_back(std::byte{0x3c});
    partials.push_back(properties);

    auto payload_length = priority;
    payload_length.push_back(std::byte{3});
    payload_length.push_back(std::byte{0x80});
    partials.push_back(payload_length);

    auto payload = priority;
    payload.push_back(std::byte{3});
    append_vi64(payload, 2);
    payload.push_back(std::byte{0xaa});
    partials.push_back(payload);

    auto range_group = fetch_header();
    append_vi64(range_group, 0x8c);
    range_group.push_back(std::byte{0x80});
    partials.push_back(range_group);

    auto range_object = fetch_header();
    append_vi64(range_object, 0x8c);
    append_vi64(range_object, 1);
    range_object.push_back(std::byte{0x80});
    partials.push_back(range_object);

    for (std::size_t index = 0; index < partials.size(); ++index) {
        FetchDecoder decoder(ascending_order());
        const auto result = decoder.push(partials[index], true);
        ASSERT_FALSE(result.error.has_value()) << index;
        ASSERT_EQ(result.observations.size(), 1u) << index;
        EXPECT_EQ(result.observations[0].kind,
                  DecoderObservationKind::ShouldClose) << index;
        EXPECT_EQ(result.observations[0].offset, partials[index].size()) << index;
        EXPECT_FALSE(result.clean_fin) << index;
    }
}

TEST(Draft18FetchTest, AcceptsConsecutiveRangeRecordsAndTerminalMisuse) {
    auto encoded = fetch_header();
    for (const auto flags : {0x8cu, 0x10cu, 0x8cu}) {
        append_vi64(encoded, flags);
        append_vi64(encoded, flags);
        append_vi64(encoded, flags + 1u);
    }
    FetchDecoder decoder(ascending_order());
    const auto empty = decoder.push({}, false);
    EXPECT_TRUE(empty.events.empty());
    const auto repeated_empty = decoder.push({}, false);
    EXPECT_TRUE(repeated_empty.events.empty());
    const auto result = decoder.push(encoded, true);
    ASSERT_FALSE(result.error.has_value());
    EXPECT_EQ(result.events.size(), 3u);
    EXPECT_TRUE(result.clean_fin);
    EXPECT_TRUE(decoder.push({}, false).local_api_misuse);

    auto invalid = fetch_header();
    append_vi64(invalid, 0x80);
    FetchDecoder invalid_decoder(ascending_order());
    ASSERT_TRUE(invalid_decoder.push(invalid, false).error.has_value());
    EXPECT_TRUE(invalid_decoder.push({}, true).local_api_misuse);
}

TEST(Draft18FetchTest, PreservesCompleteTranscriptAcrossAllChunkings) {
    auto encoded = fetch_header(17);
    append_vi64(encoded, 0x3fu);
    append_vi64(encoded, 2);
    append_vi64(encoded, 3);
    append_vi64(encoded, 4);
    encoded.push_back(std::byte{5});
    const auto property = bytes({0x3c, 0x80, 0x09});
    append_vi64(encoded, property.size());
    encoded.insert(encoded.end(), property.begin(), property.end());
    append_vi64(encoded, 2);
    encoded.push_back(std::byte{0xaa});
    encoded.push_back(std::byte{0xbb});
    append_vi64(encoded, 0x10c);
    append_vi64(encoded, 4);
    append_vi64(encoded, 8);
    append_vi64(encoded, 0x43);
    append_vi64(encoded, 1);
    encoded.push_back(std::byte{0xcc});

    FetchDecoder one_shot(ascending_order());
    AccumulatedFetchResult expected;
    accumulate(one_shot.push(encoded, true), expected);
    ASSERT_FALSE(expected.error.has_value());
    ASSERT_EQ(expected.events.size(), 3u);
    ASSERT_EQ(expected.observations.size(), 1u);

    for (std::size_t split = 0; split <= encoded.size(); ++split) {
        FetchDecoder decoder(ascending_order());
        AccumulatedFetchResult actual;
        accumulate(decoder.push(
                       std::span<const std::byte>(encoded).first(split), false),
                   actual);
        accumulate(decoder.push(
                       std::span<const std::byte>(encoded).subspan(split), true),
                   actual);
        expect_same_fetch_result(actual, expected, split);
    }

    FetchDecoder bytewise(ascending_order());
    AccumulatedFetchResult bytewise_result;
    for (const auto octet : encoded) {
        const std::array one{octet};
        accumulate(bytewise.push(one, false), bytewise_result);
    }
    accumulate(bytewise.push({}, true), bytewise_result);
    expect_same_fetch_result(bytewise_result, expected, encoded.size() + 1u);
}

TEST(Draft18FetchTest, IsStableAcrossSplitsAndClassifiesFin) {
    auto encoded = fetch_header();
    append_safe_fetch_object(encoded, 0x1cu, 2, 4, 5);
    for (std::size_t split = 0; split <= encoded.size(); ++split) {
        FetchDecoder decoder(ascending_order());
        std::vector<FetchEvent> events;
        const auto first = decoder.push(
            std::span<const std::byte>(encoded).first(split), false);
        events.insert(events.end(), first.events.begin(), first.events.end());
        const auto second = decoder.push(
            std::span<const std::byte>(encoded).subspan(split), true);
        events.insert(events.end(), second.events.begin(), second.events.end());
        ASSERT_EQ(events.size(), 1u) << split;
        const auto& object = std::get<ObjectEvent>(events[0]);
        EXPECT_EQ(object.group_id, 2u) << split;
        EXPECT_EQ(object.object_id, 4u) << split;
        EXPECT_EQ(object.stream_end_offset, encoded.size()) << split;
        EXPECT_TRUE(second.clean_fin) << split;
    }

    for (std::size_t split = 3; split < encoded.size(); ++split) {
        FetchDecoder decoder(ascending_order());
        const auto result = decoder.push(
            std::span<const std::byte>(encoded).first(split), true);
        ASSERT_EQ(result.observations.size(), 1u) << split;
        EXPECT_EQ(result.observations[0].kind,
                  DecoderObservationKind::ShouldClose) << split;
        EXPECT_FALSE(result.clean_fin) << split;
        EXPECT_TRUE(decoder.push({}, false).local_api_misuse) << split;
    }
}

TEST(Draft18PaddingStreamTest, AcceptsNonMinimalTypeAndEverySplitWithoutRetention) {
    const auto encoded = bytes({0xff, 0x00, 0x00, 0x00, 0x00, 0x13, 0x2b,
                                0x3e, 0x28, 0x00, 0x00, 0x01});
    for (std::size_t split = 0; split <= encoded.size(); ++split) {
        PaddingStreamDecoder decoder;
        const auto first = decoder.push(
            std::span<const std::byte>(encoded).first(split), false);
        const auto second = decoder.push(
            std::span<const std::byte>(encoded).subspan(split), true);
        EXPECT_EQ(first.discarded_byte_count + second.discarded_byte_count, 3u)
            << split;
        const auto nonzero = first.first_nonzero_offset
                                 ? first.first_nonzero_offset
                                 : second.first_nonzero_offset;
        EXPECT_EQ(nonzero, 11u) << split;
        EXPECT_EQ(decoder.buffered_byte_count(), 0u) << split;
        EXPECT_TRUE(second.clean_fin) << split;
        EXPECT_TRUE(decoder.push({}, false).local_api_misuse) << split;
    }
}

TEST(Draft18PaddingStreamTest, ClassifiesTruncatedHeaderAsAmbiguity) {
    const auto header = bytes({0xf0, 0x13, 0x2b, 0x3e, 0x28});
    for (std::size_t split = 0; split < header.size(); ++split) {
        PaddingStreamDecoder decoder;
        const auto result = decoder.push(
            std::span<const std::byte>(header).first(split), true);
        EXPECT_FALSE(result.error.has_value()) << split;
        ASSERT_EQ(result.observations.size(), 1u) << split;
        EXPECT_EQ(result.observations[0].kind,
                  DecoderObservationKind::DraftAmbiguity) << split;
        EXPECT_EQ(decoder.buffered_byte_count(), split) << split;
    }
}

TEST(Draft18PaddingStreamTest, RejectsWrongStreamType) {
    PaddingStreamDecoder decoder;
    const auto result = decoder.push(bytes({0x05}), false);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(result.error->offset, 0u);
    EXPECT_TRUE(decoder.push({}, true).local_api_misuse);
}

}  // namespace
}  // namespace moq::interop::wire::draft18
