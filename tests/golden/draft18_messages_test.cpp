#include "moq/interop/wire/draft18/messages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
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

void expect_bytes(std::span<const std::byte> actual,
                  std::span<const std::byte> expected) {
    EXPECT_TRUE(std::ranges::equal(actual, expected));
}

const SetupMessage& require_setup(const MessageDecodeResult& result) {
    EXPECT_TRUE(std::holds_alternative<Message>(result));
    const auto& message = std::get<Message>(result);
    EXPECT_TRUE(std::holds_alternative<SetupMessage>(message));
    return std::get<SetupMessage>(message);
}

const ByteValue& require_bytes(const KeyValuePair& entry) {
    EXPECT_TRUE(std::holds_alternative<ByteValue>(entry.value));
    return std::get<ByteValue>(entry.value);
}

const VarIntValue& require_integer(const KeyValuePair& entry) {
    EXPECT_TRUE(std::holds_alternative<VarIntValue>(entry.value));
    return std::get<VarIntValue>(entry.value);
}

void expect_decode_error(const MessageDecodeResult& result) {
    EXPECT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_FALSE(std::holds_alternative<DraftAmbiguity>(result));
}

template <class Result>
void expect_error_code(const Result& result, DecodeErrorCode code) {
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, code);
}

TEST(Draft18MessagesTest, DecodesAndEncodesEmptySetup) {
    const auto encoded = bytes({0xaf, 0x00, 0x00, 0x00});
    Cursor input(encoded);
    const auto decoded = decode_message(StreamRole::Control, input, {});
    EXPECT_TRUE(require_setup(decoded).options.empty());
    EXPECT_EQ(input.remaining(), 0u);

    ByteWriter output(encoded.size());
    ASSERT_TRUE(encode_message(Message{SetupMessage{}}, output).has_value());
    expect_bytes(output.bytes(), encoded);
}

TEST(Draft18MessagesTest, DecodesEveryKnownSetupOptionFromHandAuthoredBytes) {
    const auto encoded = bytes({
        0xaf, 0x00, 0x00, 0x14,
        0x01, 0x02, '/', 'x',
        0x02, 0x02, 0xaa, 0xbb,
        0x01, 0x25,
        0x01, 0x03, 'm', 'o', 'q',
        0x02, 0x03, 'i', 'm', 'p',
    });
    Cursor input(encoded);
    const auto decoded = decode_message(StreamRole::Control, input, {});
    const auto& options = require_setup(decoded).options;
    ASSERT_EQ(options.size(), 5u);
    EXPECT_EQ(options[0].type, 0x01u);
    expect_bytes(require_bytes(options[0]).bytes, bytes({'/', 'x'}));
    EXPECT_EQ(options[1].type, 0x03u);
    expect_bytes(require_bytes(options[1]).bytes, bytes({0xaa, 0xbb}));
    EXPECT_EQ(options[2].type, 0x04u);
    EXPECT_EQ(require_integer(options[2]).value, 37u);
    expect_bytes(require_integer(options[2]).raw_bytes, bytes({0x25}));
    EXPECT_EQ(options[3].type, 0x05u);
    expect_bytes(require_bytes(options[3]).bytes, bytes({'m', 'o', 'q'}));
    EXPECT_EQ(options[4].type, 0x07u);
    expect_bytes(require_bytes(options[4]).bytes, bytes({'i', 'm', 'p'}));

    ByteWriter output(encoded.size());
    ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
    expect_bytes(output.bytes(), encoded);
}

TEST(Draft18MessagesTest, PreservesUnknownOddEvenAndDuplicateOptionsInOrder) {
    const auto encoded = bytes({
        0xaf, 0x00, 0x00, 0x0a,
        0x09, 0x02, 0xde, 0xad,
        0x01, 0x2a,
        0x00, 0x2b,
        0x01, 0x00,
    });
    Cursor input(encoded);
    const auto decoded = decode_message(StreamRole::Control, input, {});
    const auto& options = require_setup(decoded).options;
    ASSERT_EQ(options.size(), 4u);
    EXPECT_EQ(options[0].type, 9u);
    expect_bytes(require_bytes(options[0]).bytes, bytes({0xde, 0xad}));
    EXPECT_EQ(options[1].type, 10u);
    EXPECT_EQ(require_integer(options[1]).value, 42u);
    EXPECT_EQ(options[2].type, 10u);
    EXPECT_EQ(require_integer(options[2]).value, 43u);
    EXPECT_EQ(options[3].type, 11u);
    EXPECT_TRUE(require_bytes(options[3]).bytes.empty());

    ByteWriter output(encoded.size());
    ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
    expect_bytes(output.bytes(), encoded);
}

TEST(Draft18MessagesTest, AcceptsRepeatedAuthorizationTokens) {
    const auto encoded = bytes({
        0xaf, 0x00, 0x00, 0x06,
        0x03, 0x01, 0xaa,
        0x00, 0x01, 0xbb,
    });
    Cursor input(encoded);
    const auto decoded = decode_message(StreamRole::Control, input, {});
    const auto& options = require_setup(decoded).options;
    ASSERT_EQ(options.size(), 2u);
    EXPECT_EQ(options[0].type, 3u);
    EXPECT_EQ(options[1].type, 3u);
}

