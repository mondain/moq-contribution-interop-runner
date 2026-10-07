#include "moq/interop/wire/moqlite06/group.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

Bytes text(const char* s) {
    Bytes result;
    for (; *s != '\0'; ++s) result.push_back(static_cast<std::byte>(*s));
    return result;
}

Bytes concat(Bytes head, const Bytes& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

template <class T>
DecodeErrorCode error_code(const DecodeResult<T>& result) {
    const auto* error = std::get_if<DecodeError>(&result);
    EXPECT_NE(error, nullptr);
    return error ? error->code : DecodeErrorCode::InvalidValue;
}

Bytes encode(const GroupHeader& header) {
    ByteWriter out(1u << 16);
    EXPECT_FALSE(encode_group_header(header, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

Bytes encode(const Frame& frame) {
    ByteWriter out(1u << 16);
    EXPECT_FALSE(encode_frame(frame, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t kUint64Max = std::numeric_limits<std::uint64_t>::max();

// ---- zigzag ----

TEST(Moqlite06Zigzag, TableOfKnownMappings) {
    struct Row {
        std::int64_t value;
        std::uint64_t zigzag;
    };
    const Row rows[] = {
        {0, 0},
        {-1, 1},
        {1, 2},
        {-2, 3},
        {2, 4},
        // INT32_MAX = 2^31-1 -> 2^32-2; INT32_MIN = -2^31 -> 2^32-1.
        {std::numeric_limits<std::int32_t>::max(), 0xFFFFFFFEull},
        {std::numeric_limits<std::int32_t>::min(), 0xFFFFFFFFull},
        // int64 min -> all ones; int64 max -> all ones minus one.
        {kInt64Min, kUint64Max},
        {kInt64Max, kUint64Max - 1},
        // The varint boundary: 2^61-1 -> 2^62-2 and -2^61 -> 2^62-1 (kMaxVarint).
        {kMaxFrameDelta, kMaxVarint - 1},
        {kMinFrameDelta, kMaxVarint},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.value);
        EXPECT_EQ(zigzag_encode(row.value), row.zigzag);
        EXPECT_EQ(zigzag_decode(row.zigzag), row.value);
    }
}

// Evaluated at compile time: a signed shift or negation overflow would make these non-constant expressions.
static_assert(zigzag_encode(kInt64Min) == kUint64Max);
static_assert(zigzag_encode(kInt64Max) == kUint64Max - 1);
static_assert(zigzag_decode(kUint64Max) == kInt64Min);
static_assert(zigzag_decode(kUint64Max - 1) == kInt64Max);
// kMaxVarint is the largest zigzag a decoder can ever see, and it maps to exactly -2^61, so a decoded delta is
// always within [kMinFrameDelta, kMaxFrameDelta].
static_assert(zigzag_decode(kMaxVarint) == kMinFrameDelta);
static_assert(zigzag_decode(kMaxVarint - 1) == kMaxFrameDelta);

TEST(Moqlite06Zigzag, RoundTripsFixedAndRandomValues) {
    const std::int64_t fixed[] = {0, 1, -1, 2, -2, 63, -64, 64, -65, 1000000, -1000000, kInt64Min, kInt64Max,
                                  kInt64Min + 1, kInt64Max - 1, kMaxFrameDelta, kMinFrameDelta};
    for (const auto value : fixed) EXPECT_EQ(zigzag_decode(zigzag_encode(value)), value);

    std::mt19937_64 rng(0x1b5);
    for (int round = 0; round < 10000; ++round) {
        const auto value = static_cast<std::int64_t>(rng());
        ASSERT_EQ(zigzag_decode(zigzag_encode(value)), value);
        const auto raw = rng();
        ASSERT_EQ(zigzag_encode(zigzag_decode(raw)), raw);
    }
}

// ---- GROUP ----

struct GroupVector {
    const char* name;
    Bytes wire;
    GroupHeader header;
};

const std::vector<GroupVector>& group_vectors() {
    static const std::vector<GroupVector> table = {
        // Subscribe 1, group 7, frame start 0: body 01 07 00 is 3 bytes, Message Length 03.
        {"frame_start_0", bytes({0x03, 0x01, 0x07, 0x00}), GroupHeader{1, 7, 0}},
        {"frame_start_3", bytes({0x03, 0x01, 0x07, 0x03}), GroupHeader{1, 7, 3}},
        // Subscribe 64 = 0x0040 -> 40 40; group 300 = 0x012c -> 41 2c; frame start 16384 = 0x4000 is the 4-byte
        // form 80 00 40 00. Body = 2 + 2 + 4 = 8, Message Length 08.
        {"multibyte", bytes({0x08, 0x40, 0x40, 0x41, 0x2c, 0x80, 0x00, 0x40, 0x00}), GroupHeader{64, 300, 16384}},
        // Three kMaxVarint values: each is the 8-byte form ff ff ff ff ff ff ff ff (0xc0 | 0x3f, then 7 x ff).
        // Body = 24 = 0x18.
        {"all_max",
         concat(bytes({0x18}),
                concat(bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}),
                       concat(bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}),
                              bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})))),
         GroupHeader{kMaxVarint, kMaxVarint, kMaxVarint}},
    };
    return table;
}

