#include "moq/interop/wire/moqlite06/subscribe.h"

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

Bytes encode(const Subscribe& m) { return encode_with(m, encode_subscribe); }
Bytes encode(const SubscribeUpdate& m) { return encode_with(m, encode_subscribe_update); }
Bytes encode(const SubscribeResponse& m) { return encode_with(m, encode_subscribe_response); }

DecodeResult<Subscribe> decode_s(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_subscribe(input, limits);
}
DecodeResult<SubscribeUpdate> decode_u(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_subscribe_update(input, limits);
}
DecodeResult<SubscribeResponse> decode_r(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_subscribe_response(input, limits);
}

// One table drives the vector, round-trip and every-prefix tests, so no valid vector escapes the prefix loop.
enum class Kind { Subscribe, Update, Response };

struct Vector {
    const char* name;
    Kind kind;
    Bytes wire;
    Subscribe subscribe;
    SubscribeUpdate update;
    SubscribeResponse response;
};

Vector sub(const char* name, Bytes wire, Subscribe value) {
    return Vector{name, Kind::Subscribe, std::move(wire), std::move(value), {}, SubscribeOk{}};
}
Vector upd(const char* name, Bytes wire, SubscribeUpdate value) {
    return Vector{name, Kind::Update, std::move(wire), {}, std::move(value), SubscribeOk{}};
}
Vector resp(const char* name, Bytes wire, SubscribeResponse value) {
    return Vector{name, Kind::Response, std::move(wire), {}, {}, std::move(value)};
}

const std::vector<Vector>& vectors() {
    static const std::vector<Vector> table = {
        // SUBSCRIBE id 0, path "b", track "t", priority 0x80, everything else 0.
        // Body = 00 (id) + 01 62 (path) + 01 74 (track) + 80 (priority, one raw byte) + 5 range varints of 00
        // = 1 + 2 + 2 + 1 + 5 = 11, so Message Length 0b.
        sub("subscribe_minimal", bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}),
            Subscribe{0, "b", "t", SubscribeRange{0x80, 0, 0, 0, 0, 0}}),
        // Priority 0xff is one byte; read as a varint prefix it would claim 8 bytes. Same layout as above.
        sub("subscribe_priority_ff", bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00}),
            Subscribe{0, "b", "t", SubscribeRange{0xff, 0, 0, 0, 0, 0}}),
        // Priority 0x00: same layout, priority byte 00.
        sub("subscribe_priority_00", bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
            Subscribe{0, "b", "t", SubscribeRange{0x00, 0, 0, 0, 0, 0}}),
        // Every field non-zero, with 2-byte varints: id 1; path "ab" = 02 61 62; track "c" = 01 63; priority 7f;
        // max age 300 = 0x012c -> 0x40|0x01 = 41, 2c; group start 5 = 05; group end 1024 = 0x0400 -> 44 00;
        // frame start 2 = 02; frame end 3 = 03. Body = 1 + 3 + 2 + 1 + 2 + 1 + 2 + 1 + 1 = 14, Message Length 0e.
        sub("subscribe_all_fields", bytes({0x0e, 0x01, 0x02, 0x61, 0x62, 0x01, 0x63, 0x7f, 0x41, 0x2c, 0x05, 0x44, 0x00, 0x02, 0x03}),
            Subscribe{1, "ab", "c", SubscribeRange{0x7f, 300, 5, 1024, 2, 3}}),
        // SUBSCRIBE_UPDATE is only the range. Priority 10, five varints of 00: body 6 bytes, Message Length 06.
        upd("update_minimal", bytes({0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00}),
            SubscribeUpdate{SubscribeRange{0x10, 0, 0, 0, 0, 0}}),
        // Priority ff; max age 64 -> 0x40|0x00, 0x40 = 40 40; group start 1; group end 2; frame start 3; frame end 4.
        // Body = 1 + 2 + 1 + 1 + 1 + 1 = 7, Message Length 07.
        upd("update_all_fields", bytes({0x07, 0xff, 0x40, 0x40, 0x01, 0x02, 0x03, 0x04}),
            SubscribeUpdate{SubscribeRange{0xff, 64, 1, 2, 3, 4}}),
        // SUBSCRIBE_OK group 6: Type 00, body 06, Message Length 01.
        resp("ok", bytes({0x00, 0x01, 0x06}), SubscribeOk{6}),
        // SUBSCRIBE_END group 0: Type 01, body 00, Message Length 01.
        resp("end_zero", bytes({0x01, 0x01, 0x00}), SubscribeEnd{0}),
        // SUBSCRIBE_DROP (3, 5, error 0): Type 02, body 03 05 00, Message Length 03.
        resp("drop", bytes({0x02, 0x03, 0x03, 0x05, 0x00}), SubscribeDrop{3, 5, 0}),
        // Multi-byte varints: start 256 = 0x0100 -> 41 00; end 300 = 0x012c -> 41 2c; error 16384 = 0x4000 is the
        // 4-byte form 0x80|0x00, 00, 40, 00. Type 02, body = 2 + 2 + 4 = 8, Message Length 08.
        resp("drop_multibyte", bytes({0x02, 0x08, 0x41, 0x00, 0x41, 0x2c, 0x80, 0x00, 0x40, 0x00}),
             SubscribeDrop{256, 300, 16384}),
    };
    return table;
}

