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

}  // namespace
}  // namespace moq::interop::wire::draft18