TEST(Moqlite06Group, VectorsDecodeAndEncode) {
    for (const auto& v : group_vectors()) {
        SCOPED_TRACE(v.name);
        Cursor input(v.wire);
        const auto result = decode_group_header(input);
        ASSERT_TRUE(std::holds_alternative<GroupHeader>(result));
        EXPECT_EQ(std::get<GroupHeader>(result), v.header);
        EXPECT_EQ(input.remaining(), 0u);
        EXPECT_EQ(encode(v.header), v.wire);
    }
}

TEST(Moqlite06Group, EveryPrefixOfEveryVectorIsNeedMore) {
    for (const auto& v : group_vectors()) {
        for (std::size_t cut = 0; cut < v.wire.size(); ++cut) {
            SCOPED_TRACE(std::string(v.name) + " cut " + std::to_string(cut));
            const Bytes prefix(v.wire.begin(), v.wire.begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            const auto result = decode_group_header(input);
            ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
            EXPECT_GT(std::get<NeedMore>(result).required, std::get<NeedMore>(result).available);
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
}

TEST(Moqlite06Group, BodyViolationsAreProtocolViolationAndLeaveTheCursor) {
    const std::vector<Bytes> cases = {
        bytes({0x00}),                          // Message Length 0: no fields
        bytes({0x02, 0x01, 0x07}),              // frame start missing
        bytes({0x01, 0x01}),                    // group sequence and frame start missing
        bytes({0x04, 0x01, 0x07, 0x00, 0x00}),  // one trailing byte after the three fields
        bytes({0x03, 0x01, 0x07, 0x40}),        // 2-byte varint cut off by the Message Length
    };
    for (const auto& wire : cases) {
        Cursor input(wire);
        EXPECT_EQ(error_code(decode_group_header(input)), DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.remaining(), wire.size());
    }
}

TEST(Moqlite06Group, MessageLengthOverLimitAndNonMinimalVarints) {
    DecodeLimits limits;
    limits.max_message_length = 2;
    const auto wire = bytes({0x03, 0x01, 0x07, 0x00});
    Cursor input(wire);
    EXPECT_EQ(error_code(decode_group_header(input, limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(input.remaining(), wire.size());

    // A non-minimal Subscribe ID (40 01 is 1 in two bytes) is accepted: body 40 01 07 00 = 4, length 04.
    const auto loose = bytes({0x04, 0x40, 0x01, 0x07, 0x00});
    Cursor loose_input(loose);
    const auto result = decode_group_header(loose_input);
    ASSERT_TRUE(std::holds_alternative<GroupHeader>(result));
    EXPECT_EQ(std::get<GroupHeader>(result), (GroupHeader{1, 7, 0}));
}

TEST(Moqlite06Group, EncoderRefusals) {
    ByteWriter out(256);
    constexpr auto big = kMaxVarint + 1;
    EXPECT_EQ(encode_group_header(GroupHeader{big, 0, 0}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_group_header(GroupHeader{0, big, 0}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_group_header(GroupHeader{0, 0, big}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(out.size(), 0u);

    // The body 01 07 00 is 3 bytes, over a max_message_length of 2.
    DecodeLimits limits;
    limits.max_message_length = 2;
    EXPECT_EQ(encode_group_header(GroupHeader{1, 7, 0}, out, limits),
              std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(out.size(), 0u);

    // Whole message is 4 bytes; a writer of 3 gets nothing, a writer of 4 gets all of it.
    ByteWriter small(3);
    EXPECT_EQ(encode_group_header(GroupHeader{1, 7, 0}, small), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(small.size(), 0u);
    ByteWriter exact(4);
    EXPECT_FALSE(encode_group_header(GroupHeader{1, 7, 0}, exact).has_value());
    EXPECT_EQ(exact.size(), 4u);
}

// ---- FRAME ----

struct FrameVector {
    std::string name;
    Bytes wire;
    Frame frame;
};

Bytes repeat(unsigned value, std::size_t count) { return Bytes(count, static_cast<std::byte>(value)); }

const std::vector<FrameVector>& frame_vectors() {
    static const std::vector<FrameVector> table = {
        // Delta 0 -> zigzag 0 -> 00; payload "abc" is 3 bytes -> Message Length 03; then 61 62 63.
        {"delta_0", bytes({0x00, 0x03, 0x61, 0x62, 0x63}), Frame{0, text("abc")}},
        // Delta 1 -> zigzag (1 << 1) ^ 0 = 2 -> 02.
        {"delta_1", bytes({0x02, 0x03, 0x61, 0x62, 0x63}), Frame{1, text("abc")}},
        // Delta -1 -> zigzag (-1 << 1) ^ -1 = ...fe ^ ...ff = 1 -> 01.
        {"delta_minus_1", bytes({0x01, 0x03, 0x61, 0x62, 0x63}), Frame{-1, text("abc")}},
        // Delta 33 -> zigzag 66 = 0x42, which is over 63, so the 2-byte varint 0x4000 | 0x0042 = 40 42.
        {"delta_33_two_byte_varint", bytes({0x40, 0x42, 0x03, 0x61, 0x62, 0x63}), Frame{33, text("abc")}},
        // Delta -33 -> zigzag 2*33 - 1 = 65 = 0x41 -> 40 41; empty payload -> Message Length 00.
        {"delta_minus_33_empty", bytes({0x40, 0x41, 0x00}), Frame{-33, {}}},
        // Empty payload, delta 0: just 00 00 (a gap in the group, draft 6.3.2).
        {"empty_payload", bytes({0x00, 0x00}), Frame{0, {}}},
        // First frame of a group: delta from 0 is the absolute timestamp. 1000 -> zigzag 2000 = 0x07d0 ->
        // 2-byte varint 0x4000 | 0x07d0 = 0x47d0 -> 47 d0; payload "a" -> 01 61.
        {"first_frame_absolute_1000", bytes({0x47, 0xd0, 0x01, 0x61}), Frame{1000, text("a")}},
        // Largest delta 2^61-1 -> zigzag 2^62-2 = 0x3ffffffffffffffe -> 8-byte form 0xc0|0x3f = ff, then
        // ff ff ff ff ff ff fe; empty payload 00.
        {"max_positive_delta", bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0x00}), Frame{kMaxFrameDelta, {}}},
        // Most negative delta -2^61 -> zigzag 2^62-1 = kMaxVarint -> ff x 8; empty payload 00.
        {"min_negative_delta", bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00}), Frame{kMinFrameDelta, {}}},
        // A 64-byte payload has a 2-byte Message Length: 64 = 0x0040 -> 40 40.
        {"payload_64_two_byte_length", concat(bytes({0x00, 0x40, 0x40}), repeat(0xaa, 64)), Frame{0, repeat(0xaa, 64)}},
    };
    return table;
}

TEST(Moqlite06Frame, VectorsDecodeAndEncode) {
    for (const auto& v : frame_vectors()) {
        SCOPED_TRACE(v.name);
        Cursor input(v.wire);
        const auto result = decode_frame(input);
        ASSERT_TRUE(std::holds_alternative<Frame>(result));
        EXPECT_EQ(std::get<Frame>(result), v.frame);
        EXPECT_EQ(input.remaining(), 0u);
        EXPECT_EQ(encode(v.frame), v.wire);
    }
}

// A streaming reader waits on NeedMore: every strict prefix of every valid frame is NeedMore (never an error),
// the cursor stays put, and `required` is more than what was available.
TEST(Moqlite06Frame, EveryPrefixOfEveryVectorIsNeedMore) {
    for (const auto& v : frame_vectors()) {
        for (std::size_t cut = 0; cut < v.wire.size(); ++cut) {
            SCOPED_TRACE(v.name + " cut " + std::to_string(cut));
            const Bytes prefix(v.wire.begin(), v.wire.begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            const auto result = decode_frame(input);
            ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
            const auto& need = std::get<NeedMore>(result);
            EXPECT_GT(need.required, need.available);
            EXPECT_EQ(input.remaining(), prefix.size());
        }
        // The complete vector succeeds, so the loop above is the only thing separating NeedMore from success.
        Cursor whole(v.wire);
        EXPECT_TRUE(std::holds_alternative<Frame>(decode_frame(whole)));
    }
}

TEST(Moqlite06Frame, TwoFramesBackToBackLeaveTheCursorAtTheSecond) {
    // First: delta 0, "abc" (00 03 61 62 63). Second: delta 1, "x" (02 01 78).
    const auto wire = bytes({0x00, 0x03, 0x61, 0x62, 0x63, 0x02, 0x01, 0x78});
    Cursor input(wire);
    const auto first = decode_frame(input);
    ASSERT_TRUE(std::holds_alternative<Frame>(first));
    EXPECT_EQ(std::get<Frame>(first), (Frame{0, text("abc")}));
    EXPECT_EQ(input.remaining(), 3u);
    const auto second = decode_frame(input);
    ASSERT_TRUE(std::holds_alternative<Frame>(second));
    EXPECT_EQ(std::get<Frame>(second), (Frame{1, text("x")}));
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06Frame, GroupHeaderThenFramesDecodeFromOneBuffer) {
    const auto wire = bytes({0x03, 0x01, 0x07, 0x00, 0x00, 0x01, 0x61, 0x01, 0x00});
    Cursor input(wire);
    ASSERT_TRUE(std::holds_alternative<GroupHeader>(decode_group_header(input)));
    const auto a = decode_frame(input);
    const auto b = decode_frame(input);
    ASSERT_TRUE(std::holds_alternative<Frame>(a));
    ASSERT_TRUE(std::holds_alternative<Frame>(b));
    EXPECT_EQ(std::get<Frame>(a), (Frame{0, text("a")}));
    EXPECT_EQ(std::get<Frame>(b), (Frame{-1, {}}));
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06Frame, NonMinimalVarintsAreAccepted) {
    // Delta 0 spelled as the 2-byte varint 40 00, payload length 1 spelled 40 01.
    const auto wire = bytes({0x40, 0x00, 0x40, 0x01, 0x7a});
    Cursor input(wire);
    const auto result = decode_frame(input);
    ASSERT_TRUE(std::holds_alternative<Frame>(result));
    EXPECT_EQ(std::get<Frame>(result), (Frame{0, text("z")}));
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06Frame, HugePayloadLengthIsRefusedBeforeAnyAllocation) {
    // Delta 00, then Message Length 2^40 = 0x0000010000000000 as an 8-byte varint: 0xc0 | 0x00 first byte, then
    // 00 01 00 00 00 00 00. Only 9 bytes are supplied, so any attempt to read the payload would be NeedMore.
    const auto wire = bytes({0x00, 0xc0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});
    Cursor input(wire);
    EXPECT_EQ(error_code(decode_frame(input)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(input.remaining(), wire.size());

    // Just over a small limit is refused the same way; exactly at it decodes.
    DecodeLimits limits;
    limits.max_message_length = 2;
    const auto over = bytes({0x00, 0x03, 0x61, 0x62, 0x63});
    Cursor over_input(over);
    EXPECT_EQ(error_code(decode_frame(over_input, limits)), DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(over_input.remaining(), over.size());
    // The payload length alone decides it, even when the payload bytes have not arrived yet.
    const auto only_length = bytes({0x00, 0x03});
    Cursor length_input(only_length);
    EXPECT_EQ(error_code(decode_frame(length_input, limits)), DecodeErrorCode::LengthExceedsLimit);
    const auto at = bytes({0x00, 0x02, 0x61, 0x62});
    Cursor at_input(at);
    EXPECT_TRUE(std::holds_alternative<Frame>(decode_frame(at_input, limits)));
}

TEST(Moqlite06Frame, EncoderRefusesZigzagBeyondVarintAndWritesNothing) {
    ByteWriter out(64);
    // 2^61 -> zigzag 2^62 (one past kMaxVarint); -(2^61)-1 -> zigzag 2^62+1; and the int64 extremes.
    const std::int64_t beyond_max = kMaxFrameDelta + 1;
    const std::int64_t beyond_min = kMinFrameDelta - 1;
    EXPECT_EQ(zigzag_encode(beyond_max), kMaxVarint + 1);
    EXPECT_EQ(zigzag_encode(beyond_min), kMaxVarint + 2);
    for (const auto delta : {beyond_max, beyond_min, kInt64Max, kInt64Min}) {
        EXPECT_EQ(encode_frame(Frame{delta, text("abc")}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    }
    EXPECT_EQ(out.size(), 0u);
    // The boundary values themselves encode.
    EXPECT_FALSE(encode_frame(Frame{kMaxFrameDelta, {}}, out).has_value());
    EXPECT_FALSE(encode_frame(Frame{kMinFrameDelta, {}}, out).has_value());
    EXPECT_EQ(out.size(), 18u);
}

TEST(Moqlite06Frame, EncoderLimitAndCapacityRefusalsWriteNothing) {
    DecodeLimits limits;
    limits.max_message_length = 2;
    ByteWriter out(64);
    EXPECT_EQ(encode_frame(Frame{0, text("abc")}, out, limits), std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_FALSE(encode_frame(Frame{0, text("ab")}, out, limits).has_value());
    EXPECT_EQ(out.size(), 4u);

    // 00 03 61 62 63 is 5 bytes: a writer with 4 gets nothing, with 5 gets everything.
    ByteWriter small(4);
    EXPECT_EQ(encode_frame(Frame{0, text("abc")}, small), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(small.size(), 0u);
    ByteWriter exact(5);
    EXPECT_FALSE(encode_frame(Frame{0, text("abc")}, exact).has_value());
    EXPECT_EQ(exact.size(), 5u);
}

TEST(Moqlite06Frame, RoundTripsRandomFrames) {
    std::mt19937_64 rng(0x1b5f);
    for (int round = 0; round < 2000; ++round) {
        Frame frame;
        const auto raw = static_cast<std::int64_t>(rng());
        // Keep the low 62 bits of the zigzag so the delta lies in the encodable range [-2^61, 2^61-1].
        frame.timestamp_delta = zigzag_decode(zigzag_encode(raw) & kMaxVarint);
        frame.payload.resize(rng() % 200);
        for (auto& b : frame.payload) b = static_cast<std::byte>(rng());
        const auto wire = encode(frame);
        Cursor input(wire);
        const auto result = decode_frame(input);
        ASSERT_TRUE(std::holds_alternative<Frame>(result));
        EXPECT_EQ(std::get<Frame>(result), frame);
        EXPECT_EQ(input.remaining(), 0u);
    }
}

// ---- robustness ----

TEST(Moqlite06GroupFrame, RandomBytesNeverCrash) {
    std::mt19937 rng(0x1b6);
    for (int round = 0; round < 20000; ++round) {
        Bytes data(rng() % 24);
        for (auto& b : data) b = static_cast<std::byte>(rng());
        if (!data.empty() && (rng() % 2 == 0)) data[0] = static_cast<std::byte>(rng() % 6);
        const auto before = data.size();

        Cursor group(data);
        const auto group_result = decode_group_header(group);
        EXPECT_EQ(std::holds_alternative<GroupHeader>(group_result), group.remaining() < before);

        Cursor frame(data);
        const auto frame_result = decode_frame(frame);
        EXPECT_EQ(std::holds_alternative<Frame>(frame_result), frame.remaining() < before);
    }
}

// ---- cursors whose absolute offset is near SIZE_MAX ----

// read_varint can pass through OffsetOverflow; every decoder must return it (never throw) and leave the cursor.
template <class Decode>
void expect_overflow_is_an_error(Decode decode) {
    const Bytes data = bytes({0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    {
        Cursor cursor(data, std::numeric_limits<std::size_t>::max());
        const auto result = decode(cursor);
        const auto* error = std::get_if<DecodeError>(&result);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(error->code, DecodeErrorCode::OffsetOverflow);
        EXPECT_EQ(cursor.remaining(), data.size());
    }
    {
        Cursor cursor(data, std::numeric_limits<std::size_t>::max() - 1);
        const auto result = decode(cursor);
        if (!std::holds_alternative<DecodeError>(result) && !std::holds_alternative<NeedMore>(result)) return;
        EXPECT_EQ(cursor.remaining(), data.size());
    }
}

TEST(Moqlite06Overflow, EveryDecoderReturnsAnErrorNearSizeMax) {
    expect_overflow_is_an_error([](Cursor& c) { return decode_setup(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_announce_request(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_announce_ok(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_announce_message(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_subscribe(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_subscribe_update(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_subscribe_response(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_group_header(c); });
    expect_overflow_is_an_error([](Cursor& c) { return decode_frame(c); });
    expect_overflow_is_an_error([](Cursor& c) { return read_stream_type(c); });
    expect_overflow_is_an_error([](Cursor& c) { return read_varint(c); });
    expect_overflow_is_an_error([](Cursor& c) { return read_string(c, 16); });
    expect_overflow_is_an_error([](Cursor& c) { return read_framed_body(c); });
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
