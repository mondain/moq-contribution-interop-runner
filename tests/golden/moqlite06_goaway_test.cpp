#include "moq/interop/wire/moqlite06/goaway.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

template <class T>
DecodeErrorCode error_code(const DecodeResult<T>& result) {
    const auto* error = std::get_if<DecodeError>(&result);
    EXPECT_NE(error, nullptr);
    return error ? error->code : DecodeErrorCode::InvalidValue;
}

Bytes encode(const GoawayMessage& m) {
    ByteWriter out(1u << 16);
    EXPECT_FALSE(encode_goaway(m, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

DecodeResult<GoawayMessage> decode_g(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_goaway(input, limits);
}

// "moql://b/x" = 6d 6f 71 6c 3a 2f 2f 62 2f 78 (10 bytes). Body = 0a + 10 = 11 = 0x0b.
const Bytes kUriWire = bytes({0x0b, 0x0a, 0x6d, 0x6f, 0x71, 0x6c, 0x3a, 0x2f, 0x2f, 0x62, 0x2f, 0x78});
// An empty URI means "no redirect": body 00, Message Length 1.
const Bytes kEmptyWire = bytes({0x01, 0x00});

// A GOAWAY whose URI is `size` 'a' bytes, framed by the library's own writers (the URI length 8192 is the 2-byte
// varint 60 00 and the Message Length 8194 is 60 02; the codec tests below pin the decode side).
Bytes uri_wire(std::size_t size) {
    ByteWriter body(1u << 16);
    EXPECT_TRUE(write_varint(size, body));
    for (std::size_t i = 0; i < size; ++i) EXPECT_TRUE(body.append_byte(std::byte{'a'}));
    ByteWriter framed(1u << 16);
    EXPECT_TRUE(write_framed_message(body.bytes(), framed));
    return Bytes(framed.bytes().begin(), framed.bytes().end());
}

TEST(Moqlite06Goaway, Goldens) {
    const auto a = decode_g(kUriWire);
    ASSERT_TRUE(std::holds_alternative<GoawayMessage>(a));
    EXPECT_EQ(std::get<GoawayMessage>(a).new_session_uri, "moql://b/x");
    EXPECT_EQ(encode(GoawayMessage{"moql://b/x"}), kUriWire);

    const auto b = decode_g(kEmptyWire);
    ASSERT_TRUE(std::holds_alternative<GoawayMessage>(b));
    EXPECT_EQ(std::get<GoawayMessage>(b).new_session_uri, "");
    EXPECT_EQ(encode(GoawayMessage{""}), kEmptyWire);
}

TEST(Moqlite06Goaway, MaximumUriWireIsPinned) {
    const auto wire = uri_wire(8192);
    ASSERT_GE(wire.size(), 4u);
    EXPECT_EQ(wire[0], std::byte{0x60});  // Message Length 8194 = 0x2002 -> 2-byte form 60 02
    EXPECT_EQ(wire[1], std::byte{0x02});
    EXPECT_EQ(wire[2], std::byte{0x60});  // URI length 8192 = 0x2000 -> 60 00
    EXPECT_EQ(wire[3], std::byte{0x00});
    EXPECT_EQ(wire.size(), 8194u + 2u);
}

TEST(Moqlite06Goaway, EveryPrefixIsNeedMore) {
    for (const auto* wire : {&kUriWire, &kEmptyWire}) {
        for (std::size_t cut = 0; cut < wire->size(); ++cut) {
            SCOPED_TRACE("cut " + std::to_string(cut));
            const Bytes prefix(wire->begin(), wire->begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_goaway(input)));
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
}

TEST(Moqlite06Goaway, MessageLengthLiesAreProtocolViolations) {
    EXPECT_EQ(error_code(decode_g(bytes({0x02, 0x00, 0x00}))), DecodeErrorCode::ProtocolViolation);  // trailing
    EXPECT_EQ(error_code(decode_g(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);               // no URI field
    // The URI claims 5 bytes but the body holds 2.
    EXPECT_EQ(error_code(decode_g(bytes({0x03, 0x05, 0x61, 0x62}))), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Goaway, UriLimitIs8192AndIsCheckedBeforeReading) {
    EXPECT_EQ(kMaxGoawayUri, 8192u);
    // 8192 bytes is the largest accepted URI.
    const auto ok = decode_g(uri_wire(8192));
    ASSERT_TRUE(std::holds_alternative<GoawayMessage>(ok));
    EXPECT_EQ(std::get<GoawayMessage>(ok).new_session_uri.size(), 8192u);
    // 8193 bytes is rejected from the length prefix alone: the whole message is present here...
    const auto too_long = decode_g(uri_wire(8193));
    ASSERT_EQ(error_code(too_long), DecodeErrorCode::LengthExceedsLimit);
    // ...and a message whose body holds only the length prefix (60 01 = 8193) is rejected the same way, never
    // read as a truncated string.
    EXPECT_EQ(error_code(decode_g(bytes({0x02, 0x60, 0x01}))), DecodeErrorCode::LengthExceedsLimit);
    // The error cites the rule.
    const auto* error = std::get_if<DecodeError>(&too_long);
    ASSERT_NE(error, nullptr);
    EXPECT_NE(std::string(error->detail).find("7.18"), std::string::npos);
}

TEST(Moqlite06Goaway, ConfiguredLimitsCanBeStricter) {
    DecodeLimits limits;
    limits.max_string_length = 4;
    EXPECT_EQ(error_code(decode_g(kUriWire, limits)), DecodeErrorCode::LengthExceedsLimit);
    limits.max_string_length = 1u << 16;
    limits.max_message_length = 1;
    EXPECT_EQ(error_code(decode_g(kUriWire, limits)), DecodeErrorCode::LengthExceedsLimit);
}

TEST(Moqlite06Goaway, ErrorsLeaveTheCursorUnmovedAndHugeOffsetsDoNotThrow) {
    const Bytes bad = bytes({0x02, 0x00, 0x00});
    Cursor input(bad);
    EXPECT_EQ(error_code(decode_goaway(input)), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.remaining(), bad.size());

    Cursor huge(kUriWire, std::numeric_limits<std::size_t>::max());
    EXPECT_NO_THROW({
        const auto result = decode_goaway(huge);
        EXPECT_FALSE(std::holds_alternative<GoawayMessage>(result));
    });
}

TEST(Moqlite06Goaway, RoundTripsAndEncoderLimits) {
    for (const std::string uri : {std::string(), std::string("https://relay.example/new"), std::string(8192, 'u')}) {
        const auto result = decode_g(encode(GoawayMessage{uri}));
        ASSERT_TRUE(std::holds_alternative<GoawayMessage>(result));
        EXPECT_EQ(std::get<GoawayMessage>(result).new_session_uri, uri);
    }
    ByteWriter out(1u << 16);
    EXPECT_EQ(encode_goaway(GoawayMessage{std::string(8193, 'u')}, out),
              std::optional<EncodeError>(EncodeError::LimitExceeded));
    ByteWriter tiny(3);
    EXPECT_EQ(encode_goaway(GoawayMessage{"moql://b/x"}, tiny), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_EQ(tiny.size(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
