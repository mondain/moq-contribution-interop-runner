#include "moq/interop/wire/moqlite06/probe.h"

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

Bytes encode(const ProbeMessage& m) {
    ByteWriter out(1u << 10);
    EXPECT_FALSE(encode_probe(m, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

DecodeResult<ProbeMessage> decode_p(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_probe(input, limits);
}

struct Vector {
    const char* name;
    Bytes wire;
    ProbeMessage value;
};

const std::vector<Vector>& vectors() {
    static const std::vector<Vector> table = {
        // Both unknown (0 means unknown): body 00 00, Message Length 2.
        {"unknown", bytes({0x02, 0x00, 0x00}), ProbeMessage{0, 0}},
        // 5 000 000 bps = 0x4c4b40 -> 4-byte form 80 4c 4b 40; RTT 25 ms = 0x19. Body 4 + 1 = 5.
        {"report", bytes({0x05, 0x80, 0x4c, 0x4b, 0x40, 0x19}), ProbeMessage{5000000, 25}},
        // Bitrate 63 is the last 1-byte value (3f); RTT 64 is the first 2-byte value (40 40). Body 1 + 2 = 3.
        {"boundaries", bytes({0x03, 0x3f, 0x40, 0x40}), ProbeMessage{63, 64}},
        // Two 8-byte maxima: Message Length 16 = 0x10.
        {"maximum",
         bytes({0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}),
         ProbeMessage{kMaxVarint, kMaxVarint}},
    };
    return table;
}

TEST(Moqlite06Probe, VectorsDecodeAndEncode) {
    for (const auto& v : vectors()) {
        SCOPED_TRACE(v.name);
        Cursor input(v.wire);
        const auto result = decode_probe(input);
        ASSERT_TRUE(std::holds_alternative<ProbeMessage>(result));
        EXPECT_EQ(std::get<ProbeMessage>(result), v.value);
        EXPECT_EQ(encode(v.value), v.wire);
        EXPECT_EQ(input.remaining(), 0u);
    }
}

TEST(Moqlite06Probe, EveryPrefixOfEveryVectorIsNeedMore) {
    for (const auto& v : vectors()) {
        for (std::size_t cut = 0; cut < v.wire.size(); ++cut) {
            SCOPED_TRACE(std::string(v.name) + " cut " + std::to_string(cut));
            const Bytes prefix(v.wire.begin(), v.wire.begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_probe(input)));
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
}

TEST(Moqlite06Probe, MessageLengthLiesAreProtocolViolations) {
    EXPECT_EQ(error_code(decode_p(bytes({0x03, 0x00, 0x00, 0x00}))), DecodeErrorCode::ProtocolViolation);  // trailing
    EXPECT_EQ(error_code(decode_p(bytes({0x01, 0x00}))), DecodeErrorCode::ProtocolViolation);               // RTT missing
    EXPECT_EQ(error_code(decode_p(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);                     // empty
    // Bitrate claims two bytes (41) but the Message Length leaves one.
    EXPECT_EQ(error_code(decode_p(bytes({0x01, 0x41}))), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Probe, MessageLengthLimitAndCursorBehaviour) {
    DecodeLimits limits;
    limits.max_message_length = 1;
    EXPECT_EQ(error_code(decode_p(vectors()[0].wire, limits)), DecodeErrorCode::LengthExceedsLimit);

    const Bytes bad = bytes({0x03, 0x00, 0x00, 0x00});
    Cursor input(bad);
    EXPECT_EQ(error_code(decode_probe(input)), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.remaining(), bad.size());

    const Bytes wire = vectors()[1].wire;
    Cursor huge(wire, std::numeric_limits<std::size_t>::max());
    EXPECT_NO_THROW({
        const auto result = decode_probe(huge);
        EXPECT_FALSE(std::holds_alternative<ProbeMessage>(result));
    });
}

TEST(Moqlite06Probe, ConsecutiveReportsAdvanceTheCursor) {
    Bytes wire = vectors()[1].wire;
    wire.insert(wire.end(), vectors()[2].wire.begin(), vectors()[2].wire.end());
    Cursor input(wire);
    const auto a = decode_probe(input);
    const auto b = decode_probe(input);
    ASSERT_TRUE(std::holds_alternative<ProbeMessage>(a));
    ASSERT_TRUE(std::holds_alternative<ProbeMessage>(b));
    EXPECT_EQ(std::get<ProbeMessage>(b), (ProbeMessage{63, 64}));
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06Probe, EncoderRefusesBadValuesAndWritesNothing) {
    ByteWriter out(64);
    EXPECT_EQ(encode_probe(ProbeMessage{kMaxVarint + 1, 0}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_probe(ProbeMessage{0, kMaxVarint + 1}, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    ByteWriter tiny(2);
    EXPECT_EQ(encode_probe(ProbeMessage{5000000, 25}, tiny), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_EQ(tiny.size(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