TEST(Moqlite06Subscribe, VectorsDecodeAndEncode) {
    for (const auto& v : vectors()) {
        SCOPED_TRACE(v.name);
        Cursor input(v.wire);
        switch (v.kind) {
            case Kind::Subscribe: {
                const auto result = decode_subscribe(input);
                ASSERT_TRUE(std::holds_alternative<Subscribe>(result));
                EXPECT_EQ(std::get<Subscribe>(result), v.subscribe);
                EXPECT_EQ(encode(v.subscribe), v.wire);
                break;
            }
            case Kind::Update: {
                const auto result = decode_subscribe_update(input);
                ASSERT_TRUE(std::holds_alternative<SubscribeUpdate>(result));
                EXPECT_EQ(std::get<SubscribeUpdate>(result), v.update);
                EXPECT_EQ(encode(v.update), v.wire);
                break;
            }
            case Kind::Response: {
                const auto result = decode_subscribe_response(input);
                ASSERT_TRUE(std::holds_alternative<SubscribeResponse>(result));
                EXPECT_EQ(std::get<SubscribeResponse>(result), v.response);
                EXPECT_EQ(encode(v.response), v.wire);
                break;
            }
        }
        EXPECT_EQ(input.remaining(), 0u);
    }
}

// Every strict prefix of every valid vector is short input: NeedMore, with the cursor untouched.
TEST(Moqlite06Subscribe, EveryPrefixOfEveryVectorIsNeedMore) {
    for (const auto& v : vectors()) {
        for (std::size_t cut = 0; cut < v.wire.size(); ++cut) {
            SCOPED_TRACE(std::string(v.name) + " cut " + std::to_string(cut));
            const Bytes prefix(v.wire.begin(), v.wire.begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            bool need_more = false;
            switch (v.kind) {
                case Kind::Subscribe: need_more = std::holds_alternative<NeedMore>(decode_subscribe(input)); break;
                case Kind::Update: need_more = std::holds_alternative<NeedMore>(decode_subscribe_update(input)); break;
                case Kind::Response:
                    need_more = std::holds_alternative<NeedMore>(decode_subscribe_response(input));
                    break;
            }
            EXPECT_TRUE(need_more);
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
}

TEST(Moqlite06Subscribe, ConsecutiveMessagesAdvanceTheCursor) {
    Bytes wire = bytes({0x00, 0x01, 0x06, 0x01, 0x01, 0x09});
    Cursor input(wire);
    const auto a = decode_subscribe_response(input);
    const auto b = decode_subscribe_response(input);
    ASSERT_TRUE(std::holds_alternative<SubscribeResponse>(a));
    ASSERT_TRUE(std::holds_alternative<SubscribeResponse>(b));
    EXPECT_EQ(std::get<SubscribeResponse>(a), (SubscribeResponse{SubscribeOk{6}}));
    EXPECT_EQ(std::get<SubscribeResponse>(b), (SubscribeResponse{SubscribeEnd{9}}));
    EXPECT_EQ(input.remaining(), 0u);
}

// The dispatcher reads Type first. Type 0 is SUBSCRIBE_OK here, even though ANNOUNCE_START also uses Type 0:
// the two are decoded by different functions because they arrive on different streams (Subscribe Stream vs
// Announce Stream), so the numeric overlap never has to be disambiguated by content.
TEST(Moqlite06Subscribe, DispatcherPicksAlternativeAndRejectsUnknownType) {
    const auto ok = decode_r(bytes({0x00, 0x01, 0x06}));
    ASSERT_TRUE(std::holds_alternative<SubscribeResponse>(ok));
    EXPECT_TRUE(std::holds_alternative<SubscribeOk>(std::get<SubscribeResponse>(ok)));
    const auto end = decode_r(bytes({0x01, 0x01, 0x00}));
    ASSERT_TRUE(std::holds_alternative<SubscribeResponse>(end));
    EXPECT_TRUE(std::holds_alternative<SubscribeEnd>(std::get<SubscribeResponse>(end)));
    const auto drop = decode_r(bytes({0x02, 0x03, 0x03, 0x05, 0x00}));
    ASSERT_TRUE(std::holds_alternative<SubscribeResponse>(drop));
    EXPECT_TRUE(std::holds_alternative<SubscribeDrop>(std::get<SubscribeResponse>(drop)));

    // Type 3 is unknown even though a well-formed length-prefixed body follows.
    const auto wire = bytes({0x03, 0x01, 0x00});
    Cursor input(wire);
    EXPECT_EQ(error_code(decode_subscribe_response(input)), DecodeErrorCode::InvalidValue);
    EXPECT_EQ(input.remaining(), 3u);
}

TEST(Moqlite06Subscribe, RoundTrips) {
    const std::vector<Subscribe> subs = {
        Subscribe{0x3fffffffffffffffULL, "room/cam", "video", SubscribeRange{3, 1, 2, 9, 4, 5}},
        Subscribe{9, "", "", SubscribeRange{}},
        Subscribe{70000, "p", "q", SubscribeRange{0xff, 0x3fffffffffffffffULL, 0x3fffffffffffffffULL,
                                                  0x3fffffffffffffffULL, 0x3fffffffffffffffULL,
                                                  0x3fffffffffffffffULL}},
    };
    for (const auto& s : subs) {
        const auto result = decode_s(encode(s));
        ASSERT_TRUE(std::holds_alternative<Subscribe>(result));
        EXPECT_EQ(std::get<Subscribe>(result), s);
    }
    const SubscribeUpdate update{SubscribeRange{0x42, 15000, 100, 0, 0, 0}};
    const auto ur = decode_u(encode(update));
    ASSERT_TRUE(std::holds_alternative<SubscribeUpdate>(ur));
    EXPECT_EQ(std::get<SubscribeUpdate>(ur), update);
    const std::vector<SubscribeResponse> responses = {SubscribeOk{0x3fffffffffffffffULL}, SubscribeEnd{70000},
                                                      SubscribeDrop{1, 1, 0x3fffffffffffffffULL}};
    for (const auto& r : responses) {
        const auto result = decode_r(encode(r));
        ASSERT_TRUE(std::holds_alternative<SubscribeResponse>(result));
        EXPECT_EQ(std::get<SubscribeResponse>(result), r);
    }
}

// Frame End 3 with Group End 0 (draft 7.9: MUST be 0 when Group End is 0). Body = 00 01 62 01 74 (id, "b", "t")
// + 00 (priority) + 00 (age) 00 (group start) 00 (group end) 00 (frame start) 03 (frame end) = 11, length 0b.
TEST(Moqlite06Subscribe, FrameEndWithoutGroupEnd) {
    const auto wire = bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03});
    EXPECT_EQ(error_code(decode_s(wire)), DecodeErrorCode::ProtocolViolation);
    // The update form: priority 00, age 0, group start 0, group end 0, frame start 0, frame end 3; length 06.
    EXPECT_EQ(error_code(decode_u(bytes({0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}))),
              DecodeErrorCode::ProtocolViolation);

    ByteWriter out(64);
    const Subscribe bad{0, "b", "t", SubscribeRange{0, 0, 0, 0, 0, 3}};
    EXPECT_EQ(encode_subscribe(bad, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_update(SubscribeUpdate{bad.range}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(out.size(), 0u);

    // Group End 4 with Frame End 3 is fine: same layout with group end 04 -> 00 01 62 01 74 00 | 00 00 04 00 03.
    const auto good = bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0x00, 0x00, 0x00, 0x04, 0x00, 0x03});
    const auto result = decode_s(good);
    ASSERT_TRUE(std::holds_alternative<Subscribe>(result));
    EXPECT_EQ(std::get<Subscribe>(result).range, (SubscribeRange{0, 0, 0, 4, 0, 3}));
    EXPECT_EQ(encode(Subscribe{0, "b", "t", SubscribeRange{0, 0, 0, 4, 0, 3}}), good);
}

TEST(Moqlite06Subscribe, BodyLengthViolations) {
    // Extra trailing byte: the 11-byte SUBSCRIBE body plus one stray 00, Message Length 0c.
    EXPECT_EQ(error_code(decode_s(bytes({0x0c, 0x00, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
    // One byte short: the Frame End is missing, body 10 bytes, Message Length 0a.
    EXPECT_EQ(error_code(decode_s(bytes({0x0a, 0x00, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
    // Message Length 0: no fields at all.
    EXPECT_EQ(error_code(decode_s(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_u(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);
    // Update: trailing byte (7 bytes, length 07) and one short (5 bytes, length 05).
    EXPECT_EQ(error_code(decode_u(bytes({0x07, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}))),
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_u(bytes({0x05, 0x10, 0x00, 0x00, 0x00, 0x00}))), DecodeErrorCode::ProtocolViolation);
    // A 2-byte varint cut off by the Message Length: priority 10, age first byte 41 claims 2 bytes, length 2.
    EXPECT_EQ(error_code(decode_u(bytes({0x02, 0x10, 0x41, 0x2c}))), DecodeErrorCode::ProtocolViolation);
    // Responses: OK with a trailing byte (length 2), OK with Message Length 0, DROP missing the Error Code.
    EXPECT_EQ(error_code(decode_r(bytes({0x00, 0x02, 0x06, 0x00}))), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_r(bytes({0x00, 0x00}))), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_r(bytes({0x01, 0x00}))), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(error_code(decode_r(bytes({0x02, 0x02, 0x03, 0x03}))), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Subscribe, LimitsAreEnforcedBeforeAllocation) {
    DecodeLimits limits;
    limits.max_string_length = 1;
    // Path "ab" is 2 bytes: body 00 02 61 62 and then enough filler; the string limit fires first.
    EXPECT_EQ(error_code(decode_s(bytes({0x0c, 0x00, 0x02, 0x61, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}),
                                  limits)),
              DecodeErrorCode::LengthExceedsLimit);
    // Track name over the limit too: id 0, path "b", track "tt".
    EXPECT_EQ(error_code(decode_s(bytes({0x0c, 0x00, 0x01, 0x62, 0x02, 0x74, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}),
                                  limits)),
              DecodeErrorCode::LengthExceedsLimit);
    // A string exactly at the limit decodes.
    EXPECT_TRUE(std::holds_alternative<Subscribe>(
        decode_s(bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}), limits)));
    // Message Length over the limit.
    limits.max_message_length = 2;
    EXPECT_EQ(error_code(decode_r(bytes({0x02, 0x03, 0x03, 0x05, 0x00}), limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(error_code(decode_u(bytes({0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00}), limits)),
              DecodeErrorCode::LengthExceedsLimit);
}

// A failed decode leaves the input cursor exactly where it was, for every error class.
TEST(Moqlite06Subscribe, ErrorsLeaveTheCursorUnmoved) {
    const std::vector<Bytes> subscribe_cases = {
        // Frame End 3 with Group End 0 (range coupling), then trailing byte, then one byte short.
        bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}),
        bytes({0x0c, 0x00, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x0a, 0x00, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x00}),
    };
    for (const auto& wire : subscribe_cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_subscribe(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
    const std::vector<Bytes> update_cases = {
        bytes({0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}),
        bytes({0x07, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x05, 0x10, 0x00, 0x00, 0x00, 0x00}),
        bytes({0x00}),
    };
    for (const auto& wire : update_cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_subscribe_update(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
    const std::vector<Bytes> response_cases = {
        bytes({0x00, 0x02, 0x06, 0x00}),
        bytes({0x00, 0x00}),
        bytes({0x02, 0x02, 0x03, 0x03}),
    };
    for (const auto& wire : response_cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_subscribe_response(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
}

// The string limit is judged from the length prefix alone: a 0x3f-byte path with the body cut off right after
// the prefix is LengthExceedsLimit, not a body shortfall (ProtocolViolation) or short input (NeedMore).
TEST(Moqlite06Subscribe, StringLimitIsCheckedBeforeTheBytesAreRead) {
    DecodeLimits limits;
    limits.max_string_length = 1;
    // Message Length 2, body = 00 (id) 3f (path length 63) and nothing more.
    const auto path_wire = bytes({0x02, 0x00, 0x3f});
    Cursor path_input(path_wire);
    EXPECT_EQ(error_code(decode_subscribe(path_input, limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(path_input.remaining(), path_wire.size());
    // Same for the track name: body = 00 (id) 01 62 ("b") 3f, Message Length 4.
    const auto track_wire = bytes({0x04, 0x00, 0x01, 0x62, 0x3f});
    Cursor track_input(track_wire);
    EXPECT_EQ(error_code(decode_subscribe(track_input, limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(track_input.remaining(), track_wire.size());
}

TEST(Moqlite06Subscribe, EncoderRefusals) {
    ByteWriter out(256);
    const SubscribeRange ok_range{};
    constexpr auto big = kMaxVarint + 1;
    EXPECT_EQ(encode_subscribe(Subscribe{big, "b", "t", ok_range}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe(Subscribe{0, "b", "t", SubscribeRange{0, big, 0, 0, 0, 0}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe(Subscribe{0, "b", "t", SubscribeRange{0, 0, big, 0, 0, 0}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe(Subscribe{0, "b", "t", SubscribeRange{0, 0, 0, big, 0, 0}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_update(SubscribeUpdate{SubscribeRange{0, 0, 0, 1, big, 0}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_update(SubscribeUpdate{SubscribeRange{0, 0, 0, big, 0, big}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_response(SubscribeOk{big}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_response(SubscribeEnd{big}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_response(SubscribeDrop{big, 0, 0}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_response(SubscribeDrop{0, big, 0}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_subscribe_response(SubscribeDrop{0, 0, big}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    // kMaxVarint itself is the largest legal value.
    EXPECT_FALSE(encode_subscribe_response(SubscribeOk{kMaxVarint}, out).has_value());
    const auto written = out.size();

    DecodeLimits limits;
    limits.max_string_length = 3;
    EXPECT_EQ(encode_subscribe(Subscribe{0, "abcd", "t", ok_range}, out, limits),
              std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(encode_subscribe(Subscribe{0, "a", "abcd", ok_range}, out, limits),
              std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_FALSE(encode_subscribe(Subscribe{0, "abc", "abc", ok_range}, out, limits).has_value());
    const auto after_ok = out.size();
    limits.max_message_length = 3;
    EXPECT_EQ(encode_subscribe(Subscribe{0, "a", "b", ok_range}, out, limits),
              std::optional<EncodeError>(EncodeError::LimitExceeded));

    EXPECT_GT(after_ok, written);
    EXPECT_EQ(out.size(), after_ok);  // nothing written by any refusal
}

TEST(Moqlite06Subscribe, EncoderWritesNothingWithoutRoom) {
    ByteWriter small(1);
    EXPECT_EQ(encode_subscribe_update(SubscribeUpdate{}, small), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(small.size(), 0u);
    ByteWriter none(0);
    EXPECT_EQ(encode_subscribe(Subscribe{}, none), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(none.size(), 0u);

    // SUBSCRIBE_OK group 6 is 00 01 06. A writer with room for Message Length and the body (2 bytes) but not
    // the Type byte in front of them must stay empty rather than emit a headless message.
    ByteWriter no_type_room(2);
    EXPECT_EQ(encode_subscribe_response(SubscribeOk{6}, no_type_room),
              std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(no_type_room.size(), 0u);
    ByteWriter exact(3);
    EXPECT_FALSE(encode_subscribe_response(SubscribeOk{6}, exact).has_value());
    EXPECT_EQ(exact.size(), 3u);
}

TEST(Moqlite06Subscribe, RandomBytesNeverCrash) {
    std::mt19937 rng(0x1b4);
    for (int round = 0; round < 20000; ++round) {
        Bytes data(rng() % 24);
        for (auto& b : data) b = static_cast<std::byte>(rng());
        if (!data.empty() && (rng() % 2 == 0)) data[0] = static_cast<std::byte>(rng() % 4);
        const auto before = data.size();

        Cursor s(data);
        const auto sr = decode_subscribe(s);
        EXPECT_EQ(std::holds_alternative<Subscribe>(sr), s.remaining() < before);
        Cursor u(data);
        const auto ur = decode_subscribe_update(u);
        EXPECT_EQ(std::holds_alternative<SubscribeUpdate>(ur), u.remaining() < before);
        Cursor r(data);
        const auto rr = decode_subscribe_response(r);
        EXPECT_EQ(std::holds_alternative<SubscribeResponse>(rr), r.remaining() < before);
    }
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
