#include "moq/interop/wire/moqlite06/track.h"

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

Bytes encode(const TrackRequest& m) {
    ByteWriter out(1u << 16);
    EXPECT_FALSE(encode_track_request(m, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}
Bytes encode(const TrackInfo& m) {
    ByteWriter out(1u << 16);
    EXPECT_FALSE(encode_track_info(m, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

DecodeResult<TrackRequest> decode_q(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_track_request(input, limits);
}
DecodeResult<TrackInfo> decode_i(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_track_info(input, limits);
}

// "interop.hang" = 69 6e 74 65 72 6f 70 2e 68 61 6e 67 (12 bytes), "0.m4s" = 30 2e 6d 34 73 (5 bytes).
// Body = 0c + 12 + 05 + 5 = 19 = 0x13.
const Bytes kTrackWire = bytes({0x13, 0x0c, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x6f, 0x70, 0x2e, 0x68, 0x61, 0x6e, 0x67,
                                0x05, 0x30, 0x2e, 0x6d, 0x34, 0x73});

// TRACK_INFO (60, 30000, 90000): priority 3c is one raw byte; 30000 > 16383 so it is the 4-byte form 80 00 75 30;
// 90000 = 0x00015f90 -> 80 01 5f 90. Body = 1 + 4 + 4 = 9.
const Bytes kInfoWire = bytes({0x09, 0x3c, 0x80, 0x00, 0x75, 0x30, 0x80, 0x01, 0x5f, 0x90});
// (255, 2^62-1, 1): ff, ff*8, 01. Body = 1 + 8 + 1 = 10 = 0x0a.
const Bytes kInfoMaxWire = bytes({0x0a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01});
// The CLI's "unlimited" max age 2^53-1 = 0x001fffffffffffff as the 8-byte form c0 1f ff ff ff ff ff ff.
const Bytes kInfoUnlimitedWire = bytes({0x0a, 0x3c, 0xc0, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01});

TEST(Moqlite06Track, RequestGolden) {
    const auto result = decode_q(kTrackWire);
    ASSERT_TRUE(std::holds_alternative<TrackRequest>(result));
    EXPECT_EQ(std::get<TrackRequest>(result), (TrackRequest{"interop.hang", "0.m4s"}));
    EXPECT_EQ(encode(TrackRequest{"interop.hang", "0.m4s"}), kTrackWire);
}

TEST(Moqlite06Track, InfoGoldens) {
    const auto a = decode_i(kInfoWire);
    ASSERT_TRUE(std::holds_alternative<TrackInfo>(a));
    EXPECT_EQ(std::get<TrackInfo>(a), (TrackInfo{60, 30000, 90000}));
    EXPECT_EQ(encode(TrackInfo{60, 30000, 90000}), kInfoWire);

    const auto b = decode_i(kInfoMaxWire);
    ASSERT_TRUE(std::holds_alternative<TrackInfo>(b));
    EXPECT_EQ(std::get<TrackInfo>(b), (TrackInfo{255, kMaxVarint, 1}));
    EXPECT_EQ(encode(TrackInfo{255, kMaxVarint, 1}), kInfoMaxWire);
}

TEST(Moqlite06Track, UnlimitedMaxAgeTheCliSendsDecodes) {
    const auto result = decode_i(kInfoUnlimitedWire);
    ASSERT_TRUE(std::holds_alternative<TrackInfo>(result));
    EXPECT_EQ(std::get<TrackInfo>(result).publisher_max_age_ms, 9007199254740991ULL);
}

// A zero timescale is decoded as-is: judging it is the evaluator's job (draft 7.12).
TEST(Moqlite06Track, ZeroTimescaleDecodes) {
    // priority 00, max age 00, timescale 00; body 3.
    const auto result = decode_i(bytes({0x03, 0x00, 0x00, 0x00}));
    ASSERT_TRUE(std::holds_alternative<TrackInfo>(result));
    EXPECT_EQ(std::get<TrackInfo>(result), (TrackInfo{0, 0, 0}));
}

TEST(Moqlite06Track, EveryPrefixIsNeedMore) {
    for (const auto* wire : {&kTrackWire, &kInfoWire, &kInfoMaxWire, &kInfoUnlimitedWire}) {
        for (std::size_t cut = 0; cut < wire->size(); ++cut) {
            SCOPED_TRACE("cut " + std::to_string(cut) + " of " + std::to_string(wire->size()));
            const Bytes prefix(wire->begin(), wire->begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            const bool need_more = (wire == &kTrackWire)
                                       ? std::holds_alternative<NeedMore>(decode_track_request(input))
                                       : std::holds_alternative<NeedMore>(decode_track_info(input));
            EXPECT_TRUE(need_more);
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
}

TEST(Moqlite06Track, MessageLengthLiesAreProtocolViolations) {
    // TRACK: trailing byte (length 0x14 over the 19-byte body plus a stray 00), then one byte short (0x12).
    Bytes extra = kTrackWire;
    extra[0] = std::byte{0x14};
    extra.push_back(std::byte{0});
    EXPECT_EQ(error_code(decode_q(extra)), DecodeErrorCode::ProtocolViolation);
    Bytes shorter(kTrackWire.begin(), kTrackWire.end() - 1);
    shorter[0] = std::byte{0x12};
    EXPECT_EQ(error_code(decode_q(shorter)), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_q(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);
    // TRACK_INFO: trailing byte (length 4 over a 3-byte body), one short (length 2), empty.
    EXPECT_EQ(error_code(decode_i(bytes({0x04, 0x00, 0x00, 0x00, 0x00}))), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_i(bytes({0x02, 0x00, 0x00}))), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_i(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);
    // A 2-byte varint cut off by the Message Length: priority, then 41 claims two bytes but only one remains.
    EXPECT_EQ(error_code(decode_i(bytes({0x02, 0x10, 0x41}))), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Track, StringLimitIsEnforcedBeforeReading) {
    DecodeLimits limits;
    limits.max_string_length = 4;
    // Path length 5 > 4; the bytes are not even present in the body.
    EXPECT_EQ(error_code(decode_q(bytes({0x02, 0x05, 0x61}), limits)), DecodeErrorCode::LengthExceedsLimit);
    // Track name over the limit: path "b" (01 62), track length 5.
    EXPECT_EQ(error_code(decode_q(bytes({0x04, 0x01, 0x62, 0x05, 0x61}), limits)), DecodeErrorCode::LengthExceedsLimit);
    // Exactly at the limit decodes: "abcd", "t" -> 04 61 62 63 64 01 74, length 7.
    EXPECT_TRUE(std::holds_alternative<TrackRequest>(
        decode_q(bytes({0x07, 0x04, 0x61, 0x62, 0x63, 0x64, 0x01, 0x74}), limits)));
    limits.max_message_length = 2;
    EXPECT_EQ(error_code(decode_i(kInfoWire, limits)), DecodeErrorCode::LengthExceedsLimit);
}

TEST(Moqlite06Track, ErrorsLeaveTheCursorUnmoved) {
    const std::vector<Bytes> cases = {bytes({0x04, 0x00, 0x00, 0x00, 0x00}), bytes({0x02, 0x00, 0x00}), bytes({0x00})};
    for (const auto& wire : cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_track_info(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
}

TEST(Moqlite06Track, ConsecutiveMessagesAdvanceTheCursor) {
    Bytes wire = kInfoWire;
    wire.insert(wire.end(), kInfoMaxWire.begin(), kInfoMaxWire.end());
    Cursor input(wire);
    const auto a = decode_track_info(input);
    const auto b = decode_track_info(input);
    ASSERT_TRUE(std::holds_alternative<TrackInfo>(a));
    ASSERT_TRUE(std::holds_alternative<TrackInfo>(b));
    EXPECT_EQ(std::get<TrackInfo>(b).publisher_priority, 255);
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06Track, MaximumOffsetCursorReturnsAnErrorWithoutThrowing) {
    const Bytes wire = kInfoWire;
    Cursor input(wire, std::numeric_limits<std::size_t>::max());
    EXPECT_NO_THROW({
        const auto result = decode_track_info(input);
        EXPECT_FALSE(std::holds_alternative<TrackInfo>(result));
    });
    Cursor request_input(kTrackWire, std::numeric_limits<std::size_t>::max());
    EXPECT_NO_THROW({
        const auto result = decode_track_request(request_input);
        EXPECT_FALSE(std::holds_alternative<TrackRequest>(result));
    });
}

TEST(Moqlite06Track, RoundTrips) {
    const std::vector<TrackRequest> requests = {{"", ""}, {"room/cam", "video"}, {std::string(300, 'p'), "t"}};
    for (const auto& r : requests) {
        const auto result = decode_q(encode(r));
        ASSERT_TRUE(std::holds_alternative<TrackRequest>(result));
        EXPECT_EQ(std::get<TrackRequest>(result), r);
    }
    const std::vector<TrackInfo> infos = {{0, 0, 0}, {80, 9007199254740991ULL, 48000}, {255, kMaxVarint, kMaxVarint}};
    for (const auto& i : infos) {
        const auto result = decode_i(encode(i));
        ASSERT_TRUE(std::holds_alternative<TrackInfo>(result));
        EXPECT_EQ(std::get<TrackInfo>(result), i);
    }
}

TEST(Moqlite06Track, EncodersRefuseBadValuesAndWriteNothing) {
    ByteWriter out(256);
    EXPECT_EQ(encode_track_info(TrackInfo{1, kMaxVarint + 1, 1}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_track_info(TrackInfo{1, 1, kMaxVarint + 1}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    DecodeLimits limits;
    limits.max_string_length = 3;
    EXPECT_EQ(encode_track_request(TrackRequest{"abcd", "t"}, out, limits), std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(encode_track_request(TrackRequest{"a", "abcd"}, out, limits), std::optional<EncodeError>(EncodeError::LimitExceeded));
    ByteWriter tiny(3);
    EXPECT_EQ(encode_track_info(TrackInfo{60, 30000, 90000}, tiny), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_EQ(tiny.size(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
