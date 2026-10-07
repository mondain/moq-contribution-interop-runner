#include "moq/interop/wire/moqlite06/announce.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <random>
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

template <class T, class Fn>
Bytes encode_with(const T& message, Fn fn, const DecodeLimits& limits = kDefaultLimits) {
    ByteWriter out(1u << 16);
    EXPECT_FALSE(fn(message, out, limits).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

Bytes encode(const AnnounceRequest& m) { return encode_with(m, encode_announce_request); }
Bytes encode(const AnnounceOk& m) { return encode_with(m, encode_announce_ok); }
Bytes encode(const AnnounceMessage& m) { return encode_with(m, encode_announce_message); }

DecodeResult<AnnounceMessage> decode_message(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_announce_message(input, limits);
}

AnnounceMessage message_ok(const Bytes& data) {
    Cursor input(data);
    const auto result = decode_announce_message(input);
    EXPECT_TRUE(std::holds_alternative<AnnounceMessage>(result));
    EXPECT_EQ(input.remaining(), 0u);
    return std::get<AnnounceMessage>(result);
}

// ANNOUNCE_REQUEST prefix "a": body = Prefix (s) = 01 61, so Message Length 2.
TEST(Moqlite06Announce, RequestVectors) {
    const AnnounceRequest a{"a"};
    EXPECT_EQ(encode(a), bytes({0x02, 0x01, 0x61}));
    Bytes wire = bytes({0x02, 0x01, 0x61});
    Cursor input(wire);
    const auto result = decode_announce_request(input);
    ASSERT_TRUE(std::holds_alternative<AnnounceRequest>(result));
    EXPECT_EQ(std::get<AnnounceRequest>(result), a);
    EXPECT_EQ(input.remaining(), 0u);

    // Empty prefix: body = Prefix (s) = 00, Message Length 1.
    const AnnounceRequest empty{""};
    EXPECT_EQ(encode(empty), bytes({0x01, 0x00}));
    wire = bytes({0x01, 0x00});
    Cursor again(wire);
    const auto result2 = decode_announce_request(again);
    ASSERT_TRUE(std::holds_alternative<AnnounceRequest>(result2));
    EXPECT_EQ(std::get<AnnounceRequest>(result2), empty);
}

TEST(Moqlite06Announce, RequestViolations) {
    // String length 5 but only one byte follows inside Message Length 2.
    Bytes wire = bytes({0x02, 0x05, 0x61});
    Cursor a(wire);
    EXPECT_EQ(error_code(decode_announce_request(a)), DecodeErrorCode::ProtocolViolation);
    // Trailing byte: Message Length 3, string "a" uses 2.
    wire = bytes({0x03, 0x01, 0x61, 0x00});
    Cursor b(wire);
    EXPECT_EQ(error_code(decode_announce_request(b)), DecodeErrorCode::ProtocolViolation);
    // Empty body (Message Length 0): no prefix at all.
    wire = bytes({0x00});
    Cursor c(wire);
    EXPECT_EQ(error_code(decode_announce_request(c)), DecodeErrorCode::ProtocolViolation);
    // Prefix longer than max_string_length.
    DecodeLimits limits;
    limits.max_string_length = 1;
    wire = bytes({0x03, 0x02, 0x61, 0x62});
    Cursor d(wire);
    EXPECT_EQ(error_code(decode_announce_request(d, limits)), DecodeErrorCode::LengthExceedsLimit);
    // Short input.
    wire = bytes({0x02, 0x01});
    Cursor e(wire);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_announce_request(e)));
    EXPECT_EQ(e.remaining(), 2u);
}

