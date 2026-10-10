#include "moq/interop/wire/moqlite06/fetch.h"

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

Bytes encode(const FetchRequest& m) {
    ByteWriter out(1u << 16);
    EXPECT_FALSE(encode_fetch_request(m, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

DecodeResult<FetchRequest> decode_f(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_fetch_request(input, limits);
}

struct Vector {
    const char* name;
    Bytes wire;
    FetchRequest value;
};

const std::vector<Vector>& vectors() {
    static const std::vector<Vector> table = {
        // path "b" (01 62), track "t" (01 74), priority 80, group 0, frame start 0, frame end 0 (to the end).
        // Body = 2 + 2 + 1 + 1 + 1 + 1 = 8.
        {"whole_group", bytes({0x08, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00}),
         FetchRequest{"b", "t", 0x80, 0, 0, 0}},
        // path "ab" (02 61 62), track "c" (01 63), priority 7f, group 300 (0x012c -> 41 2c), start 2, end 5.
        // Body = 3 + 2 + 1 + 2 + 1 + 1 = 10 = 0x0a.
        {"range", bytes({0x0a, 0x02, 0x61, 0x62, 0x01, 0x63, 0x7f, 0x41, 0x2c, 0x02, 0x05}),
         FetchRequest{"ab", "c", 0x7f, 300, 2, 5}},
        // Maximum varints: priority ff (one raw byte), group 2^62-1, frame start 2^62-2 (ff*7 fe), frame end 2^62-1
        // (index 2^62-2 = the start: a legal single frame). Body = 2 + 2 + 1 + 24 = 29 = 0x1d.
        {"maximum",
         bytes({0x1d, 0x01, 0x62, 0x01, 0x74, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                0xff, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}),
         FetchRequest{"b", "t", 0xff, kMaxVarint, kMaxVarint - 1, kMaxVarint}},
        // A single frame: start 5, end 6 (index 5, encoded index+1).
        {"single_frame", bytes({0x08, 0x01, 0x62, 0x01, 0x74, 0x00, 0x00, 0x05, 0x06}),
         FetchRequest{"b", "t", 0, 0, 5, 6}},
        // Priority 00 with a non-zero group 1.
        {"priority_zero", bytes({0x08, 0x01, 0x62, 0x01, 0x74, 0x00, 0x01, 0x00, 0x00}),
         FetchRequest{"b", "t", 0, 1, 0, 0}},
    };
    return table;
}

// Inverted: start 5, end 3 (index 2) -> body 01 62 01 74 00 00 05 03, length 08. The codec decodes it.
const Bytes kInvertedWire = bytes({0x08, 0x01, 0x62, 0x01, 0x74, 0x00, 0x00, 0x05, 0x03});

TEST(Moqlite06Fetch, VectorsDecodeAndEncode) {
    for (const auto& v : vectors()) {
        SCOPED_TRACE(v.name);
        Cursor input(v.wire);
        const auto result = decode_fetch_request(input);
        ASSERT_TRUE(std::holds_alternative<FetchRequest>(result));
        EXPECT_EQ(std::get<FetchRequest>(result), v.value);
        EXPECT_EQ(encode(v.value), v.wire);
        EXPECT_EQ(input.remaining(), 0u);
    }
}

TEST(Moqlite06Fetch, EveryPrefixOfEveryVectorIsNeedMore) {
    for (const auto& v : vectors()) {
        for (std::size_t cut = 0; cut < v.wire.size(); ++cut) {
            SCOPED_TRACE(std::string(v.name) + " cut " + std::to_string(cut));
            const Bytes prefix(v.wire.begin(), v.wire.begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_fetch_request(input)));
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
}

TEST(Moqlite06Fetch, InvertedRangeDecodesAndIsReported) {
    const auto result = decode_f(kInvertedWire);
    ASSERT_TRUE(std::holds_alternative<FetchRequest>(result));
    const auto& request = std::get<FetchRequest>(result);
    EXPECT_EQ(request, (FetchRequest{"b", "t", 0, 0, 5, 3}));
    EXPECT_TRUE(fetch_range_inverted(request));
}

TEST(Moqlite06Fetch, InvertedTruthTable) {
    auto inverted = [](std::uint64_t start, std::uint64_t end) {
        return fetch_range_inverted(FetchRequest{"b", "t", 0, 0, start, end});
    };
    EXPECT_FALSE(inverted(0, 0));   // to the end
    EXPECT_FALSE(inverted(9, 0));   // to the end from 9
    EXPECT_FALSE(inverted(0, 1));   // frame 0 only
    EXPECT_FALSE(inverted(5, 6));   // frame 5 only: equal bounds are a legal single frame
    EXPECT_FALSE(inverted(2, 5));   // frames 2..4
    EXPECT_TRUE(inverted(5, 5));    // end index 4 < start 5
    EXPECT_TRUE(inverted(5, 3));    // end index 2 < start 5
    EXPECT_TRUE(inverted(1, 1));    // end index 0 < start 1
}

TEST(Moqlite06Fetch, MessageLengthLiesAreProtocolViolations) {
    // Trailing byte: length 09 over the 8-byte body plus a stray 00.
    EXPECT_EQ(error_code(decode_f(bytes({0x09, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
    // One byte short: the Frame End is missing, length 07.
    EXPECT_EQ(error_code(decode_f(bytes({0x07, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_f(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);
    // A multi-byte varint cut by the Message Length: group claims two bytes (41) but one remains.
    EXPECT_EQ(error_code(decode_f(bytes({0x06, 0x01, 0x62, 0x01, 0x74, 0x00, 0x41}))), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Fetch, StringLimitIsEnforcedBeforeReading) {
    DecodeLimits limits;
    limits.max_string_length = 1;
    EXPECT_EQ(error_code(decode_f(bytes({0x03, 0x02, 0x61, 0x62}), limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(error_code(decode_f(bytes({0x05, 0x01, 0x62, 0x02, 0x74, 0x74}), limits)),
              DecodeErrorCode::LengthExceedsLimit);
    EXPECT_TRUE(std::holds_alternative<FetchRequest>(decode_f(vectors()[0].wire, limits)));
    limits.max_message_length = 2;
    EXPECT_EQ(error_code(decode_f(vectors()[0].wire, limits)), DecodeErrorCode::LengthExceedsLimit);
}

TEST(Moqlite06Fetch, ErrorsLeaveTheCursorUnmoved) {
    const std::vector<Bytes> cases = {
        bytes({0x09, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x07, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00}),
        bytes({0x00}),
    };
    for (const auto& wire : cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_fetch_request(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
}

TEST(Moqlite06Fetch, MaximumOffsetCursorReturnsAnErrorWithoutThrowing) {
    const Bytes wire = vectors()[1].wire;
    Cursor input(wire, std::numeric_limits<std::size_t>::max());
    EXPECT_NO_THROW({
        const auto result = decode_fetch_request(input);
        EXPECT_FALSE(std::holds_alternative<FetchRequest>(result));
    });
}

TEST(Moqlite06Fetch, RoundTrips) {
    const std::vector<FetchRequest> requests = {
        {"", "", 0, 0, 0, 0},
        {"room/cam", "video", 3, 1, 2, 9},
        {std::string(300, 'p'), "t", 0xff, kMaxVarint, 0, kMaxVarint},
    };
    for (const auto& r : requests) {
        const auto result = decode_f(encode(r));
        ASSERT_TRUE(std::holds_alternative<FetchRequest>(result));
        EXPECT_EQ(std::get<FetchRequest>(result), r);
    }
}

TEST(Moqlite06Fetch, TypedEncoderRefusesAnInvertedRangeAndBadValues) {
    ByteWriter out(256);
    EXPECT_EQ(encode_fetch_request(FetchRequest{"b", "t", 0, 0, 5, 3}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_fetch_request(FetchRequest{"b", "t", 0, kMaxVarint + 1, 0, 0}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_fetch_request(FetchRequest{"b", "t", 0, 0, kMaxVarint + 1, 0}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_fetch_request(FetchRequest{"b", "t", 0, 0, 0, kMaxVarint + 1}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    DecodeLimits limits;
    limits.max_string_length = 1;
    EXPECT_EQ(encode_fetch_request(FetchRequest{"bb", "t", 0, 0, 0, 0}, out, limits), std::optional<EncodeError>(EncodeError::LimitExceeded));
    ByteWriter tiny(3);
    EXPECT_EQ(encode_fetch_request(FetchRequest{"b", "t", 0, 0, 0, 0}, tiny), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_EQ(tiny.size(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
