#include "moq/interop/wire/draft18/messages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

template <class Encoder>
void expect_atomic_encode_error(Encoder&& encoder, EncodeErrorCode code) {
    ByteWriter output(65'535);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    ASSERT_TRUE(output.append_byte(std::byte{0xdd}));
    const auto before =
        std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
    const auto result = encoder(output);
    EXPECT_FALSE(result.has_value());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code, code);
    expect_bytes(output.bytes(), before);
}

const SetupMessage& require_setup(const MessageDecodeResult& result) {
    EXPECT_TRUE(std::holds_alternative<Message>(result));
    const auto& message = std::get<Message>(result);
    EXPECT_TRUE(std::holds_alternative<SetupMessage>(message));
    return std::get<SetupMessage>(message);
}

template <class T>
const T& require_message(const MessageDecodeResult& result) {
    EXPECT_TRUE(std::holds_alternative<Message>(result));
    const auto& message = std::get<Message>(result);
    EXPECT_TRUE(std::holds_alternative<T>(message));
    return std::get<T>(message);
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

struct MessageAuditVector {
    std::uint64_t type;
    StreamRole role;
    std::vector<std::byte> frame;
    std::size_t variant_index;
    bool also_valid_on_other_role;
};

const std::vector<MessageAuditVector>& message_audit_vectors() {
    static const std::vector<MessageAuditVector> vectors{
        {0x2f00, StreamRole::Control, bytes({0xaf, 0x00, 0x00, 0x00}),
         Message{SetupMessage{}}.index(), false},
        {0x10, StreamRole::Control,
         bytes({0x10, 0x00, 0x03, 0x00, 0x00, 0x02}),
         Message{GoawayMessage{}}.index(), true},
        {0x10, StreamRole::Request, bytes({0x10, 0x00, 0x02, 0x00, 0x00}),
         Message{GoawayMessage{}}.index(), true},
        {0x03, StreamRole::Request,
         bytes({0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}),
         Message{SubscribeMessage{}}.index(), false},
        {0x04, StreamRole::Request, bytes({0x04, 0x00, 0x02, 0x00, 0x00}),
         Message{SubscribeOkMessage{}}.index(), false},
        {0x1d, StreamRole::Request,
         bytes({0x1d, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00}),
         Message{PublishMessage{}}.index(), false},
        {0x0b, StreamRole::Request,
         bytes({0x0b, 0x00, 0x03, 0x00, 0x00, 0x00}),
         Message{PublishDoneMessage{}}.index(), false},
        {0x16, StreamRole::Request,
         bytes({0x16, 0x00, 0x05, 0x0a, 0x02, 0x02, 0x05, 0x00}),
         Message{FetchMessage{}}.index(), false},
        {0x18, StreamRole::Request,
         bytes({0x18, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}),
         Message{FetchOkMessage{}}.index(), false},
        {0x0d, StreamRole::Request,
         bytes({0x0d, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}),
         Message{TrackStatusMessage{}}.index(), false},
        {0x06, StreamRole::Request,
         bytes({0x06, 0x00, 0x03, 0x00, 0x00, 0x00}),
         Message{PublishNamespaceMessage{}}.index(), false},
        {0x50, StreamRole::Request,
         bytes({0x50, 0x00, 0x03, 0x00, 0x00, 0x00}),
         Message{SubscribeNamespaceMessage{}}.index(), false},
        {0x51, StreamRole::Request,
         bytes({0x51, 0x00, 0x03, 0x00, 0x00, 0x00}),
         Message{SubscribeTracksMessage{}}.index(), false},
        {0x08, StreamRole::Request, bytes({0x08, 0x00, 0x01, 0x00}),
         Message{NamespaceMessage{}}.index(), false},
        {0x0e, StreamRole::Request, bytes({0x0e, 0x00, 0x01, 0x00}),
         Message{NamespaceDoneMessage{}}.index(), false},
        {0x0f, StreamRole::Request, bytes({0x0f, 0x00, 0x02, 0x00, 0x00}),
         Message{PublishBlockedMessage{}}.index(), false},
        {0x02, StreamRole::Request, bytes({0x02, 0x00, 0x02, 0x00, 0x00}),
         Message{RequestUpdateMessage{}}.index(), false},
        {0x07, StreamRole::Request, bytes({0x07, 0x00, 0x01, 0x00}),
         Message{RequestOkMessage{}}.index(), false},
        {0x05, StreamRole::Request,
         bytes({0x05, 0x00, 0x03, 0x00, 0x00, 0x00}),
         Message{RequestErrorMessage{}}.index(), false},
    };
    return vectors;
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

TEST(Draft18MessagesTest, EachRegisteredSetupOptionHasIndependentGoldenBytes) {
    struct Vector {
        std::uint64_t type;
        std::vector<std::byte> frame;
    };
    const std::vector<Vector> vectors{
        {0x01, bytes({0xaf, 0x00, 0x00, 0x02, 0x01, 0x00})},
        {0x03, bytes({0xaf, 0x00, 0x00, 0x02, 0x03, 0x00})},
        {0x04, bytes({0xaf, 0x00, 0x00, 0x02, 0x04, 0x00})},
        {0x05, bytes({0xaf, 0x00, 0x00, 0x02, 0x05, 0x00})},
        {0x07, bytes({0xaf, 0x00, 0x00, 0x02, 0x07, 0x00})},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.frame);
        const auto decoded = decode_message(StreamRole::Control, input, {});
        const auto& setup = require_setup(decoded);
        ASSERT_EQ(setup.options.size(), 1u);
        EXPECT_EQ(setup.options[0].type, vector.type);
        ByteWriter output(vector.frame.size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), vector.frame);
    }
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

TEST(Draft18MessagesTest, PreservesDuplicateKnownNonRepeatableOptionsForScoring) {
    const std::vector<std::vector<std::byte>> encoded{
        bytes({0xaf, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00}),
        bytes({0xaf, 0x00, 0x00, 0x04, 0x04, 0x01, 0x00, 0x02}),
        bytes({0xaf, 0x00, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00}),
        bytes({0xaf, 0x00, 0x00, 0x04, 0x07, 0x00, 0x00, 0x00}),
    };
    for (const auto& frame : encoded) {
        Cursor input(frame);
        const auto result = decode_message(StreamRole::Control, input, {});
        const auto& setup = require_setup(result);
        ASSERT_EQ(setup.options.size(), 2u);
        EXPECT_EQ(setup.options[0].type, setup.options[1].type);
        EXPECT_EQ(input.remaining(), 0u);

        ByteWriter output(frame.size());
        const auto encode_result = encode_message(std::get<Message>(result), output);
        EXPECT_FALSE(encode_result.has_value());
        ASSERT_NE(encode_result.error(), nullptr);
        EXPECT_EQ(encode_result.error()->code, EncodeErrorCode::InvalidValue);
        EXPECT_TRUE(output.bytes().empty());
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

TEST(Draft18MessagesTest, InvalidTypedKvpEncodersAreAtomic) {
    const std::vector<KeyValuePairs> invalid{
        {KeyValuePair{2, VarIntValue{1, {}}},
         KeyValuePair{1, ByteValue{bytes({0xaa})}}},
        {KeyValuePair{2, ByteValue{bytes({0xaa})}}},
        {KeyValuePair{1, VarIntValue{1, {}}}},
        {KeyValuePair{1, ByteValue{
                             std::vector<std::byte>(65'536u, std::byte{0xaa})}}},
    };
    const std::vector<EncodeErrorCode> codes{
        EncodeErrorCode::InvalidValue,
        EncodeErrorCode::InvalidValue,
        EncodeErrorCode::InvalidValue,
        EncodeErrorCode::PayloadTooLarge,
    };
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        expect_atomic_encode_error(
            [&](ByteWriter& output) {
                return encode_key_value_pairs(invalid[index], output);
            },
            codes[index]);
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

TEST(Draft18RequestMessagesTest, DecodesAndEncodesEveryOpeningMessage) {
    const std::vector<std::byte> subscribe =
        bytes({0x03, 0x00, 0x09, 0x02, 0x01, 0x01, 'n', 0x01, 't',
               0x01, 0x20, 0x07});
    Cursor subscribe_input(subscribe);
    const auto subscribe_result =
        decode_message(StreamRole::Request, subscribe_input, {});
    const auto& subscribe_message =
        require_message<SubscribeMessage>(subscribe_result);
    EXPECT_EQ(subscribe_message.request_id, 2u);
    ASSERT_EQ(subscribe_message.track_namespace.fields.size(), 1u);
    expect_bytes(subscribe_message.track_namespace.fields[0], bytes({'n'}));
    expect_bytes(subscribe_message.track_name.bytes, bytes({'t'}));
    ASSERT_EQ(subscribe_message.parameters.size(), 1u);
    EXPECT_EQ(subscribe_message.parameters[0].type, 0x20u);

    const std::vector<std::byte> publish =
        bytes({0x1d, 0x00, 0x11, 0x03, 0x01, 0x01, 'n', 0x01, 't',
               0x05, 0x00, 0x02, 0x09, 0x01, 0x02, 0xaa, 0xbb,
               0xbf, 0xfd, 0x07});
    Cursor publish_input(publish);
    const auto publish_result = decode_message(StreamRole::Request, publish_input, {});
    const auto& publish_message = require_message<PublishMessage>(publish_result);
    EXPECT_EQ(publish_message.request_id, 3u);
    EXPECT_EQ(publish_message.track_alias, 5u);
    ASSERT_EQ(publish_message.track_properties.entries.size(), 3u);
    EXPECT_EQ(publish_message.track_properties.entries[0].type, 2u);
    EXPECT_EQ(publish_message.track_properties.entries[1].type, 3u);
    EXPECT_EQ(publish_message.track_properties.entries[2].type, 0x4000u);
    EXPECT_TRUE(is_mandatory_track_property(
        publish_message.track_properties.entries[2]));

    const std::vector<std::byte> track_status =
        bytes({0x0d, 0x00, 0x07, 0x04, 0x01, 0x01, 'n', 0x01, 't', 0x00});
    Cursor status_input(track_status);
    EXPECT_EQ(require_message<TrackStatusMessage>(
                  decode_message(StreamRole::Request, status_input, {}))
                  .request_id,
              4u);

    const std::vector<std::byte> publish_namespace =
        bytes({0x06, 0x00, 0x05, 0x05, 0x01, 0x01, 'n', 0x00});
    Cursor publish_namespace_input(publish_namespace);
    EXPECT_EQ(require_message<PublishNamespaceMessage>(decode_message(
                  StreamRole::Request, publish_namespace_input, {}))
                  .request_id,
              5u);

    const std::vector<std::byte> subscribe_namespace =
        bytes({0x50, 0x00, 0x03, 0x06, 0x00, 0x00});
    Cursor subscribe_namespace_input(subscribe_namespace);
    EXPECT_TRUE(require_message<SubscribeNamespaceMessage>(decode_message(
                    StreamRole::Request, subscribe_namespace_input, {}))
                    .track_namespace_prefix.fields.empty());

    const std::vector<std::byte> subscribe_tracks =
        bytes({0x51, 0x00, 0x03, 0x07, 0x00, 0x00});
    Cursor subscribe_tracks_input(subscribe_tracks);
    EXPECT_TRUE(require_message<SubscribeTracksMessage>(decode_message(
                    StreamRole::Request, subscribe_tracks_input, {}))
                    .track_namespace_prefix.fields.empty());

    for (const auto* result : {&subscribe_result, &publish_result}) {
        ByteWriter output(64);
        ASSERT_TRUE(encode_message(std::get<Message>(*result), output).has_value());
        const auto& expected = result == &subscribe_result ? subscribe : publish;
        expect_bytes(output.bytes(), expected);
    }
    for (const auto& encoded : {track_status, publish_namespace,
                                subscribe_namespace, subscribe_tracks}) {
        Cursor input(encoded);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(decoded));
        ByteWriter output(encoded.size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), encoded);
    }
}

TEST(Draft18RequestMessagesTest, DecodesAndEncodesEveryFetchForm) {
    const std::vector<std::vector<std::byte>> vectors{
        bytes({0x16, 0x00, 0x0c, 0x08, 0x01, 0x01, 0x01, 'n',
               0x01, 't', 0x01, 0x02, 0x03, 0x00, 0x00}),
        bytes({0x16, 0x00, 0x05, 0x0a, 0x02, 0x02, 0x05, 0x00}),
        bytes({0x16, 0x00, 0x05, 0x0c, 0x03, 0x02, 0x05, 0x00}),
    };
    for (std::size_t index = 0; index < vectors.size(); ++index) {
        Cursor input(vectors[index]);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        const auto& fetch = require_message<FetchMessage>(decoded);
        EXPECT_EQ(fetch.request_id, 8u + index * 2u);
        if (index == 0) {
            const auto& standalone = std::get<StandaloneFetch>(fetch.fetch);
            EXPECT_EQ(standalone.start, (Location{1, 2}));
            EXPECT_EQ(standalone.end, (Location{3, 0}));
        } else if (index == 1) {
            const auto& joining = std::get<RelativeJoiningFetch>(fetch.fetch);
            EXPECT_EQ(joining.joining_request_id, 2u);
            EXPECT_EQ(joining.joining_start, 5u);
        } else {
            const auto& joining = std::get<AbsoluteJoiningFetch>(fetch.fetch);
            EXPECT_EQ(joining.joining_request_id, 2u);
            EXPECT_EQ(joining.joining_start, 5u);
        }
        ByteWriter output(vectors[index].size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), vectors[index]);
    }
}

TEST(Draft18RequestMessagesTest,
     EveryFetchFormAcceptsNonMinimalFieldsAndEncodesCanonically) {
    struct Vector {
        std::vector<std::byte> encoded;
        std::vector<std::byte> canonical;
    };
    const std::vector<Vector> vectors{
        {bytes({0x16, 0x00, 0x16, 0x80, 0x08, 0x80, 0x01, 0x80,
                0x01, 0x80, 0x01, 'n', 0x80, 0x01, 't', 0x80, 0x01,
                0x80, 0x02, 0x80, 0x03, 0x80, 0x00, 0x80, 0x00}),
         bytes({0x16, 0x00, 0x0c, 0x08, 0x01, 0x01, 0x01, 'n',
                0x01, 't', 0x01, 0x02, 0x03, 0x00, 0x00})},
        {bytes({0x16, 0x00, 0x0a, 0x80, 0x0a, 0x80, 0x02, 0x80,
                0x02, 0x80, 0x05, 0x80, 0x00}),
         bytes({0x16, 0x00, 0x05, 0x0a, 0x02, 0x02, 0x05, 0x00})},
        {bytes({0x16, 0x00, 0x0a, 0x80, 0x0c, 0x80, 0x03, 0x80,
                0x02, 0x80, 0x05, 0x80, 0x00}),
         bytes({0x16, 0x00, 0x05, 0x0c, 0x03, 0x02, 0x05, 0x00})},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(decoded));
        ByteWriter output(vector.canonical.size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), vector.canonical);
    }
}

TEST(Draft18RequestMessagesTest, AcceptsNonMinimalFieldsAndEncodesCanonically) {
    const auto encoded = bytes({0x03, 0x00, 0x08, 0x80, 0x02, 0x01, 0x01,
                                'n', 0x01, 't', 0x00});
    Cursor input(encoded);
    const auto decoded = decode_message(StreamRole::Request, input, {});
    EXPECT_EQ(require_message<SubscribeMessage>(decoded).request_id, 2u);
    ByteWriter output(16);
    ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
    expect_bytes(output.bytes(),
                 bytes({0x03, 0x00, 0x07, 0x02, 0x01, 0x01, 'n',
                        0x01, 't', 0x00}));
}

TEST(Draft18RequestMessagesTest, UsesExactParameterContextAndPropagatesAmbiguity) {
    const std::vector<std::vector<std::byte>> illegal{
        bytes({0x03, 0x00, 0x09, 0x02, 0x01, 0x01, 'n', 0x01, 't',
               0x01, 0x08, 0x01}),
        bytes({0x1d, 0x00, 0x0a, 0x03, 0x01, 0x01, 'n', 0x01, 't',
               0x05, 0x01, 0x20, 0x01}),
        bytes({0x16, 0x00, 0x0e, 0x04, 0x01, 0x01, 0x01, 'n', 0x01, 't',
               0x01, 0x02, 0x03, 0x00, 0x01, 0x08, 0x01}),
        bytes({0x0d, 0x00, 0x09, 0x04, 0x01, 0x01, 'n', 0x01, 't',
               0x01, 0x20, 0x01}),
        bytes({0x06, 0x00, 0x07, 0x05, 0x01, 0x01, 'n', 0x01, 0x10, 0x01}),
        bytes({0x50, 0x00, 0x05, 0x06, 0x00, 0x01, 0x10, 0x01}),
        bytes({0x51, 0x00, 0x05, 0x07, 0x00, 0x01, 0x20, 0x01}),
    };
    for (const auto& frame : illegal) {
        Cursor input(frame, 200);
        expect_error_code(decode_message(StreamRole::Request, input, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 200u);
    }

    const auto ambiguous = bytes({0x03, 0x00, 0x08, 0x02, 0x01, 0x01, 'n',
                                  0x01, 't', 0x01, 0x04});
    Cursor ambiguity_input(ambiguous, 210);
    const auto ambiguity =
        decode_message(StreamRole::Request, ambiguity_input, {});
    EXPECT_TRUE(std::holds_alternative<DraftAmbiguity>(ambiguity));
    EXPECT_FALSE(std::holds_alternative<DecodeError>(ambiguity));
    EXPECT_EQ(ambiguity_input.offset(), 210u);
}

TEST(Draft18RequestMessagesTest, RejectsInvalidFetchAndTrailingPayload) {
    const std::vector<std::vector<std::byte>> invalid{
        bytes({0x16, 0x00, 0x03, 0x02, 0x04, 0x00}),
        bytes({0x16, 0x00, 0x0c, 0x02, 0x01, 0x01, 0x01, 'n',
               0x01, 't', 0x03, 0x00, 0x02, 0x00, 0x00}),
        bytes({0x03, 0x00, 0x08, 0x02, 0x01, 0x01, 'n', 0x01, 't',
               0x00, 0xff}),
    };
    for (const auto& frame : invalid) {
        Cursor input(frame, 220);
        expect_error_code(decode_message(StreamRole::Request, input, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 220u);
    }
}

TEST(Draft18RequestMessagesTest, StandaloneFetchUsesLexicographicLocationOrder) {
    struct Vector {
        Location start;
        Location end;
        bool accepted;
    };
    const std::vector<Vector> vectors{
        {{3, 4}, {3, 4}, true},
        {{3, 4}, {3, 3}, false},
        {{3, 4}, {2, 99}, false},
        {{3, 4}, {4, 0}, true},
        {{3, 1}, {3, 0}, false},
        {{3, 0}, {3, 0}, true},
    };
    for (const auto& vector : vectors) {
        const auto frame = bytes({0x16, 0x00, 0x0c, 0x02, 0x01, 0x01, 0x01,
                                  'n', 0x01, 't',
                                  static_cast<unsigned>(vector.start.group),
                                  static_cast<unsigned>(vector.start.object),
                                  static_cast<unsigned>(vector.end.group),
                                  static_cast<unsigned>(vector.end.object), 0x00});
        Cursor input(frame, 225);
        const auto result = decode_message(StreamRole::Request, input, {});
        EXPECT_EQ(std::holds_alternative<Message>(result), vector.accepted);
        if (!vector.accepted) {
            expect_error_code(result, DecodeErrorCode::ProtocolViolation);
            EXPECT_EQ(input.offset(), 225u);
        }
    }
}

TEST(Draft18RequestMessagesTest, PublishPropertyDeltaResetsAfterParameters) {
    const auto encoded = bytes({0x1d, 0x00, 0x0c, 0x03, 0x01, 0x01, 'n',
                                0x01, 't', 0x05, 0x01, 0x08, 0x07,
                                0x02, 0x09});
    Cursor input(encoded);
    const auto decoded = decode_message(StreamRole::Request, input, {});
    const auto& publish = require_message<PublishMessage>(decoded);
    ASSERT_EQ(publish.parameters.size(), 1u);
    EXPECT_EQ(publish.parameters[0].type, 8u);
    ASSERT_EQ(publish.track_properties.entries.size(), 1u);
    EXPECT_EQ(publish.track_properties.entries[0].type, 2u);
    ByteWriter output(encoded.size());
    ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
    expect_bytes(output.bytes(), encoded);
}

TEST(Draft18RequestMessagesTest, BoundedInnerFailuresHaveExactPeerErrors) {
    const auto truncated_namespace =
        bytes({0x03, 0x00, 0x04, 0x02, 0x01, 0x02, 'n'});
    Cursor namespace_input(truncated_namespace, 227);
    expect_error_code(decode_message(StreamRole::Request, namespace_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(namespace_input.offset(), 227u);

    const auto malformed_property =
        bytes({0x1d, 0x00, 0x0b, 0x03, 0x01, 0x01, 'n', 0x01, 't',
               0x05, 0x00, 0x01, 0x02, 0xaa});
    Cursor property_input(malformed_property, 228);
    expect_error_code(decode_message(StreamRole::Request, property_input, {}),
                      DecodeErrorCode::KeyValueFormattingError);
    EXPECT_EQ(property_input.offset(), 228u);
}

TEST(Draft18RequestMessagesTest, TrackPropertyKvpErrorsKeepDraftExactCodes) {
    const auto delta_overflow = bytes({
        0x1d, 0x00, 0x13, 0x03, 0x01, 0x01, 'n', 0x01, 't', 0x05, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01,
    });
    Cursor overflow_input(delta_overflow, 229);
    expect_error_code(decode_message(StreamRole::Request, overflow_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(overflow_input.offset(), 229u);

    const auto draft_oversize =
        bytes({0x1d, 0x00, 0x0c, 0x03, 0x01, 0x01, 'n', 0x01, 't',
               0x05, 0x00, 0x01, 0xc1, 0x00, 0x00});
    Cursor draft_limit_input(draft_oversize, 230);
    expect_error_code(decode_message(StreamRole::Request, draft_limit_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(draft_limit_input.offset(), 230u);

    const auto configured_oversize =
        bytes({0x1d, 0x00, 0x0d, 0x03, 0x01, 0x01, 'n', 0x01, 't',
               0x05, 0x00, 0x01, 0x03, 0xaa, 0xbb, 0xcc});
    Limits limits;
    limits.maximum_odd_value_length = 2;
    Cursor configured_limit_input(configured_oversize, 231);
    expect_error_code(
        decode_message(StreamRole::Request, configured_limit_input, limits),
        DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(configured_limit_input.offset(), 231u);
}

TEST(Draft18RequestMessagesTest, RejectsEveryWrongPhysicalRoleBeforePayload) {
    const std::vector<std::vector<std::byte>> headers{
        bytes({0x03, 0xff, 0xff}), bytes({0x1d, 0xff, 0xff}),
        bytes({0x16, 0xff, 0xff}), bytes({0x0d, 0xff, 0xff}),
        bytes({0x06, 0xff, 0xff}), bytes({0x50, 0xff, 0xff}),
        bytes({0x51, 0xff, 0xff}),
    };
    for (const auto& header : headers) {
        Cursor input(header, 230);
        expect_error_code(decode_message(StreamRole::Control, input, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 230u);
    }
}

TEST(Draft18ContinuationMessagesTest, DecodesAndEncodesEveryRemainingMessage) {
    const std::vector<std::vector<std::byte>> frames{
        bytes({0x10, 0x00, 0x05, 0x02, 'u', 'r', 0x05, 0x02}),
        bytes({0x10, 0x00, 0x04, 0x02, 'u', 'r', 0x05}),
        bytes({0x04, 0x00, 0x04, 0x05, 0x00, 0x02, 0x09}),
        bytes({0x07, 0x00, 0x04, 0x00, 0x01, 0x01, 0xaa}),
        bytes({0x05, 0x00, 0x04, 0x01, 0x00, 0x01, 'x'}),
        bytes({0x05, 0x00, 0x0a, 0x34, 0x01, 0x00, 0x01, 'u',
               0x01, 0x01, 'n', 0x01, 't'}),
        bytes({0x02, 0x00, 0x02, 0x04, 0x00}),
        bytes({0x0b, 0x00, 0x05, 0x25, 0x07, 0x02, 0x00, 0xff}),
        bytes({0x18, 0x00, 0x06, 0x01, 0x02, 0x03, 0x00, 0x02, 0x09}),
        bytes({0x08, 0x00, 0x03, 0x01, 0x01, 'n'}),
        bytes({0x0e, 0x00, 0x01, 0x00}),
        bytes({0x0f, 0x00, 0x05, 0x01, 0x01, 'n', 0x01, 't'}),
    };
    const std::vector<StreamRole> roles{
        StreamRole::Control, StreamRole::Request, StreamRole::Request,
        StreamRole::Request, StreamRole::Request, StreamRole::Request,
        StreamRole::Request, StreamRole::Request, StreamRole::Request,
        StreamRole::Request, StreamRole::Request, StreamRole::Request,
    };

    for (std::size_t index = 0; index < frames.size(); ++index) {
        Cursor input(frames[index]);
        const auto decoded = decode_message(roles[index], input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(decoded)) << index;
        EXPECT_EQ(input.remaining(), 0u) << index;
        ByteWriter output(frames[index].size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value())
            << index;
        expect_bytes(output.bytes(), frames[index]);
    }

    Cursor control_goaway_input(frames[0]);
    const auto control_goaway_result =
        decode_message(StreamRole::Control, control_goaway_input, {});
    const auto& control_goaway =
        require_message<GoawayMessage>(control_goaway_result);
    ASSERT_TRUE(control_goaway.request_id.has_value());
    EXPECT_EQ(*control_goaway.request_id, 2u);

    Cursor request_goaway_input(frames[1]);
    const auto request_goaway_result =
        decode_message(StreamRole::Request, request_goaway_input, {});
    const auto& request_goaway = require_message<GoawayMessage>(
        request_goaway_result);
    EXPECT_FALSE(request_goaway.request_id.has_value());

    Cursor subscribe_ok_input(frames[2]);
    const auto subscribe_ok_result =
        decode_message(StreamRole::Request, subscribe_ok_input, {});
    const auto& subscribe_ok = require_message<SubscribeOkMessage>(
        subscribe_ok_result);
    EXPECT_EQ(subscribe_ok.track_alias, 5u);
    ASSERT_EQ(subscribe_ok.track_properties.entries.size(), 1u);
    EXPECT_EQ(subscribe_ok.track_properties.entries[0].type, 2u);

    Cursor request_error_input(frames[5]);
    const auto request_error_result =
        decode_message(StreamRole::Request, request_error_input, {});
    const auto& request_error = require_message<RequestErrorMessage>(
        request_error_result);
    ASSERT_TRUE(request_error.redirect.has_value());
    expect_bytes(request_error.redirect->connect_uri, bytes({'u'}));
    expect_bytes(request_error.redirect->track_name.bytes, bytes({'t'}));

    Cursor publish_done_input(frames[7]);
    const auto publish_done_result =
        decode_message(StreamRole::Request, publish_done_input, {});
    const auto& publish_done = require_message<PublishDoneMessage>(
        publish_done_result);
    EXPECT_EQ(publish_done.status_code, 37u);
    expect_bytes(publish_done.reason_phrase.bytes, bytes({0x00, 0xff}));
}

TEST(Draft18ContinuationMessagesTest, PreservesUnknownCodesAndPropertyBoundaries) {
    const std::vector<std::vector<std::byte>> frames{
        bytes({0x05, 0x00, 0x03, 0x25, 0x00, 0x00}),
        bytes({0x0b, 0x00, 0x03, 0x25, 0x07, 0x00}),
        bytes({0x04, 0x00, 0x02, 0x05, 0x00}),
        bytes({0x07, 0x00, 0x01, 0x00}),
        bytes({0x18, 0x00, 0x04, 0x00, 0x02, 0x03, 0x00}),
    };
    for (const auto& frame : frames) {
        Cursor input(frame);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(decoded));
        ByteWriter output(frame.size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), frame);
    }
}

TEST(Draft18ContinuationMessagesTest, ReasonPhraseBoundariesAreBinarySafe) {
    for (const std::size_t length : {0u, 1u, 1'024u}) {
        SCOPED_TRACE(length);
        std::vector<std::byte> frame{std::byte{0x0b}};
        const auto payload_length = 2u + (length < 64 ? 1u : 2u) + length;
        frame.push_back(static_cast<std::byte>((payload_length >> 8u) & 0xffu));
        frame.push_back(static_cast<std::byte>(payload_length & 0xffu));
        frame.push_back(std::byte{0x25});
        frame.push_back(std::byte{0x07});
        if (length < 64) {
            frame.push_back(static_cast<std::byte>(length));
        } else {
            frame.push_back(std::byte{0x84});
            frame.push_back(std::byte{0x00});
        }
        frame.resize(frame.size() + length, std::byte{0});
        if (length != 0) frame.back() = std::byte{0xff};

        Cursor input(frame);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        if (const auto* error = std::get_if<DecodeError>(&decoded)) {
            ADD_FAILURE() << error->detail << " at " << error->offset;
        }
        const auto& done = require_message<PublishDoneMessage>(decoded);
        ASSERT_EQ(done.reason_phrase.bytes.size(), length);
        if (length != 0) {
            EXPECT_EQ(done.reason_phrase.bytes.back(), std::byte{0xff});
        }
        ByteWriter output(frame.size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), frame);
    }

    std::vector<std::byte> oversized{std::byte{0x0b}, std::byte{0x04},
                                     std::byte{0x05}, std::byte{0x00},
                                     std::byte{0x00}, std::byte{0x84},
                                     std::byte{0x01}};
    oversized.resize(3u + 1'029u, std::byte{0xaa});
    Cursor oversized_input(oversized, 300);
    expect_error_code(decode_message(StreamRole::Request, oversized_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(oversized_input.offset(), 300u);

    const auto nonminimal = bytes({0x0b, 0x00, 0x05, 0x00, 0x00,
                                   0x80, 0x01, 0x00});
    Cursor nonminimal_input(nonminimal);
    const auto decoded = decode_message(StreamRole::Request, nonminimal_input, {});
    ByteWriter canonical(8);
    ASSERT_TRUE(encode_message(std::get<Message>(decoded), canonical).has_value());
    expect_bytes(canonical.bytes(), bytes({0x0b, 0x00, 0x04, 0x00, 0x00,
                                           0x01, 0x00}));
}

TEST(Draft18ContinuationMessagesTest, GoawayUriBoundariesAndFormsAreExact) {
    for (const auto role : {StreamRole::Control, StreamRole::Request}) {
        std::vector<std::byte> frame{std::byte{0x10}, std::byte{0x20},
                                     static_cast<std::byte>(role == StreamRole::Control
                                                                ? 0x04
                                                                : 0x03),
                                     std::byte{0xa0}, std::byte{0x00}};
        frame.resize(frame.size() + 8'192u, std::byte{'u'});
        frame.push_back(std::byte{0x00});
        if (role == StreamRole::Control) frame.push_back(std::byte{0x02});
        Cursor input(frame);
        const auto decoded = decode_message(role, input, {});
        if (const auto* error = std::get_if<DecodeError>(&decoded)) {
            ADD_FAILURE() << error->detail << " at " << error->offset;
        }
        ASSERT_TRUE(std::holds_alternative<Message>(decoded));
        EXPECT_EQ(require_message<GoawayMessage>(decoded).new_session_uri.size(),
                  8'192u);
        ByteWriter output(frame.size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), frame);
    }

    std::vector<std::byte> oversized{std::byte{0x10}, std::byte{0x20},
                                     std::byte{0x04}, std::byte{0xa0},
                                     std::byte{0x01}};
    oversized.resize(oversized.size() + 8'193u, std::byte{'u'});
    oversized.push_back(std::byte{0x00});
    Cursor oversized_input(oversized, 310);
    expect_error_code(decode_message(StreamRole::Request, oversized_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(oversized_input.offset(), 310u);

    const std::vector<std::pair<StreamRole, std::vector<std::byte>>> invalid{
        {StreamRole::Control, bytes({0x10, 0x00, 0x02, 0x00, 0x00})},
        {StreamRole::Request, bytes({0x10, 0x00, 0x03, 0x00, 0x00, 0x02})},
    };
    for (const auto& [role, frame] : invalid) {
        Cursor input(frame, 320);
        expect_error_code(decode_message(role, input, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 320u);
    }
}

TEST(Draft18ContinuationMessagesTest, RedirectPreservesBinaryUriAndNameBounds) {
    const auto binary = bytes({0x05, 0x00, 0x09, 0x34, 0x00, 0x00,
                               0x02, 0x00, 0xff, 0x00, 0x01, 0x00});
    Cursor binary_input(binary);
    const auto binary_result =
        decode_message(StreamRole::Request, binary_input, {});
    const auto& error = require_message<RequestErrorMessage>(binary_result);
    ASSERT_TRUE(error.redirect.has_value());
    expect_bytes(error.redirect->connect_uri, bytes({0x00, 0xff}));
    EXPECT_TRUE(error.redirect->track_namespace.fields.empty());
    expect_bytes(error.redirect->track_name.bytes, bytes({0x00}));

    std::vector<std::byte> boundary{std::byte{0x05}, std::byte{0x10},
                                    std::byte{0x07}, std::byte{0x34},
                                    std::byte{0x00}, std::byte{0x00},
                                    std::byte{0x00}, std::byte{0x00},
                                    std::byte{0x90}, std::byte{0x00}};
    boundary.resize(3u + 4'103u, std::byte{'t'});
    Cursor boundary_input(boundary);
    const auto boundary_result =
        decode_message(StreamRole::Request, boundary_input, {});
    ASSERT_TRUE(std::holds_alternative<Message>(boundary_result));
    EXPECT_EQ(require_message<RequestErrorMessage>(boundary_result)
                  .redirect->track_name.bytes.size(),
              4'096u);

    std::vector<std::byte> oversized{std::byte{0x05}, std::byte{0x10},
                                     std::byte{0x08}, std::byte{0x34},
                                     std::byte{0x00}, std::byte{0x00},
                                     std::byte{0x00}, std::byte{0x00},
                                     std::byte{0x90}, std::byte{0x01}};
    oversized.resize(3u + 4'104u, std::byte{'t'});
    Cursor oversized_input(oversized, 325);
    expect_error_code(decode_message(StreamRole::Request, oversized_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(oversized_input.offset(), 325u);
}

TEST(Draft18ContinuationMessagesTest, PropertyDeltaRestartsAfterParameters) {
    const std::vector<std::vector<std::byte>> frames{
        bytes({0x04, 0x00, 0x06, 0x05, 0x01, 0x08, 0x01, 0x02, 0x09}),
        bytes({0x07, 0x00, 0x05, 0x01, 0x08, 0x01, 0x02, 0x09}),
    };
    for (const auto& frame : frames) {
        Cursor input(frame);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(decoded));
        const auto& message = std::get<Message>(decoded);
        const auto& properties = std::holds_alternative<SubscribeOkMessage>(message)
            ? std::get<SubscribeOkMessage>(message).track_properties
            : std::get<RequestOkMessage>(message).track_properties;
        ASSERT_EQ(properties.entries.size(), 1u);
        EXPECT_EQ(properties.entries[0].type, 2u);
    }
}

TEST(Draft18ContinuationMessagesTest, RejectsRedirectShapeAndBoundedTruncation) {
    const std::vector<std::vector<std::byte>> invalid{
        bytes({0x05, 0x00, 0x04, 0x34, 0x00, 0x00, 0x00}),
        bytes({0x05, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00}),
        bytes({0x05, 0x00, 0x07, 0x34, 0x00, 0x00, 0x00, 0x01, 0x01, 'n'}),
    };
    for (const auto& frame : invalid) {
        Cursor input(frame, 330);
        expect_error_code(decode_message(StreamRole::Request, input, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 330u);
    }
}

TEST(Draft18ContinuationMessagesTest,
     RedirectUriBeyondFrameCapacityIsProtocolViolation) {
    const auto frame =
        bytes({0x05, 0x00, 0x06, 0x34, 0x00, 0x00, 0xc1, 0x00, 0x00});
    Cursor input(frame, 335);
    expect_error_code(decode_message(StreamRole::Request, input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 335u);
}

TEST(Draft18ContinuationMessagesTest, PropagatesUnresolvedParameterAmbiguity) {
    const std::vector<std::vector<std::byte>> ambiguous{
        bytes({0x07, 0x00, 0x02, 0x01, 0x04}),
        bytes({0x02, 0x00, 0x03, 0x04, 0x01, 0x04}),
    };
    for (const auto& frame : ambiguous) {
        Cursor input(frame, 340);
        const auto result = decode_message(StreamRole::Request, input, {});
        EXPECT_TRUE(std::holds_alternative<DraftAmbiguity>(result));
        EXPECT_FALSE(std::holds_alternative<DecodeError>(result));
        EXPECT_EQ(input.offset(), 340u);
    }
}

TEST(Draft18ContinuationMessagesTest, FetchOkPreservesUndefinedUint8Values) {
    for (const unsigned end_of_track : {2u, 255u}) {
        const auto frame = bytes({0x18, 0x00, 0x04, end_of_track,
                                  0x00, 0x00, 0x00});
        Cursor input(frame);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        EXPECT_EQ(require_message<FetchOkMessage>(decoded).end_of_track,
                  end_of_track);
        ByteWriter output(frame.size());
        ASSERT_TRUE(encode_message(std::get<Message>(decoded), output).has_value());
        expect_bytes(output.bytes(), frame);
    }
}

TEST(Draft18ContinuationMessagesTest, RejectsLocallyImpossibleSuffixAndName) {
    std::vector<std::byte> blocked{std::byte{0x0f}, std::byte{0x10},
                                   std::byte{0x06}, std::byte{0x00},
                                   std::byte{0xc0}, std::byte{0x10},
                                   std::byte{0x01}};
    blocked.resize(3u + 4'102u, std::byte{'t'});
    Cursor blocked_input(blocked, 355);
    expect_error_code(decode_message(StreamRole::Request, blocked_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(blocked_input.offset(), 355u);
}

TEST(Draft18ContinuationMessagesTest, EveryNewFrameTruncationIsAtomic) {
    const std::vector<std::vector<std::byte>> frames{
        bytes({0x10, 0x00, 0x03, 0x00, 0x00, 0x02}),
        bytes({0x04, 0x00, 0x02, 0x05, 0x00}),
        bytes({0x07, 0x00, 0x01, 0x00}),
        bytes({0x05, 0x00, 0x03, 0x01, 0x00, 0x00}),
        bytes({0x02, 0x00, 0x02, 0x04, 0x00}),
        bytes({0x0b, 0x00, 0x03, 0x00, 0x00, 0x00}),
        bytes({0x18, 0x00, 0x04, 0x00, 0x02, 0x03, 0x00}),
        bytes({0x08, 0x00, 0x01, 0x00}),
        bytes({0x0e, 0x00, 0x01, 0x00}),
        bytes({0x0f, 0x00, 0x02, 0x00, 0x00}),
    };
    for (const auto& frame : frames) {
        for (std::size_t available = 0; available < frame.size(); ++available) {
            Cursor input(std::span<const std::byte>(frame).first(available), 360);
            EXPECT_TRUE(std::holds_alternative<NeedMore>(
                decode_message(StreamRole::Request, input, {})));
            EXPECT_EQ(input.offset(), 360u);
        }
    }
}

TEST(Draft18ContinuationMessagesTest, InvalidEncodingAndCapacityAreAtomic) {
    const std::vector<Message> invalid{
        Message{GoawayMessage{std::vector<std::byte>(8'193u), 0, std::nullopt}},
        Message{RequestErrorMessage{1, 0, {}, Redirect{}}},
        Message{RequestErrorMessage{0x34, 0, {}, std::nullopt}},
        Message{PublishDoneMessage{0, 0, {std::vector<std::byte>(1'025u)}}},
    };
    for (const auto& message : invalid) {
        ByteWriter output(16);
        ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
        const auto before = std::vector<std::byte>(output.bytes().begin(),
                                                   output.bytes().end());
        EXPECT_FALSE(encode_message(message, output).has_value());
        expect_bytes(output.bytes(), before);
    }

    const Message valid{NamespaceMessage{TrackNamespace{}}};
    ByteWriter output(3);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    const auto before = std::vector<std::byte>(output.bytes().begin(),
                                               output.bytes().end());
    EXPECT_FALSE(encode_message(valid, output).has_value());
    expect_bytes(output.bytes(), before);
}

TEST(Draft18ContinuationMessagesTest, PreservesEveryRegisteredStatusCode) {
    const std::vector<std::uint64_t> request_error_codes{
        0, 1, 2, 3, 4, 5, 6, 9, 0x10, 0x11, 0x12, 0x19,
        0x20, 0x30, 0x31, 0x32, 0x33,
    };
    for (const auto code : request_error_codes) {
        const auto frame = bytes({0x05, 0x00, 0x03,
                                  static_cast<unsigned>(code), 0x00, 0x00});
        Cursor input(frame);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        const auto& error = require_message<RequestErrorMessage>(decoded);
        EXPECT_EQ(error.error_code, code);
        EXPECT_FALSE(error.redirect.has_value());
    }
    const auto redirect = bytes({0x05, 0x00, 0x06, 0x34, 0x00, 0x00,
                                 0x00, 0x00, 0x00});
    Cursor redirect_input(redirect);
    const auto redirect_result =
        decode_message(StreamRole::Request, redirect_input, {});
    EXPECT_TRUE(require_message<RequestErrorMessage>(redirect_result)
                    .redirect.has_value());

    const std::vector<std::uint64_t> publish_done_codes{
        0, 1, 2, 3, 4, 5, 6, 8, 9, 0x12,
    };
    for (const auto code : publish_done_codes) {
        const auto frame = bytes({0x0b, 0x00, 0x03,
                                  static_cast<unsigned>(code), 0x00, 0x00});
        Cursor input(frame);
        const auto decoded = decode_message(StreamRole::Request, input, {});
        EXPECT_EQ(require_message<PublishDoneMessage>(decoded).status_code, code);
    }
}

TEST(Draft18RequestMessagesTest, DispatchesEveryRuledTableFiveTypeOnExactRole) {
    const std::vector<std::vector<std::byte>> request_frames{
        bytes({0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x04, 0x00, 0x02, 0x00, 0x00}),
        bytes({0x1d, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x0b, 0x00, 0x03, 0x00, 0x00, 0x00}),
        bytes({0x16, 0x00, 0x09, 0x00, 0x01, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00}),
        bytes({0x18, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x0d, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x06, 0x00, 0x03, 0x00, 0x00, 0x00}),
        bytes({0x50, 0x00, 0x03, 0x00, 0x00, 0x00}),
        bytes({0x51, 0x00, 0x03, 0x00, 0x00, 0x00}),
        bytes({0x08, 0x00, 0x01, 0x00}),
        bytes({0x0e, 0x00, 0x01, 0x00}),
        bytes({0x0f, 0x00, 0x02, 0x00, 0x00}),
        bytes({0x02, 0x00, 0x02, 0x00, 0x00}),
        bytes({0x07, 0x00, 0x01, 0x00}),
        bytes({0x05, 0x00, 0x03, 0x00, 0x00, 0x00}),
    };
    for (const auto& frame : request_frames) {
        Cursor input(frame);
        const auto result = decode_message(StreamRole::Request, input, {});
        EXPECT_TRUE(std::holds_alternative<Message>(result));

        const auto type_header = bytes({std::to_integer<unsigned>(frame[0]),
                                        0xff, 0xff});
        Cursor wrong_role(type_header, 245);
        expect_error_code(decode_message(StreamRole::Control, wrong_role, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(wrong_role.offset(), 245u);
    }

    const auto setup = bytes({0xaf, 0x00, 0x00, 0x00});
    Cursor setup_input(setup);
    EXPECT_TRUE(std::holds_alternative<Message>(
        decode_message(StreamRole::Control, setup_input, {})));
    const auto setup_header = bytes({0xaf, 0x00, 0xff, 0xff});
    Cursor setup_wrong_role(setup_header, 246);
    expect_error_code(decode_message(StreamRole::Request, setup_wrong_role, {}),
                      DecodeErrorCode::ProtocolViolation);

    const std::vector<std::pair<StreamRole, std::vector<std::byte>>> goaways{
        {StreamRole::Control, bytes({0x10, 0x00, 0x03, 0x00, 0x00, 0x00})},
        {StreamRole::Request, bytes({0x10, 0x00, 0x02, 0x00, 0x00})},
    };
    for (const auto& [role, frame] : goaways) {
        Cursor input(frame);
        const auto result = decode_message(role, input, {});
        EXPECT_TRUE(std::holds_alternative<Message>(result));
    }

    const std::vector<std::vector<std::byte>> peer_errors{
        bytes({0x01, 0xff, 0xff}), bytes({0x1e, 0xff, 0xff}),
        bytes({0x7f, 0xff, 0xff}),
    };
    for (const auto& header : peer_errors) {
        for (const auto role : {StreamRole::Control, StreamRole::Request}) {
            Cursor input(header, 250);
            expect_error_code(decode_message(role, input, {}),
                              DecodeErrorCode::ProtocolViolation);
            EXPECT_EQ(input.offset(), 250u);
        }
    }
}

TEST(Draft18RequestMessagesTest, EveryOpeningFrameTruncationIsAtomic) {
    const std::vector<std::vector<std::byte>> frames{
        bytes({0x03, 0x00, 0x07, 0x02, 0x01, 0x01, 'n', 0x01, 't', 0x00}),
        bytes({0x1d, 0x00, 0x08, 0x03, 0x01, 0x01, 'n', 0x01, 't', 0x05, 0x00}),
        bytes({0x16, 0x00, 0x05, 0x04, 0x02, 0x02, 0x05, 0x00}),
        bytes({0x0d, 0x00, 0x07, 0x04, 0x01, 0x01, 'n', 0x01, 't', 0x00}),
        bytes({0x06, 0x00, 0x05, 0x05, 0x01, 0x01, 'n', 0x00}),
        bytes({0x50, 0x00, 0x03, 0x06, 0x00, 0x00}),
        bytes({0x51, 0x00, 0x03, 0x07, 0x00, 0x00}),
    };
    for (const auto& frame : frames) {
        for (std::size_t available = 0; available < frame.size(); ++available) {
            Cursor input(std::span<const std::byte>(frame).first(available), 260);
            EXPECT_TRUE(std::holds_alternative<NeedMore>(
                decode_message(StreamRole::Request, input, {})))
                << frame.size() << ' ' << available;
            EXPECT_EQ(input.offset(), 260u);
        }
    }
}

TEST(Draft18RequestMessagesTest, FramingAndEncodingFailuresAreAtomic) {
    const auto frame =
        bytes({0x03, 0x00, 0x07, 0x02, 0x01, 0x01, 'n', 0x01, 't', 0x00});
    auto concatenated = frame;
    concatenated.insert(concatenated.end(), frame.begin(), frame.end());
    Cursor input(concatenated);
    const auto first = decode_message(StreamRole::Request, input, {});
    ASSERT_TRUE(std::holds_alternative<Message>(first));
    EXPECT_EQ(input.remaining(), frame.size());

    ByteWriter output(frame.size());
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    const auto before = std::vector<std::byte>(output.bytes().begin(),
                                               output.bytes().end());
    EXPECT_FALSE(encode_message(std::get<Message>(first), output).has_value());
    expect_bytes(output.bytes(), before);

    auto too_long = frame;
    too_long[2] = std::byte{0x08};
    Cursor too_long_input(too_long, 270);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(
        decode_message(StreamRole::Request, too_long_input, {})));
    EXPECT_EQ(too_long_input.offset(), 270u);

    auto too_short = frame;
    too_short[2] = std::byte{0x06};
    Cursor too_short_input(too_short, 280);
    expect_error_code(decode_message(StreamRole::Request, too_short_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(too_short_input.offset(), 280u);

    const SubscribeMessage invalid{
        2, TrackNamespace{{bytes({'n'})}}, TrackName{bytes({'t'})},
        Parameters{Parameter{0x08, VarIntParameterValue{1}}}};
    const FetchMessage reversed{
        4,
        StandaloneFetch{TrackNamespace{{bytes({'n'})}}, TrackName{bytes({'t'})},
                        Location{2, 0}, Location{1, 99}},
        {}};
    for (const auto& message : {Message{invalid}, Message{reversed}}) {
        ByteWriter invalid_output(64);
        ASSERT_TRUE(invalid_output.append_byte(std::byte{0xcc}));
        const auto invalid_before = std::vector<std::byte>(
            invalid_output.bytes().begin(), invalid_output.bytes().end());
        const auto result = encode_message(message, invalid_output);
        EXPECT_FALSE(result.has_value());
        expect_bytes(invalid_output.bytes(), invalid_before);
    }
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

TEST(Draft18StructuresTest,
     NamespaceAndTrackNameAcceptNonMinimalLengthsAndEncodeCanonically) {
    const auto encoded_namespace =
        bytes({0x80, 0x02, 0x80, 0x01, 'a', 0x80, 0x01, 'b'});
    Cursor namespace_input(encoded_namespace);
    const auto decoded_namespace = decode_track_namespace(namespace_input, {});
    ASSERT_TRUE(std::holds_alternative<TrackNamespace>(decoded_namespace));
    ByteWriter namespace_output(5);
    ASSERT_TRUE(encode_track_namespace(std::get<TrackNamespace>(decoded_namespace),
                                       namespace_output)
                    .has_value());
    expect_bytes(namespace_output.bytes(), bytes({0x02, 0x01, 'a', 0x01, 'b'}));

    const auto encoded_name = bytes({0x80, 0x01, 't'});
    Cursor name_input(encoded_name);
    const auto decoded_name = decode_track_name(
        name_input, std::get<TrackNamespace>(decoded_namespace), {});
    ASSERT_TRUE(std::holds_alternative<TrackName>(decoded_name));
    ByteWriter name_output(2);
    ASSERT_TRUE(encode_track_name(std::get<TrackName>(decoded_name),
                                  std::get<TrackNamespace>(decoded_namespace),
                                  name_output)
                    .has_value());
    expect_bytes(name_output.bytes(), bytes({0x01, 't'}));
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

TEST(Draft18StructuresTest, InvalidTypedNamespaceAndTrackNameEncodersAreAtomic) {
    const TrackNamespace too_many{
        std::vector<std::vector<std::byte>>(33, bytes({'n'}))};
    const TrackNamespace empty_field{{bytes({'n'}), {}}};
    const TrackNamespace oversized{
        {std::vector<std::byte>(4'097u, std::byte{'n'})}};
    for (const auto& name_space : {too_many, empty_field}) {
        expect_atomic_encode_error(
            [&](ByteWriter& output) {
                return encode_track_namespace(name_space, output);
            },
            EncodeErrorCode::InvalidValue);
    }
    expect_atomic_encode_error(
        [&](ByteWriter& output) {
            return encode_track_namespace(oversized, output);
        },
        EncodeErrorCode::PayloadTooLarge);

    const TrackNamespace maximum{
        {std::vector<std::byte>(4'096u, std::byte{'n'})}};
    expect_atomic_encode_error(
        [&](ByteWriter& output) {
            return encode_track_name(TrackName{bytes({'t'})}, maximum, output);
        },
        EncodeErrorCode::PayloadTooLarge);
    expect_atomic_encode_error(
        [&](ByteWriter& output) {
            return encode_track_name(TrackName{}, oversized, output);
        },
        EncodeErrorCode::PayloadTooLarge);
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

TEST(Draft18StructuresTest,
     EverySubscriptionFilterVi64AcceptsNonMinimalAndEncodesCanonically) {
    struct Vector {
        std::vector<std::byte> encoded;
        std::vector<std::byte> canonical;
    };
    const std::vector<Vector> vectors{
        {bytes({0x80, 0x01}), bytes({0x01})},
        {bytes({0x80, 0x02}), bytes({0x02})},
        {bytes({0x80, 0x03, 0x80, 0x05, 0x80, 0x06}),
         bytes({0x03, 0x05, 0x06})},
        {bytes({0x80, 0x04, 0x80, 0x05, 0x80, 0x06, 0x80, 0x07}),
         bytes({0x04, 0x05, 0x06, 0x07})},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto decoded =
            decode_subscription_filter(input, vector.encoded.size());
        ASSERT_TRUE(std::holds_alternative<SubscriptionFilter>(decoded));
        ByteWriter output(vector.canonical.size());
        ASSERT_TRUE(encode_subscription_filter(
                        std::get<SubscriptionFilter>(decoded), output)
                        .has_value());
        expect_bytes(output.bytes(), vector.canonical);
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

TEST(Draft18StructuresTest, InvalidTypedSubscriptionFilterEncodersAreAtomic) {
    const std::vector<SubscriptionFilter> invalid{
        {static_cast<SubscriptionFilterType>(0), std::nullopt, std::nullopt},
        {SubscriptionFilterType::NextGroupStart, Location{0, 0}, std::nullopt},
        {SubscriptionFilterType::AbsoluteStart, std::nullopt, std::nullopt},
        {SubscriptionFilterType::AbsoluteStart, Location{0, 0}, 1},
        {SubscriptionFilterType::AbsoluteRange, Location{0, 0}, std::nullopt},
        {SubscriptionFilterType::AbsoluteRange,
         Location{std::numeric_limits<std::uint64_t>::max(), 0}, 1},
    };
    for (const auto& filter : invalid) {
        expect_atomic_encode_error(
            [&](ByteWriter& output) {
                return encode_subscription_filter(filter, output);
            },
            EncodeErrorCode::InvalidValue);
    }
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

TEST(Draft18StructuresTest,
     EveryTokenVi64AcceptsNonMinimalAndEncodesCanonically) {
    struct Vector {
        std::vector<std::byte> encoded;
        std::vector<std::byte> canonical;
    };
    const std::vector<Vector> vectors{
        {bytes({0x80, 0x00, 0x80, 0x25}), bytes({0x00, 0x25})},
        {bytes({0x80, 0x01, 0x80, 0x25, 0x80, 0x07, 0xaa}),
         bytes({0x01, 0x25, 0x07, 0xaa})},
        {bytes({0x80, 0x02, 0x80, 0x25}), bytes({0x02, 0x25})},
        {bytes({0x80, 0x03, 0x80, 0x07, 0xaa}),
         bytes({0x03, 0x07, 0xaa})},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto decoded = decode_token(input, vector.encoded.size());
        ASSERT_TRUE(std::holds_alternative<Token>(decoded));
        ByteWriter output(vector.canonical.size());
        ASSERT_TRUE(encode_token(std::get<Token>(decoded), output).has_value());
        expect_bytes(output.bytes(), vector.canonical);
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

TEST(Draft18StructuresTest, InvalidTypedTokenEncoderShapesAreAtomic) {
    const std::vector<Token> invalid{
        Token{static_cast<TokenAliasType>(4), std::nullopt, std::nullopt, {}},
        Token{TokenAliasType::Delete, std::nullopt, std::nullopt, {}},
        Token{TokenAliasType::Delete, 1, 2, {}},
        Token{TokenAliasType::Register, 1, std::nullopt, {}},
        Token{TokenAliasType::Register, std::nullopt, 2, {}},
        Token{TokenAliasType::UseAlias, 1, std::nullopt, bytes({0xaa})},
        Token{TokenAliasType::UseValue, 1, 2, bytes({0xaa})},
        Token{TokenAliasType::UseValue, std::nullopt, std::nullopt, {}},
    };
    for (const auto& token : invalid) {
        expect_atomic_encode_error(
            [&](ByteWriter& output) { return encode_token(token, output); },
            EncodeErrorCode::InvalidValue);
    }
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

TEST(Draft18ParametersTest,
     EveryVi64BearingParameterFamilyAcceptsNonMinimalAndCanonicalizes) {
    struct Vector {
        std::vector<std::byte> encoded;
        std::vector<std::byte> canonical;
        std::uint64_t count;
        ParameterContext context;
    };
    const std::vector<Vector> vectors{
        {bytes({0x80, 0x02, 0x80, 0x25}), bytes({0x02, 0x25}), 1,
         ParameterContext::Subscribe},
        {bytes({0x80, 0x03, 0x80, 0x05, 0x80, 0x03, 0x80, 0x07,
                0xaa}),
         bytes({0x03, 0x03, 0x03, 0x07, 0xaa}), 1,
         ParameterContext::Subscribe},
        {bytes({0x80, 0x09, 0x80, 0x05, 0x80, 0x06}),
         bytes({0x09, 0x05, 0x06}), 1, ParameterContext::SubscribeOk},
        {bytes({0x80, 0x21, 0x80, 0x08, 0x80, 0x04, 0x80, 0x01,
                0x80, 0x02, 0x80, 0x03}),
         bytes({0x21, 0x04, 0x04, 0x01, 0x02, 0x03}), 1,
         ParameterContext::Subscribe},
        {bytes({0x80, 0x34, 0x80, 0x01, 0x80, 0x01, 'n'}),
         bytes({0x34, 0x01, 0x01, 'n'}), 1,
         ParameterContext::RequestUpdateSubscribeNamespace},
        {bytes({0x80, 0x02, 0x80, 0x25, 0x80, 0x04, 0x80, 0x07}),
         bytes({0x02, 0x25, 0x04, 0x07}), 2,
         ParameterContext::Subscribe},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto decoded =
            decode_parameters(input, vector.count, vector.context, {});
        ASSERT_TRUE(std::holds_alternative<Parameters>(decoded));
        ByteWriter output(vector.canonical.size());
        ASSERT_TRUE(encode_parameters(std::get<Parameters>(decoded),
                                      vector.context, output)
                        .has_value());
        expect_bytes(output.bytes(), vector.canonical);
    }
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

TEST(Draft18ParametersTest,
     DraftAmbiguityAndPeerErrorsHaveDistinctPublicClassifications) {
    const auto ambiguous_bytes = bytes({0x04});
    Cursor ambiguous_input(ambiguous_bytes, 125);
    const auto ambiguous = decode_parameters(
        ambiguous_input, 1, ParameterContext::Subscribe, {});
    ASSERT_TRUE(std::holds_alternative<DraftAmbiguity>(ambiguous));
    EXPECT_FALSE(std::get<DraftAmbiguity>(ambiguous).detail.empty());
    EXPECT_EQ(ambiguous_input.offset(), 125u);

    const auto peer_error_bytes = bytes({0x04});
    Cursor peer_error_input(peer_error_bytes, 126);
    const auto peer_error = decode_parameters(
        peer_error_input, 1, ParameterContext::Fetch, {});
    ASSERT_TRUE(std::holds_alternative<DecodeError>(peer_error));
    EXPECT_EQ(std::get<DecodeError>(peer_error).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_FALSE(std::get<DecodeError>(peer_error).detail.empty());
    EXPECT_EQ(peer_error_input.offset(), 126u);
}

TEST(Draft18ParametersTest, InvalidTypedParameterEncodersAreAtomic) {
    const Token malformed_token{TokenAliasType::UseAlias, std::nullopt,
                                std::nullopt, {}};
    const SubscriptionFilter malformed_filter{
        SubscriptionFilterType::AbsoluteRange, Location{0, 0}, std::nullopt};
    const TrackNamespace malformed_namespace{{{}}};
    const std::vector<std::pair<Parameters, ParameterContext>> invalid{
        {{Parameter{0x05, VarIntParameterValue{1}}},
         ParameterContext::Unresolved},
        {{Parameter{0x06, VarIntParameterValue{1}},
          Parameter{0x02, VarIntParameterValue{1}}},
         ParameterContext::Subscribe},
        {{Parameter{0x02, VarIntParameterValue{1}},
          Parameter{0x02, VarIntParameterValue{2}}},
         ParameterContext::Subscribe},
        {{Parameter{0x02, VarIntParameterValue{1}}}, ParameterContext::Fetch},
        {{Parameter{0x02, Uint8ParameterValue{1}}},
         ParameterContext::Subscribe},
        {{Parameter{0x03, malformed_token}}, ParameterContext::Subscribe},
        {{Parameter{0x09, Uint8ParameterValue{1}}},
         ParameterContext::SubscribeOk},
        {{Parameter{0x10, Uint8ParameterValue{2}}},
         ParameterContext::Subscribe},
        {{Parameter{0x22, Uint8ParameterValue{0}}},
         ParameterContext::Subscribe},
        {{Parameter{0x21, malformed_filter}}, ParameterContext::Subscribe},
        {{Parameter{0x34, malformed_namespace}},
         ParameterContext::RequestUpdateSubscribeNamespace},
    };
    for (const auto& [parameters, context] : invalid) {
        expect_atomic_encode_error(
            [&](ByteWriter& output) {
                return encode_parameters(parameters, context, output);
            },
            EncodeErrorCode::InvalidValue);
    }
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

TEST(Draft18ExhaustiveAuditTest, EveryRuledMessageTypeHasExactTypedRoundTrip) {
    std::array<bool, std::variant_size_v<Message>> seen{};
    for (const auto& vector : message_audit_vectors()) {
        SCOPED_TRACE(vector.type);
        Cursor input(vector.frame, 1'000);
        const auto result = decode_message(vector.role, input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(result));
        const auto& message = std::get<Message>(result);
        EXPECT_EQ(message.index(), vector.variant_index);
        EXPECT_EQ(input.remaining(), 0u);
        seen[message.index()] = true;

        ByteWriter output(vector.frame.size());
        ASSERT_TRUE(encode_message(message, output).has_value());
        expect_bytes(output.bytes(), vector.frame);
    }
    EXPECT_TRUE(std::ranges::all_of(seen, [](bool value) { return value; }));
}

TEST(Draft18ExhaustiveAuditTest, EveryMinimalFrameTruncationIsAtomic) {
    for (const auto& vector : message_audit_vectors()) {
        for (std::size_t available = 0; available < vector.frame.size();
             ++available) {
            SCOPED_TRACE(vector.type);
            SCOPED_TRACE(available);
            Cursor input(std::span<const std::byte>(vector.frame).first(available),
                         1'100);
            const auto result = decode_message(vector.role, input, {});
            ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
            EXPECT_EQ(input.offset(), 1'100u);
        }
    }
}

TEST(Draft18ExhaustiveAuditTest, WrongRolesFailImmediatelyAfterType) {
    for (const auto& vector : message_audit_vectors()) {
        if (vector.also_valid_on_other_role) continue;
        SCOPED_TRACE(vector.type);
        const auto type_size = vector.type == 0x2f00 ? 2u : 1u;
        Cursor input(std::span<const std::byte>(vector.frame).first(type_size),
                     1'200);
        const auto wrong_role = vector.role == StreamRole::Control
                                    ? StreamRole::Request
                                    : StreamRole::Control;
        expect_error_code(decode_message(wrong_role, input, {}),
                          DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 1'200u);
    }
}

TEST(Draft18ExhaustiveAuditTest, ReservedRemovedAndUnknownTypesAreProtocolErrors) {
    const std::vector<std::vector<std::byte>> types{
        bytes({0x01}), bytes({0x20}), bytes({0x21}), bytes({0x40}),
        bytes({0x41}), bytes({0x1e}), bytes({0x7f}), bytes({0x80, 0x7f}),
    };
    for (const auto& type : types) {
        Cursor input(type, 1'300);
        const auto result = decode_message(StreamRole::Control, input, {});
        expect_error_code(result, DecodeErrorCode::ProtocolViolation);
        EXPECT_FALSE(std::holds_alternative<DraftAmbiguity>(result));
        EXPECT_EQ(input.offset(), 1'300u);
    }
}

TEST(Draft18ExhaustiveAuditTest, CompleteBoundedKvpFailuresHaveExactErrors) {
    const auto malformed = bytes({0xaf, 0x00, 0x00, 0x01, 0x01});
    Cursor malformed_input(malformed, 1'400);
    expect_error_code(decode_message(StreamRole::Control, malformed_input, {}),
                      DecodeErrorCode::KeyValueFormattingError);
    EXPECT_EQ(malformed_input.offset(), 1'400u);

    const auto overflow = bytes({
        0xaf, 0x00, 0x00, 0x0b,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x01,
    });
    Cursor overflow_input(overflow, 1'410);
    expect_error_code(decode_message(StreamRole::Control, overflow_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(overflow_input.offset(), 1'410u);

    const auto draft_oversize =
        bytes({0xaf, 0x00, 0x00, 0x04, 0x01, 0xc1, 0x00, 0x00});
    Cursor draft_limit_input(draft_oversize, 1'420);
    expect_error_code(decode_message(StreamRole::Control, draft_limit_input, {}),
                      DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(draft_limit_input.offset(), 1'420u);

    const auto configured_oversize =
        bytes({0xaf, 0x00, 0x00, 0x05, 0x01, 0x03, 0xaa, 0xbb, 0xcc});
    Limits limits;
    limits.maximum_odd_value_length = 2;
    Cursor configured_limit_input(configured_oversize, 1'430);
    expect_error_code(
        decode_message(StreamRole::Control, configured_limit_input, limits),
        DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(configured_limit_input.offset(), 1'430u);
}

TEST(Draft18ExhaustiveAuditTest, NonMinimalStructuralFamiliesCanonicalize) {
    struct Vector {
        StreamRole role;
        std::vector<std::byte> encoded;
        std::vector<std::byte> canonical;
    };
    const std::vector<Vector> vectors{
        {StreamRole::Control,
         bytes({0xc0, 0x2f, 0x00, 0x00, 0x04, 0x80, 0x04, 0x80, 0x25}),
         bytes({0xaf, 0x00, 0x00, 0x02, 0x04, 0x25})},
        {StreamRole::Request,
         bytes({0x10, 0x00, 0x04, 0x80, 0x00, 0x80, 0x00}),
         bytes({0x10, 0x00, 0x02, 0x00, 0x00})},
        {StreamRole::Request,
         bytes({0x03, 0x00, 0x08, 0x80, 0x00, 0x80, 0x00,
                0x80, 0x00, 0x80, 0x00}),
         bytes({0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00})},
        {StreamRole::Request,
         bytes({0x0b, 0x00, 0x06, 0x80, 0x00, 0x80, 0x00,
                0x80, 0x00}),
         bytes({0x0b, 0x00, 0x03, 0x00, 0x00, 0x00})},
    };
    for (const auto& vector : vectors) {
        Cursor input(vector.encoded);
        const auto result = decode_message(vector.role, input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(result));
        ByteWriter output(vector.canonical.size());
        ASSERT_TRUE(encode_message(std::get<Message>(result), output).has_value());
        expect_bytes(output.bytes(), vector.canonical);
    }
}

TEST(Draft18ExhaustiveAuditTest, ConcatenatedFramesConsumeExactlyOneAtATime) {
    for (const auto& vector : message_audit_vectors()) {
        std::vector<std::byte> concatenated = vector.frame;
        concatenated.insert(concatenated.end(), vector.frame.begin(),
                            vector.frame.end());
        Cursor input(concatenated);
        for (const auto expected_remaining : {vector.frame.size(), std::size_t{0}}) {
            const auto result = decode_message(vector.role, input, {});
            ASSERT_TRUE(std::holds_alternative<Message>(result));
            EXPECT_EQ(input.remaining(), expected_remaining);
        }
    }
}

TEST(Draft18ExhaustiveAuditTest, EveryEncoderVariantIsCapacityAtomic) {
    for (const auto& vector : message_audit_vectors()) {
        Cursor input(vector.frame);
        const auto decoded = decode_message(vector.role, input, {});
        ASSERT_TRUE(std::holds_alternative<Message>(decoded));

        ByteWriter output(vector.frame.size());
        ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
        const auto before =
            std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
        const auto result = encode_message(std::get<Message>(decoded), output);
        EXPECT_FALSE(result.has_value());
        ASSERT_NE(result.error(), nullptr);
        EXPECT_EQ(result.error()->code, EncodeErrorCode::OutputCapacity);
        expect_bytes(output.bytes(), before);
    }
}

}  // namespace
}  // namespace moq::interop::wire::draft18