// ANNOUNCE_OK hop 0, count 0: body 00 00, Message Length 2.
// hop 0x1234: 2-byte varint is 0x40|0x12 = 0x52, 0x34; count 3 is 03; body is 3 bytes, Message Length 3.
TEST(Moqlite06Announce, OkVectors) {
    struct Case {
        AnnounceOk value;
        Bytes wire;
    };
    const std::vector<Case> cases = {
        {AnnounceOk{0, 0}, bytes({0x02, 0x00, 0x00})},
        {AnnounceOk{0x1234, 3}, bytes({0x03, 0x52, 0x34, 0x03})},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(encode(c.value), c.wire);
        Cursor input(c.wire);
        const auto result = decode_announce_ok(input);
        ASSERT_TRUE(std::holds_alternative<AnnounceOk>(result));
        EXPECT_EQ(std::get<AnnounceOk>(result), c.value);
        EXPECT_EQ(input.remaining(), 0u);
    }
}

TEST(Moqlite06Announce, OkViolations) {
    // Only the Hop ID is present.
    Bytes wire = bytes({0x01, 0x07});
    Cursor a(wire);
    EXPECT_EQ(error_code(decode_announce_ok(a)), DecodeErrorCode::ProtocolViolation);
    // Trailing byte after both fields.
    wire = bytes({0x03, 0x01, 0x02, 0x03});
    Cursor b(wire);
    EXPECT_EQ(error_code(decode_announce_ok(b)), DecodeErrorCode::ProtocolViolation);
    // A 2-byte Hop ID varint cut off by the Message Length (length 1, first byte 0x52 announces 2 bytes).
    wire = bytes({0x01, 0x52, 0x34});
    Cursor c(wire);
    EXPECT_EQ(error_code(decode_announce_ok(c)), DecodeErrorCode::ProtocolViolation);
}