TEST(Draft18MessagesTest, RejectsDuplicateKnownNonRepeatableOptionsAtomically) {
    const std::vector<std::vector<std::byte>> encoded{
        bytes({0xaf, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00}),
        bytes({0xaf, 0x00, 0x00, 0x04, 0x04, 0x01, 0x00, 0x02}),
        bytes({0xaf, 0x00, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00}),
        bytes({0xaf, 0x00, 0x00, 0x04, 0x07, 0x00, 0x00, 0x00}),
    };
    for (const auto& frame : encoded) {
        Cursor input(frame, 50);
        expect_decode_error(decode_message(StreamRole::Control, input, {}));
        EXPECT_EQ(input.offset(), 50u);
    }
}

TEST(Draft18MessagesTest, AcceptsNonMinimalVi64ButEmitsCanonicalEncoding) {
    const auto encoded = bytes({
        0xc0, 0x2f, 0x00, 0x00, 0x04,
        0x80, 0x04, 0x80, 0x25,
    });
    Cursor input(encoded);
    const auto decoded = decode_message(StreamRole::Control, input, {});
    const auto& options = require_setup(decoded).options;
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(options[0].type, 4u);
    EXPECT_EQ(require_integer(options[0]).value, 37u);
    expect_bytes(require_integer(options[0]).raw_bytes, bytes({0x80, 0x25}));

    ByteWriter output(16);
    ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
    expect_bytes(output.bytes(), bytes({0xaf, 0x00, 0x00, 0x02, 0x04, 0x25}));
}

TEST(Draft18MessagesTest, EveryFrameTruncationNeedsMoreWithoutConsumption) {
    const auto encoded = bytes({
        0xaf, 0x00, 0x00, 0x06,
        0x01, 0x04, 'p', 'a', 't', 'h',
    });
    for (std::size_t available = 0; available < encoded.size(); ++available) {
        Cursor input(std::span<const std::byte>(encoded).first(available), 100);
        const auto result = decode_message(StreamRole::Control, input, {});
        ASSERT_TRUE(std::holds_alternative<NeedMore>(result)) << available;
        EXPECT_EQ(input.offset(), 100u) << available;
    }
}

TEST(Draft18MessagesTest, EveryKvpTruncationNeedsMoreWithoutConsumption) {
    const auto encoded = bytes({0x01, 0x03, 0xaa, 0xbb, 0xcc});
    for (std::size_t available = 0; available < encoded.size(); ++available) {
        Cursor input(std::span<const std::byte>(encoded).first(available), 25);
        const auto result = decode_key_value_pairs(input, encoded.size(), {});
        ASSERT_TRUE(std::holds_alternative<NeedMore>(result)) << available;
        EXPECT_EQ(input.offset(), 25u);
    }
}