// ANNOUNCE_START: Type 00; body = suffix "b" (01 62) + Hop Count 02 + hops 07 09 + warm 00 + cold 01
// = 2 + 1 + 2 + 1 + 1 = 7 bytes, so Message Length 07.
// A failed decode leaves the input cursor exactly where it was, for every error class.
TEST(Moqlite06Announce, ErrorsLeaveTheCursorUnmoved) {
    const std::vector<Bytes> request_cases = {
        bytes({0x02, 0x05, 0x61}),        // string length beyond the body
        bytes({0x03, 0x01, 0x61, 0x00}),  // trailing byte
        bytes({0x00}),                    // empty body
    };
    for (const auto& wire : request_cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_announce_request(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
    const std::vector<Bytes> ok_cases = {
        bytes({0x01, 0x07}),
        bytes({0x03, 0x01, 0x02, 0x03}),
        bytes({0x01, 0x52, 0x34}),
    };
    for (const auto& wire : ok_cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_announce_ok(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
    const std::vector<Bytes> message_cases = {
        bytes({0x01, 0x00}),              // END with Message Length 0: no announce id
        bytes({0x01, 0x02, 0x05, 0x00}),  // END with a trailing byte
    };
    for (const auto& wire : message_cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_announce_message(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
}

// The string limit is judged from the length prefix alone: a 0x3f-byte string with the body cut off right
// after the prefix is LengthExceedsLimit, not a body shortfall (ProtocolViolation) or short input (NeedMore).
TEST(Moqlite06Announce, StringLimitIsCheckedBeforeTheBytesAreRead) {
    DecodeLimits limits;
    limits.max_string_length = 1;
    // ANNOUNCE_REQUEST: Message Length 1, body = 3f.
    const auto request_wire = bytes({0x01, 0x3f});
    Cursor request_input(request_wire);
    EXPECT_EQ(error_code(decode_announce_request(request_input, limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(request_input.remaining(), request_wire.size());
    // ANNOUNCE_START (Type 00): Message Length 1, body = 3f.
    const auto start_wire = bytes({0x00, 0x01, 0x3f});
    Cursor start_input(start_wire);
    EXPECT_EQ(error_code(decode_announce_message(start_input, limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(start_input.remaining(), start_wire.size());
}

TEST(Moqlite06Announce, StartTwoHops) {
    const AnnounceMessage message = AnnounceStart{"b", RouteMetadata{{7, 9}, 0, 1}};
    const auto wire = bytes({0x00, 0x07, 0x01, 0x62, 0x02, 0x07, 0x09, 0x00, 0x01});
    EXPECT_EQ(encode(message), wire);
    EXPECT_EQ(message_ok(wire), message);
}

// Zero hops, empty suffix: body = 00 (suffix) 00 (count) 04 (warm 4) 05 (cold 5), Message Length 4.
TEST(Moqlite06Announce, StartEmptySuffixZeroHops) {
    const AnnounceMessage message = AnnounceStart{"", RouteMetadata{{}, 4, 5}};
    const auto wire = bytes({0x00, 0x04, 0x00, 0x00, 0x04, 0x05});
    EXPECT_EQ(encode(message), wire);
    EXPECT_EQ(message_ok(wire), message);
}

// Costs are raw 62-bit values: warm 100 = 40 64, cold kMaxVarint = ff ff ff ff ff ff ff ff.
// Body = 01 62 (suffix) 00 (count) 40 64 + 8 bytes = 2 + 1 + 2 + 8 = 13, Message Length 0d.
TEST(Moqlite06Announce, StartCostsAreRaw) {
    const AnnounceMessage message = AnnounceStart{"b", RouteMetadata{{}, 100, kMaxVarint}};
    const auto wire = bytes({0x00, 0x0d, 0x01, 0x62, 0x00, 0x40, 0x64, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    EXPECT_EQ(encode(message), wire);
    EXPECT_EQ(message_ok(wire), message);
}

// ANNOUNCE_END id 5: Type 01, body 05, Message Length 1.
TEST(Moqlite06Announce, EndVector) {
    const AnnounceMessage message = AnnounceEnd{5};
    const auto wire = bytes({0x01, 0x01, 0x05});
    EXPECT_EQ(encode(message), wire);
    EXPECT_EQ(message_ok(wire), message);
}

// ANNOUNCE_UPDATE id 5, one hop (7), warm 2, cold 3: Type 02; body = 05 01 07 02 03, Message Length 5.
TEST(Moqlite06Announce, UpdateVector) {
    const AnnounceMessage message = AnnounceUpdate{5, RouteMetadata{{7}, 2, 3}};
    const auto wire = bytes({0x02, 0x05, 0x05, 0x01, 0x07, 0x02, 0x03});
    EXPECT_EQ(encode(message), wire);
    EXPECT_EQ(message_ok(wire), message);
}

TEST(Moqlite06Announce, DispatcherPicksAlternativeAndRejectsUnknownType) {
    EXPECT_TRUE(std::holds_alternative<AnnounceStart>(message_ok(bytes({0x00, 0x04, 0x00, 0x00, 0x00, 0x00}))));
    EXPECT_TRUE(std::holds_alternative<AnnounceEnd>(message_ok(bytes({0x01, 0x01, 0x00}))));
    EXPECT_TRUE(std::holds_alternative<AnnounceUpdate>(message_ok(bytes({0x02, 0x04, 0x00, 0x00, 0x00, 0x00}))));

    // Type 3 is unknown even though a well-formed length-prefixed body follows.
    const auto wire = bytes({0x03, 0x01, 0x00});
    Cursor input(wire);
    EXPECT_EQ(error_code(decode_announce_message(input)), DecodeErrorCode::InvalidValue);
    EXPECT_EQ(input.remaining(), 3u);
}

TEST(Moqlite06Announce, RoundTripsAllSixMessages) {
    const AnnounceRequest request{"room/"};
    {
        const auto wire = encode(request);
        Cursor input(wire);
        const auto result = decode_announce_request(input);
        ASSERT_TRUE(std::holds_alternative<AnnounceRequest>(result));
        EXPECT_EQ(std::get<AnnounceRequest>(result), request);
    }
    const AnnounceOk ok{0x3fffffffffffffffULL, 17};
    {
        const auto wire = encode(ok);
        Cursor input(wire);
        const auto result = decode_announce_ok(input);
        ASSERT_TRUE(std::holds_alternative<AnnounceOk>(result));
        EXPECT_EQ(std::get<AnnounceOk>(result), ok);
    }
    const std::vector<AnnounceMessage> messages = {
        AnnounceStart{"cam/1", RouteMetadata{{0x1234, 0, 0, 99999999}, 12, 30}},
        AnnounceStart{"", RouteMetadata{}},
        AnnounceEnd{70000},
        AnnounceUpdate{3, RouteMetadata{{5, 6}, 30, 12}},
        AnnounceUpdate{0, RouteMetadata{}},
    };
    for (const auto& message : messages) EXPECT_EQ(message_ok(encode(message)), message);
}

// Hop Count 2 but one Hop ID: body = 00 (suffix) 02 (count) 07 (hop), Message Length 3; the body ends
// before the second hop.
TEST(Moqlite06Announce, HopCountLargerThanBodyIsViolation) {
    const auto wire = bytes({0x00, 0x03, 0x00, 0x02, 0x07});
    EXPECT_EQ(error_code(decode_message(wire)), DecodeErrorCode::ProtocolViolation);
    // The costs are missing too: count 1, hop 7, warm 0, no cold. Message Length 4.
    const auto no_cold = bytes({0x00, 0x04, 0x00, 0x01, 0x07, 0x00});
    EXPECT_EQ(error_code(decode_message(no_cold)), DecodeErrorCode::ProtocolViolation);
}

// Hop Count 1 but the body carries two hops worth of bytes: suffix 00, count 01, hop 07, warm 08, cold 09,
// then one more byte 0a. Message Length 6, the last byte is trailing.
TEST(Moqlite06Announce, TrailingBytesAreViolation) {
    const auto wire = bytes({0x00, 0x06, 0x00, 0x01, 0x07, 0x08, 0x09, 0x0a});
    EXPECT_EQ(error_code(decode_message(wire)), DecodeErrorCode::ProtocolViolation);
    // END with a trailing byte.
    EXPECT_EQ(error_code(decode_message(bytes({0x01, 0x02, 0x05, 0x00}))), DecodeErrorCode::ProtocolViolation);
    // Update: id 5, count 0, warm 0, cold 0, plus one stray byte.
    EXPECT_EQ(error_code(decode_message(bytes({0x02, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
}

// Message Length shorter than the fields: END with Message Length 0 has no Announce ID.
TEST(Moqlite06Announce, MessageLengthShorterThanFields) {
    EXPECT_EQ(error_code(decode_message(bytes({0x01, 0x00}))), DecodeErrorCode::ProtocolViolation);
    // Update whose body (id 5, count 0, warm 0) lacks the cold cost: Message Length 3.
    EXPECT_EQ(error_code(decode_message(bytes({0x02, 0x03, 0x05, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
}

// Update id 5, hops 7 7, warm 0, cold 0: body = 05 02 07 07 00 00, Message Length 6.
TEST(Moqlite06Announce, DuplicateNonZeroHopIsViolationButZerosAreLegal) {
    EXPECT_EQ(error_code(decode_message(bytes({0x02, 0x06, 0x05, 0x02, 0x07, 0x07, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
    // Start: suffix "", hops 7 8 7 (non-adjacent duplicate), warm 0, cold 0: body 00 03 07 08 07 00 00, length 7.
    EXPECT_EQ(error_code(decode_message(bytes({0x00, 0x07, 0x00, 0x03, 0x07, 0x08, 0x07, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
    // Duplicate zeros: body 05 02 00 00 00 00, length 6.
    const auto zeros = message_ok(bytes({0x02, 0x06, 0x05, 0x02, 0x00, 0x00, 0x00, 0x00}));
    EXPECT_EQ(zeros, (AnnounceMessage{AnnounceUpdate{5, RouteMetadata{{0, 0}, 0, 0}}}));
}

// Hop Count 1025 = 0x401 is the 2-byte varint (0x40 | 0x04), 0x01 = 44 01.
// Body = 00 (suffix) 44 01 and nothing else, Message Length 3: the limit check must fire before the missing
// hop bytes are noticed, and nothing is sized by the claimed count.
TEST(Moqlite06Announce, HopCountOverLimitIsLengthExceedsLimit) {
    const auto wire = bytes({0x00, 0x03, 0x00, 0x44, 0x01});
    EXPECT_EQ(error_code(decode_message(wire)), DecodeErrorCode::LengthExceedsLimit);
    // A huge claimed count (kMaxVarint) is the same: body = 00 (suffix) + 8-byte varint, Message Length 9.
    const auto huge = bytes({0x00, 0x09, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    EXPECT_EQ(error_code(decode_message(huge)), DecodeErrorCode::LengthExceedsLimit);
    // With max_hops 1: count 2 (body 00 02 07 08, length 4) is over; count 1 (body 00 01 07 00 00, length 5)
    // is exactly at the limit and decodes.
    DecodeLimits limits;
    limits.max_hops = 1;
    EXPECT_EQ(error_code(decode_message(bytes({0x00, 0x04, 0x00, 0x02, 0x07, 0x08}), limits)),
              DecodeErrorCode::LengthExceedsLimit);
    EXPECT_TRUE(std::holds_alternative<AnnounceMessage>(
        decode_message(bytes({0x00, 0x05, 0x00, 0x01, 0x07, 0x00, 0x00}), limits)));
}

TEST(Moqlite06Announce, MessageLengthOverLimit) {
    DecodeLimits limits;
    limits.max_message_length = 2;
    EXPECT_EQ(error_code(decode_message(bytes({0x01, 0x03, 0x01, 0x02, 0x03}), limits)),
              DecodeErrorCode::LengthExceedsLimit);
}

TEST(Moqlite06Announce, EveryPrefixIsNeedMoreAndLeavesTheCursor) {
    const std::vector<Bytes> wires = {
        bytes({0x00, 0x07, 0x01, 0x62, 0x02, 0x07, 0x09, 0x00, 0x01}),
        bytes({0x01, 0x01, 0x05}),
        bytes({0x02, 0x05, 0x05, 0x01, 0x07, 0x02, 0x03}),
    };
    for (const auto& wire : wires) {
        for (std::size_t cut = 0; cut < wire.size(); ++cut) {
            const Bytes prefix(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            const auto result = decode_announce_message(input);
            EXPECT_TRUE(std::holds_alternative<NeedMore>(result)) << "cut " << cut;
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
    const std::vector<Bytes> requests = {bytes({0x02, 0x01, 0x61}), bytes({0x03, 0x52, 0x34, 0x03})};
    for (std::size_t cut = 0; cut < requests[0].size(); ++cut) {
        const Bytes prefix(requests[0].begin(), requests[0].begin() + static_cast<std::ptrdiff_t>(cut));
        Cursor input(prefix);
        EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_announce_request(input))) << cut;
    }
    for (std::size_t cut = 0; cut < requests[1].size(); ++cut) {
        const Bytes prefix(requests[1].begin(), requests[1].begin() + static_cast<std::ptrdiff_t>(cut));
        Cursor input(prefix);
        EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_announce_ok(input))) << cut;
    }
}

TEST(Moqlite06Announce, ConsecutiveMessagesAdvanceTheCursor) {
    Bytes wire = bytes({0x01, 0x01, 0x05});
    const Bytes second = bytes({0x01, 0x01, 0x06});
    wire.insert(wire.end(), second.begin(), second.end());
    Cursor input(wire);
    const auto a = decode_announce_message(input);
    const auto b = decode_announce_message(input);
    ASSERT_TRUE(std::holds_alternative<AnnounceMessage>(a));
    ASSERT_TRUE(std::holds_alternative<AnnounceMessage>(b));
    EXPECT_EQ(std::get<AnnounceMessage>(a), (AnnounceMessage{AnnounceEnd{5}}));
    EXPECT_EQ(std::get<AnnounceMessage>(b), (AnnounceMessage{AnnounceEnd{6}}));
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06Announce, EncoderRefusals) {
    ByteWriter out(256);
    // Duplicate non-zero Hop ID.
    EXPECT_EQ(encode_announce_message(AnnounceUpdate{1, RouteMetadata{{7, 7}, 0, 0}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    // Duplicate zeros are fine.
    EXPECT_FALSE(encode_announce_message(AnnounceUpdate{1, RouteMetadata{{0, 0}, 0, 0}}, out).has_value());
    const auto written = out.size();

    // Values above kMaxVarint.
    EXPECT_EQ(encode_announce_message(AnnounceEnd{kMaxVarint + 1}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_announce_message(AnnounceStart{"", RouteMetadata{{}, kMaxVarint + 1, 0}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_announce_message(AnnounceStart{"", RouteMetadata{{}, 0, kMaxVarint + 1}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_announce_message(AnnounceUpdate{1, RouteMetadata{{kMaxVarint + 1}, 0, 0}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_announce_ok(AnnounceOk{kMaxVarint + 1, 0}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_announce_ok(AnnounceOk{0, kMaxVarint + 1}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));

    // Hop count above max_hops.
    DecodeLimits limits;
    limits.max_hops = 2;
    EXPECT_EQ(encode_announce_message(AnnounceUpdate{1, RouteMetadata{{1, 2, 3}, 0, 0}}, out, limits),
              std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_FALSE(encode_announce_message(AnnounceUpdate{1, RouteMetadata{{1, 2}, 0, 0}}, out, limits).has_value());
    const auto after_ok = out.size();

    // String over max_string_length.
    limits.max_string_length = 3;
    EXPECT_EQ(encode_announce_message(AnnounceStart{"abcd", RouteMetadata{}}, out, limits),
              std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(encode_announce_request(AnnounceRequest{"abcd"}, out, limits),
              std::optional<EncodeError>(EncodeError::LimitExceeded));

    EXPECT_EQ(out.size(), after_ok);  // nothing written by any refusal
    EXPECT_GT(after_ok, written);
}

TEST(Moqlite06Announce, EncoderWritesNothingWithoutRoom) {
    ByteWriter small(2);
    EXPECT_EQ(encode_announce_message(AnnounceEnd{5}, small), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(small.size(), 0u);
    EXPECT_EQ(encode_announce_request(AnnounceRequest{"a"}, small),
              std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(small.size(), 0u);
    ByteWriter none(0);
    EXPECT_EQ(encode_announce_ok(AnnounceOk{0, 0}, none), std::optional<EncodeError>(EncodeError::OutputCapacity));
}

TEST(Moqlite06Announce, RandomBytesNeverCrash) {
    std::mt19937 rng(0x1b3);
    for (int round = 0; round < 20000; ++round) {
        Bytes data(rng() % 24);
        for (auto& b : data) b = static_cast<std::byte>(rng());
        if (!data.empty() && (rng() % 2 == 0)) data[0] = static_cast<std::byte>(rng() % 4);
        Cursor input(data);
        const auto before = input.remaining();
        const auto result = decode_announce_message(input);
        if (std::holds_alternative<AnnounceMessage>(result)) {
            EXPECT_LT(input.remaining(), before);
        } else {
            EXPECT_EQ(input.remaining(), before);
        }
        Cursor req(data);
        (void)decode_announce_request(req);
        Cursor okc(data);
        (void)decode_announce_ok(okc);
    }
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