TEST(Draft18MessagesTest, LeavesTrailingFrameUntouched) {
    const auto encoded = bytes({
        0xaf, 0x00, 0x00, 0x00,
        0xaf, 0x00, 0x00, 0x00,
    });
    Cursor input(encoded);
    EXPECT_TRUE(std::holds_alternative<Message>(
        decode_message(StreamRole::Control, input, {})));
    EXPECT_EQ(input.offset(), 4u);
    EXPECT_EQ(input.remaining(), 4u);
    EXPECT_TRUE(std::holds_alternative<Message>(
        decode_message(StreamRole::Control, input, {})));
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Draft18MessagesTest, HandlesZeroAndMaximumPayloadLengths) {
    const auto empty = bytes({0xaf, 0x00, 0x00, 0x00});
    Cursor empty_input(empty);
    EXPECT_TRUE(require_setup(decode_message(StreamRole::Control, empty_input, {}))
                    .options.empty());

    std::vector<std::byte> maximum{std::byte{0xaf}, std::byte{0x00},
                                   std::byte{0xff}, std::byte{0xff},
                                   std::byte{0x09}, std::byte{0xc0},
                                   std::byte{0xff}, std::byte{0xfb}};
    maximum.resize(4u + 65'535u, std::byte{0xa5});
    Cursor maximum_input(maximum);
    const auto maximum_result = decode_message(StreamRole::Control, maximum_input, {});
    const auto& options = require_setup(maximum_result).options;
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(require_bytes(options[0]).bytes.size(), 65'531u);
    EXPECT_EQ(maximum_input.remaining(), 0u);
}

TEST(Draft18MessagesTest, DistinguishesOuterTruncationFromShortDeclaredPayload) {
    const auto too_long = bytes({0xaf, 0x00, 0x00, 0x02, 0x01});
    Cursor long_input(too_long, 10);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(
        decode_message(StreamRole::Control, long_input, {})));
    EXPECT_EQ(long_input.offset(), 10u);

    const auto too_short = bytes({0xaf, 0x00, 0x00, 0x01, 0x01, 0x00});
    Cursor short_input(too_short, 20);
    expect_decode_error(decode_message(StreamRole::Control, short_input, {}));
    EXPECT_EQ(short_input.offset(), 20u);
}

TEST(Draft18MessagesTest, RejectsDeltaOverflowAndConfiguredOddValueLimit) {
    const auto overflow = bytes({
        0xaf, 0x00, 0x00, 0x0b,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x01,
    });
    Cursor overflow_input(overflow, 30);
    expect_decode_error(decode_message(StreamRole::Control, overflow_input, {}));
    EXPECT_EQ(overflow_input.offset(), 30u);

    const auto over_limit = bytes({0xaf, 0x00, 0x00, 0x05,
                                   0x09, 0x03, 0x01, 0x02, 0x03});
    Cursor limited_input(over_limit, 40);
    Limits limits;
    limits.maximum_odd_value_length = 2;
    const auto result = decode_message(StreamRole::Control, limited_input, limits);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code,
              DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(limited_input.offset(), 40u);
}

TEST(Draft18MessagesTest, RejectsWrongRoleReservedStaleAndUnknownTypes) {
    const auto setup = bytes({0xaf, 0x00, 0x00, 0x00});
    Cursor request_input(setup);
    expect_decode_error(decode_message(StreamRole::Request, request_input, {}));
    EXPECT_EQ(request_input.offset(), 0u);

    const std::vector<std::vector<std::byte>> rejected{
        bytes({0x01, 0x00, 0x00}),
        bytes({0x20, 0x00, 0x00}),
        bytes({0x21, 0x00, 0x00}),
        bytes({0x40, 0x00, 0x00}),
        bytes({0x41, 0x00, 0x00}),
        bytes({0x1e, 0x00, 0x00}),
        bytes({0x7f, 0x00, 0x00}),
    };
    for (const auto& frame : rejected) {
        Cursor input(frame, 60);
        expect_decode_error(decode_message(StreamRole::Control, input, {}));
        EXPECT_EQ(input.offset(), 60u);
    }
}

TEST(Draft18MessagesTest, RejectsInvalidTypeAfterHeaderWithoutWaitingForPayload) {
    const std::vector<std::vector<std::byte>> rejected{
        bytes({0x1e, 0xff, 0xff}),
        bytes({0x01, 0xff, 0xff}),
        bytes({0x7f, 0xff, 0xff}),
    };
    for (const auto& header : rejected) {
        Cursor input(header, 70);
        expect_decode_error(decode_message(StreamRole::Control, input, {}));
        EXPECT_EQ(input.offset(), 70u);
    }
}

TEST(Draft18MessagesTest, RejectsWrongPhysicalRoleWithoutWaitingForPayload) {
    const auto setup_header = bytes({0xaf, 0x00, 0xff, 0xff});
    Cursor input(setup_header, 80);
    expect_decode_error(decode_message(StreamRole::Request, input, {}));
    EXPECT_EQ(input.offset(), 80u);
}

TEST(Draft18MessagesTest, TrackPropertiesPreserveUnknownMandatoryAndRawValues) {
    const auto encoded = bytes({
        0x0a, 0x80, 0x25,
        0xc0, 0x3f, 0xf6, 0x01,
    });
    Cursor input(encoded);
    const auto decoded = decode_track_properties(input, encoded.size(), {});
    ASSERT_TRUE(std::holds_alternative<TrackProperties>(decoded));
    const auto& properties = std::get<TrackProperties>(decoded).entries;
    ASSERT_EQ(properties.size(), 2u);
    EXPECT_EQ(properties[0].type, 10u);
    EXPECT_EQ(require_integer(properties[0]).value, 37u);
    expect_bytes(require_integer(properties[0]).raw_bytes, bytes({0x80, 0x25}));
    EXPECT_EQ(properties[1].type, 0x4000u);
    EXPECT_TRUE(is_mandatory_track_property(properties[1]));
    expect_bytes(require_integer(properties[1]).raw_bytes, bytes({0x01}));
}

TEST(Draft18MessagesTest, DraftAmbiguityCannotBeConfusedWithPeerDecodeError) {
    const MessageDecodeResult ambiguity =
        DraftAmbiguity{72, "draft does not specify parameter encoding"};
    EXPECT_TRUE(std::holds_alternative<DraftAmbiguity>(ambiguity));
    EXPECT_FALSE(std::holds_alternative<DecodeError>(ambiguity));

    const MessageDecodeResult peer_error =
        DecodeError{DecodeErrorCode::InvalidValue, 72, "peer sent reserved type"};
    EXPECT_TRUE(std::holds_alternative<DecodeError>(peer_error));
    EXPECT_FALSE(std::holds_alternative<DraftAmbiguity>(peer_error));
}

TEST(Draft18MessagesTest, EncodingFailureIsAtomicAtCapacityBoundary) {
    const SetupMessage setup{{KeyValuePair{1, ByteValue{bytes({'a', 'b'})}}}};
    ByteWriter output(6);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    const auto before = std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
    const auto result = encode_message(Message{setup}, output);
    EXPECT_FALSE(result.has_value());
    expect_bytes(output.bytes(), before);
}

TEST(Draft18MessagesTest, EncodingFromDecodedSourceDoesNotAliasDestinationStorage) {
    ByteWriter output(16);
    ASSERT_TRUE(output.append_bytes(bytes({0xaf, 0x00, 0x00, 0x03,
                                          0x01, 0x01, 0xaa})));
    Cursor source(output.bytes());
    const auto decoded = decode_message(StreamRole::Control, source, {});
    ASSERT_TRUE(std::holds_alternative<Message>(decoded));
    ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
    expect_bytes(output.bytes(), bytes({0xaf, 0x00, 0x00, 0x03, 0x01, 0x01, 0xaa,
                                       0xaf, 0x00, 0x00, 0x03, 0x01, 0x01, 0xaa}));
}

template <class T>
void expect_peer_error(const DraftDecodeResult<T>& result) {
    EXPECT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_FALSE(std::holds_alternative<DraftAmbiguity>(result));
}

TEST(Draft18StructuresTest, LocationAcceptsNonMinimalAndEncodesCanonically) {
    const auto encoded = bytes({0x80, 0x25, 0x07});
    Cursor input(encoded, 10);
    const auto decoded = decode_location(input);
    ASSERT_TRUE(std::holds_alternative<Location>(decoded));
    EXPECT_EQ(std::get<Location>(decoded).group, 37u);
    EXPECT_EQ(std::get<Location>(decoded).object, 7u);
    EXPECT_EQ(input.offset(), 13u);

    ByteWriter output(2);
    ASSERT_TRUE(encode_location(std::get<Location>(decoded), output).has_value());
    expect_bytes(output.bytes(), bytes({0x25, 0x07}));
}

TEST(Draft18StructuresTest, LocationTruncationAndCapacityFailureAreAtomic) {
    const auto encoded = bytes({0x80, 0x25, 0x80, 0x26});
    for (std::size_t available = 0; available < encoded.size(); ++available) {
        Cursor input(std::span<const std::byte>(encoded).first(available), 20);
        EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_location(input)));
        EXPECT_EQ(input.offset(), 20u);
    }
    ByteWriter output(1);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    EXPECT_FALSE(encode_location(Location{1, 2}, output).has_value());
    expect_bytes(output.bytes(), bytes({0xcc}));
}

TEST(Draft18StructuresTest, NamespaceAcceptsZeroAndThirtyTwoArbitraryFields) {
    const auto empty = bytes({0x00});
    Cursor empty_input(empty);
    const auto empty_result = decode_track_namespace(empty_input, {});
    ASSERT_TRUE(std::holds_alternative<TrackNamespace>(empty_result));
    EXPECT_TRUE(std::get<TrackNamespace>(empty_result).fields.empty());

    std::vector<std::byte> encoded{std::byte{0x20}};
    for (std::size_t index = 0; index < 32; ++index) {
        encoded.push_back(std::byte{0x01});
        encoded.push_back(static_cast<std::byte>(index));
    }
    Cursor input(encoded);
    const auto decoded = decode_track_namespace(input, {});
    ASSERT_TRUE(std::holds_alternative<TrackNamespace>(decoded));
    const auto& fields = std::get<TrackNamespace>(decoded).fields;
    ASSERT_EQ(fields.size(), 32u);
    expect_bytes(fields[31], bytes({31}));

    ByteWriter output(encoded.size());
    ASSERT_TRUE(encode_track_namespace(std::get<TrackNamespace>(decoded), output)
                    .has_value());
    expect_bytes(output.bytes(), encoded);
}

TEST(Draft18StructuresTest, NamespaceRejectsCountEmptyFieldAndValueOverflowAtomically) {
    const std::vector<std::vector<std::byte>> invalid{
        bytes({0x21}),
        bytes({0x01, 0x00}),
    };
    for (const auto& encoded : invalid) {
        Cursor input(encoded, 30);
        expect_error_code(decode_track_namespace(input, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 30u);
    }

    std::vector<std::byte> maximum{std::byte{0x01}, std::byte{0xc0},
                                   std::byte{0x10}, std::byte{0x00}};
    maximum.resize(4u + 4'096u, std::byte{0xff});
    Cursor maximum_input(maximum);
    const auto maximum_result = decode_track_namespace(maximum_input, {});
    ASSERT_TRUE(std::holds_alternative<TrackNamespace>(maximum_result));

    std::vector<std::byte> overflow{std::byte{0x01}, std::byte{0xc0},
                                    std::byte{0x10}, std::byte{0x01}};
    overflow.resize(4u + 4'097u, std::byte{0xee});
    Cursor overflow_input(overflow, 40);
    expect_error_code(decode_track_namespace(overflow_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(overflow_input.offset(), 40u);
}

TEST(Draft18StructuresTest, TrackNameAllowsEmptyAndEnforcesFullNameBoundary) {
    TrackNamespace name_space{{bytes({'n'})}};
    const auto empty = bytes({0x00});
    Cursor empty_input(empty);
    const auto empty_result = decode_track_name(empty_input, name_space, {});
    ASSERT_TRUE(std::holds_alternative<TrackName>(empty_result));
    EXPECT_TRUE(std::get<TrackName>(empty_result).bytes.empty());

    TrackNamespace large_namespace{{std::vector<std::byte>(4'095u, std::byte{0xaa})}};
    const auto one = bytes({0x01, 0x00});
    Cursor boundary_input(one);
    EXPECT_TRUE(std::holds_alternative<TrackName>(
        decode_track_name(boundary_input, large_namespace, {})));

    const auto two = bytes({0x02, 0x00, 0xff});
    Cursor overflow_input(two, 50);
    expect_error_code(decode_track_name(overflow_input, large_namespace, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(overflow_input.offset(), 50u);

    TrackNamespace oversized_namespace{
        {std::vector<std::byte>(4'097u, std::byte{0xaa})}};
    Cursor oversized_namespace_input(empty, 55);
    expect_error_code(
        decode_track_name(oversized_namespace_input, oversized_namespace, {}),
        DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(oversized_namespace_input.offset(), 55u);

    ByteWriter encoded_name(2);
    ASSERT_TRUE(encode_track_name(TrackName{bytes({0xff})}, large_namespace,
                                  encoded_name)
                    .has_value());
    expect_bytes(encoded_name.bytes(), bytes({0x01, 0xff}));
}

TEST(Draft18StructuresTest, DecodesAndEncodesEverySubscriptionFilterForm) {
    struct Vector {
        std::vector<std::byte> encoded;
        SubscriptionFilterType type;
    };
    const std::vector<Vector> vectors{
        {bytes({0x01}), SubscriptionFilterType::NextGroupStart},
        {bytes({0x02}), SubscriptionFilterType::LargestObject},
        {bytes({0x03, 0x05, 0x06}), SubscriptionFilterType::AbsoluteStart},
        {bytes({0x04, 0x05, 0x06, 0x07}), SubscriptionFilterType::AbsoluteRange},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto decoded = decode_subscription_filter(input, vector.encoded.size());
        ASSERT_TRUE(std::holds_alternative<SubscriptionFilter>(decoded));
        EXPECT_EQ(std::get<SubscriptionFilter>(decoded).type, vector.type);
        ByteWriter output(vector.encoded.size());
        ASSERT_TRUE(encode_subscription_filter(
                        std::get<SubscriptionFilter>(decoded), output)
                        .has_value());
        expect_bytes(output.bytes(), vector.encoded);
    }
}

TEST(Draft18StructuresTest, RejectsInvalidFilterTypeTrailingBytesAndRangeOverflow) {
    const std::vector<std::vector<std::byte>> invalid{
        bytes({}),
        bytes({0x03}),
        bytes({0x03, 0x00}),
        bytes({0x04, 0x00, 0x00}),
        bytes({0x05}),
        bytes({0x01, 0x00}),
        bytes({0x04, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
               0xff, 0x00, 0x01}),
    };
    for (const auto& encoded : invalid) {
        Cursor input(encoded, 60);
        expect_error_code(decode_subscription_filter(input, encoded.size()),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 60u);
    }

    const auto incomplete = bytes({0x04, 0x00});
    Cursor incomplete_input(incomplete, 65);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(
        decode_subscription_filter(incomplete_input, incomplete.size() + 1)));
    EXPECT_EQ(incomplete_input.offset(), 65u);
}

TEST(Draft18StructuresTest, DecodesEveryTokenFormAndPreservesOpaqueValue) {
    struct Vector {
        std::vector<std::byte> encoded;
        TokenAliasType type;
    };
    const std::vector<Vector> vectors{
        {bytes({0x00, 0x25}), TokenAliasType::Delete},
        {bytes({0x01, 0x25, 0x07, 0x61, 0x00, 0xff}), TokenAliasType::Register},
        {bytes({0x02, 0x25}), TokenAliasType::UseAlias},
        {bytes({0x03, 0x07, 0x61, 0x00, 0xff}), TokenAliasType::UseValue},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto decoded = decode_token(input, vector.encoded.size());
        ASSERT_TRUE(std::holds_alternative<Token>(decoded));
        EXPECT_EQ(std::get<Token>(decoded).alias_type, vector.type);
        ByteWriter output(vector.encoded.size());
        ASSERT_TRUE(encode_token(std::get<Token>(decoded), output).has_value());
        expect_bytes(output.bytes(), vector.encoded);
    }
}

TEST(Draft18StructuresTest, TokenRejectsMissingTrailingAndUnknownAliasForms) {
    const std::vector<std::vector<std::byte>> malformed{
        bytes({0x00}),
        bytes({0x00, 0x25, 0x00}),
        bytes({0x01, 0x25}),
        bytes({0x02}),
        bytes({0x02, 0x25, 0x00}),
        bytes({0x03}),
    };
    for (const auto& encoded : malformed) {
        Cursor input(encoded, 70);
        const auto result = decode_token(input, encoded.size());
        expect_peer_error(result);
        EXPECT_EQ(std::get<DecodeError>(result).code,
                  DecodeErrorCode::KeyValueFormattingError);
        EXPECT_EQ(input.offset(), 70u);
    }

    const auto unknown_alias = bytes({0x04});
    Cursor unknown_alias_input(unknown_alias, 70);
    const auto unknown_alias_result =
        decode_token(unknown_alias_input, unknown_alias.size());
    expect_peer_error(unknown_alias_result);
    EXPECT_EQ(std::get<DecodeError>(unknown_alias_result).code,
              DecodeErrorCode::KeyValueFormattingError);
    EXPECT_EQ(unknown_alias_input.offset(), 70u);

    Token invalid_token;
    invalid_token.alias_type = static_cast<TokenAliasType>(4);
    ByteWriter output(16);
    EXPECT_FALSE(encode_token(invalid_token, output).has_value());
    EXPECT_TRUE(output.bytes().empty());
}

TEST(Draft18StructuresTest, ProtocolViolationsRemainDistinctFromTokenFormatting) {
    const auto unknown_filter = bytes({0x05});
    Cursor filter_input(unknown_filter);
    const auto filter_result =
        decode_subscription_filter(filter_input, unknown_filter.size());
    ASSERT_TRUE(std::holds_alternative<DecodeError>(filter_result));
    EXPECT_EQ(std::get<DecodeError>(filter_result).code,
              DecodeErrorCode::ProtocolViolation);

    const auto invalid_namespace = bytes({0x01, 0x00});
    Cursor namespace_input(invalid_namespace);
    const auto namespace_result = decode_track_namespace(namespace_input, {});
    ASSERT_TRUE(std::holds_alternative<DecodeError>(namespace_result));
    EXPECT_EQ(std::get<DecodeError>(namespace_result).code,
              DecodeErrorCode::ProtocolViolation);

    const auto unknown_parameter = bytes({0x05});
    Cursor parameter_input(unknown_parameter);
    const auto parameter_result = decode_parameters(
        parameter_input, 1, ParameterContext::Unresolved, {});
    ASSERT_TRUE(std::holds_alternative<DecodeError>(parameter_result));
    EXPECT_EQ(std::get<DecodeError>(parameter_result).code,
              DecodeErrorCode::ProtocolViolation);
}

TEST(Draft18StructuresTest, RepresentativeStructureTruncationsAreAtomic) {
    const auto namespace_bytes = bytes({0x02, 0x01, 'a', 0x01, 'b'});
    for (std::size_t available = 0; available < namespace_bytes.size(); ++available) {
        Cursor input(std::span<const std::byte>(namespace_bytes).first(available), 80);
        EXPECT_TRUE(std::holds_alternative<NeedMore>(
            decode_track_namespace(input, {})));
        EXPECT_EQ(input.offset(), 80u);
    }
    const auto token_bytes = bytes({0x01, 0x25, 0x07, 0xaa});
    for (std::size_t available = 0; available < token_bytes.size(); ++available) {
        Cursor input(std::span<const std::byte>(token_bytes).first(available), 90);
        EXPECT_TRUE(std::holds_alternative<NeedMore>(
            decode_token(input, token_bytes.size())));
        EXPECT_EQ(input.offset(), 90u);
    }
}

TEST(Draft18ParametersTest, DecodesEveryUnambiguousParameterAndCanonicalizes) {
    const auto encoded = bytes({
        0x02, 0x25,
        0x01, 0x03, 0x03, 0x05, 0xaa,
        0x03, 0x07,
        0x02, 0x08,
        0x01, 0x02, 0x03,
        0x07, 0x01,
        0x10, 0xff,
        0x01, 0x03, 0x03, 0x04, 0x05,
        0x01, 0x02,
        0x10, 0x09,
        0x02, 0x01, 0x01, 'n',
    });
    Cursor input(encoded);
    const auto decoded = decode_parameters(input, 11, ParameterContext::Unresolved, {});
    ASSERT_TRUE(std::holds_alternative<Parameters>(decoded));
    const auto& parameters = std::get<Parameters>(decoded);
    ASSERT_EQ(parameters.size(), 11u);
    EXPECT_EQ(parameters[0].type, 0x02u);
    EXPECT_EQ(parameters[1].type, 0x03u);
    EXPECT_EQ(parameters[10].type, 0x34u);
    EXPECT_EQ(validate_parameter_scope(parameters[0].type,
                                       ParameterContext::Unresolved),
              ParameterScopeResult::Unresolved);

    ByteWriter output(encoded.size());
    ASSERT_TRUE(encode_parameters(parameters, ParameterContext::Unresolved, output)
                    .has_value());
    expect_bytes(output.bytes(), encoded);
}

TEST(Draft18ParametersTest, EachUnambiguousParameterHasIndependentGoldenBytes) {
    struct Vector {
        std::vector<std::byte> encoded;
        std::uint64_t type;
        ParameterContext context;
    };
    const std::vector<Vector> vectors{
        {bytes({0x02, 0x25}), 0x02, ParameterContext::Subscribe},
        {bytes({0x03, 0x02, 0x03, 0x01}), 0x03, ParameterContext::Subscribe},
        {bytes({0x06, 0x25}), 0x06, ParameterContext::Subscribe},
        {bytes({0x08, 0x25}), 0x08, ParameterContext::SubscribeOk},
        {bytes({0x09, 0x01, 0x02}), 0x09, ParameterContext::SubscribeOk},
        {bytes({0x10, 0x01}), 0x10, ParameterContext::Subscribe},
        {bytes({0x20, 0xff}), 0x20, ParameterContext::Subscribe},
        {bytes({0x21, 0x01, 0x01}), 0x21, ParameterContext::Subscribe},
        {bytes({0x22, 0x02}), 0x22, ParameterContext::Subscribe},
        {bytes({0x32, 0x25}), 0x32, ParameterContext::Subscribe},
        {bytes({0x34, 0x01, 0x01, 'n'}), 0x34,
         ParameterContext::RequestUpdateSubscribeNamespace},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto decoded = decode_parameters(input, 1, vector.context, {});
        ASSERT_TRUE(std::holds_alternative<Parameters>(decoded)) << vector.type;
        const auto& parameters = std::get<Parameters>(decoded);
        ASSERT_EQ(parameters.size(), 1u);
        EXPECT_EQ(parameters[0].type, vector.type);
        ByteWriter output(vector.encoded.size());
        ASSERT_TRUE(encode_parameters(parameters, vector.context, output).has_value());
        expect_bytes(output.bytes(), vector.encoded);
    }
}

TEST(Draft18ParametersTest, AcceptsNonMinimalValueAndEmitsCanonicalParameterBytes) {
    const auto encoded = bytes({0x02, 0x80, 0x25});
    Cursor input(encoded);
    const auto decoded =
        decode_parameters(input, 1, ParameterContext::Subscribe, {});
    ASSERT_TRUE(std::holds_alternative<Parameters>(decoded));
    ByteWriter output(2);
    ASSERT_TRUE(encode_parameters(std::get<Parameters>(decoded),
                                  ParameterContext::Subscribe, output)
                    .has_value());
    expect_bytes(output.bytes(), bytes({0x02, 0x25}));
}

TEST(Draft18ParametersTest, ConfiguredLimitBoundsLengthPrefixedFilter) {
    const auto encoded = bytes({0x21, 0x01, 0x01});
    Cursor input(encoded, 95);
    Limits limits;
    limits.maximum_odd_value_length = 0;
    const auto decoded =
        decode_parameters(input, 1, ParameterContext::Subscribe, limits);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code,
              DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(input.offset(), 95u);
}

TEST(Draft18ParametersTest, ScopeMatrixMatchesEveryParameterDefinition) {
    const std::vector<std::pair<std::uint64_t, std::vector<ParameterContext>>> allowed{
        {0x02, {ParameterContext::Subscribe, ParameterContext::PublishOk,
                ParameterContext::RequestUpdateSubscription}},
        {0x03, {ParameterContext::Publish, ParameterContext::Subscribe,
                ParameterContext::RequestUpdateSubscription,
                ParameterContext::RequestUpdateFetch,
                ParameterContext::RequestUpdatePublishNamespace,
                ParameterContext::RequestUpdateSubscribeNamespace,
                ParameterContext::RequestUpdateSubscribeTracks,
                ParameterContext::SubscribeNamespace, ParameterContext::SubscribeTracks,
                ParameterContext::PublishNamespace, ParameterContext::TrackStatus,
                ParameterContext::Fetch}},
        {0x04, {ParameterContext::Subscribe}},
        {0x06, {ParameterContext::Subscribe, ParameterContext::PublishOk,
                ParameterContext::RequestUpdateSubscription}},
        {0x08, {ParameterContext::SubscribeOk, ParameterContext::Publish,
                ParameterContext::PublishOk, ParameterContext::RequestUpdateOk}},
        {0x09, {ParameterContext::SubscribeOk, ParameterContext::Publish,
                ParameterContext::RequestUpdateOk, ParameterContext::TrackStatusOk}},
        {0x0a, {ParameterContext::Fetch}},
        {0x10, {ParameterContext::Subscribe,
                ParameterContext::RequestUpdateSubscription, ParameterContext::Publish,
                ParameterContext::PublishOk, ParameterContext::SubscribeTracks}},
        {0x20, {ParameterContext::Subscribe, ParameterContext::Fetch,
                ParameterContext::RequestUpdateSubscription,
                ParameterContext::RequestUpdateFetch, ParameterContext::PublishOk}},
        {0x21, {ParameterContext::Subscribe, ParameterContext::PublishOk,
                ParameterContext::RequestUpdateSubscription}},
        {0x22, {ParameterContext::Subscribe, ParameterContext::PublishOk,
                ParameterContext::Fetch}},
        {0x32, {ParameterContext::PublishOk, ParameterContext::Subscribe,
                ParameterContext::RequestUpdateSubscription}},
        {0x34, {ParameterContext::RequestUpdateSubscribeNamespace,
                ParameterContext::RequestUpdateSubscribeTracks}},
    };
    for (const auto& [type, contexts] : allowed) {
        for (const auto context : contexts) {
            EXPECT_EQ(validate_parameter_scope(type, context),
                      ParameterScopeResult::Allowed)
                << type << ' ' << static_cast<int>(context);
        }
        EXPECT_EQ(validate_parameter_scope(type, ParameterContext::FetchOk),
                  ParameterScopeResult::Forbidden)
            << type;
        EXPECT_EQ(validate_parameter_scope(type, ParameterContext::Unresolved),
                  ParameterScopeResult::Unresolved)
            << type;
    }
}

TEST(Draft18ParametersTest, RepeatedAuthorizationTokensRemainOrdered) {
    const auto encoded = bytes({
        0x03, 0x02, 0x03, 0x01,
        0x00, 0x03, 0x03, 0x01, 0xaa,
    });
    Cursor input(encoded);
    const auto decoded = decode_parameters(input, 2, ParameterContext::Subscribe, {});
    ASSERT_TRUE(std::holds_alternative<Parameters>(decoded));
    const auto& parameters = std::get<Parameters>(decoded);
    ASSERT_EQ(parameters.size(), 2u);
    EXPECT_EQ(parameters[0].type, 3u);
    EXPECT_EQ(parameters[1].type, 3u);
}

TEST(Draft18ParametersTest, ProtocolViolationPathsUseExactSessionError) {
    struct InvalidParameterList {
        std::vector<std::byte> encoded;
        std::uint64_t count;
        ParameterContext context;
    };
    const std::vector<InvalidParameterList> invalid{
        {bytes({0x02, 0x01, 0x00, 0x02}), 2, ParameterContext::Unresolved},
        {bytes({0x05}), 1, ParameterContext::Unresolved},
        {bytes({0x34, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                0xff, 0xff}), 2, ParameterContext::Unresolved},
        {bytes({0x02, 0x01}), 1, ParameterContext::Fetch},
        {bytes({0x10, 0x02}), 1, ParameterContext::Unresolved},
        {bytes({0x03, 0xc1, 0x00, 0x00}), 1,
         ParameterContext::Unresolved},
        {bytes({0x21, 0xc1, 0x00, 0x00}), 1,
         ParameterContext::Unresolved},
        {bytes({0x21, 0x02, 0x03, 0x00}), 1,
         ParameterContext::Unresolved},
        {bytes({0x21, 0x02, 0x01, 0x00}), 1,
         ParameterContext::Unresolved},
        {bytes({0x22, 0x00}), 1, ParameterContext::Unresolved},
    };
    for (const auto& test : invalid) {
        Cursor input(test.encoded, 100);
        expect_error_code(
            decode_parameters(input, test.count, test.context, {}),
            DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 100u);
    }
}

TEST(Draft18ParametersTest, TimeoutAmbiguityFollowsKnownScopeValidation) {
    const std::vector<std::pair<std::vector<std::byte>, ParameterContext>> ambiguous{
        {bytes({0x04}), ParameterContext::Subscribe},
        {bytes({0x04}), ParameterContext::Unresolved},
        {bytes({0x0a}), ParameterContext::Fetch},
        {bytes({0x0a}), ParameterContext::Unresolved},
    };
    for (const auto& [encoded, context] : ambiguous) {
        Cursor input(encoded, 110);
        const auto result = decode_parameters(input, 1, context, {});
        EXPECT_TRUE(std::holds_alternative<DraftAmbiguity>(result));
        EXPECT_FALSE(std::holds_alternative<DecodeError>(result));
        EXPECT_EQ(input.offset(), 110u);
    }

    const auto forbidden_bytes = bytes({0x04});
    Cursor forbidden(forbidden_bytes, 120);
    expect_error_code(
        decode_parameters(forbidden, 1, ParameterContext::Fetch, {}),
        DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(forbidden.offset(), 120u);

    const Parameters timeout{
        Parameter{0x04, VarIntParameterValue{0}},
    };
    ByteWriter output(8);
    const auto encode_result =
        encode_parameters(timeout, ParameterContext::Subscribe, output);
    EXPECT_TRUE(encode_result.is_ambiguity());
    EXPECT_FALSE(encode_result.has_value());
    EXPECT_TRUE(output.bytes().empty());
}

TEST(Draft18ParametersTest, TruncationCountFeasibilityAndCapacityAreAtomic) {
    const auto encoded = bytes({0x02, 0x80, 0x25, 0x06, 0x07});
    for (std::size_t available = 0; available < encoded.size(); ++available) {
        Cursor input(std::span<const std::byte>(encoded).first(available), 130);
        EXPECT_TRUE(std::holds_alternative<NeedMore>(
            decode_parameters(input, 2, ParameterContext::Unresolved, {})));
        EXPECT_EQ(input.offset(), 130u);
    }

    Limits limits;
    limits.maximum_parameter_count = 2;
    Cursor excessive(encoded, 140);
    expect_peer_error(
        decode_parameters(excessive, 3, ParameterContext::Unresolved, limits));
    EXPECT_EQ(excessive.offset(), 140u);

    const Parameters parameters{
        Parameter{0x02, VarIntParameterValue{37}},
    };
    ByteWriter output(1);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    EXPECT_FALSE(encode_parameters(parameters, ParameterContext::Subscribe, output)
                     .has_value());
    expect_bytes(output.bytes(), bytes({0xcc}));
}

}  // namespace
}  // namespace moq::interop::wire::draft18
