#include "moq/interop/wire/moqlite06/datagram.h"

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

Bytes text(const std::string& value) {
    Bytes result;
    for (const char c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

DecodeResult<DatagramBody> decode(const Bytes& data) {
    Cursor input(data);
    return decode_datagram_body(input);
}

DecodeErrorCode error_code(const DecodeResult<DatagramBody>& result) {
    const auto* error = std::get_if<DecodeError>(&result);
    EXPECT_NE(error, nullptr);
    return error ? error->code : DecodeErrorCode::InvalidValue;
}

Bytes encode(const DatagramBody& body) {
    ByteWriter out(2048);
    EXPECT_FALSE(encode_datagram_body(body, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

struct Vector {
    const char* name;
    Bytes wire;
    DatagramBody value;
};

const std::vector<Vector>& vectors() {
    static const std::vector<Vector> table = {
        // Subscribe ID 0, Group Sequence 0, Timestamp 0, empty payload: three zero bytes, no length prefix.
        {"empty", bytes({0x00, 0x00, 0x00}), DatagramBody{0, 0, 0, {}}},
        // id 1 = 01; sequence 300 = 0x012c -> 41 2c; timestamp 90000 = 0x00015f90 -> 80 01 5f 90; payload "ab".
        {"typical", bytes({0x01, 0x41, 0x2c, 0x80, 0x01, 0x5f, 0x90, 0x61, 0x62}),
         DatagramBody{1, 300, 90000, text("ab")}},
        // Three 8-byte maxima, then one payload byte.
        {"maximum",
         bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x78}),
         DatagramBody{kMaxVarint, kMaxVarint, kMaxVarint, text("x")}},
        // A NUL payload is data.
        {"nul_payload", bytes({0x02, 0x03, 0x04, 0x00, 0x00}), DatagramBody{2, 3, 4, Bytes(2, std::byte{0})}},
    };
    return table;
}

TEST(Moqlite06Datagram, VectorsDecodeAndEncode) {
    for (const auto& v : vectors()) {
        SCOPED_TRACE(v.name);
        Cursor input(v.wire);
        const auto result = decode_datagram_body(input);
        ASSERT_TRUE(std::holds_alternative<DatagramBody>(result));
        EXPECT_EQ(std::get<DatagramBody>(result), v.value);
        EXPECT_EQ(input.remaining(), 0u);
        EXPECT_EQ(encode(v.value), v.wire);
    }
}

// A datagram is whole or it is not: a cut header is a protocol violation, never "need more bytes".
TEST(Moqlite06Datagram, EveryStrictPrefixOfTheHeaderIsAViolation) {
    const std::vector<std::pair<std::string, Bytes>> headers = {
        {"empty", bytes({0x00, 0x00, 0x00})},
        {"typical", bytes({0x01, 0x41, 0x2c, 0x80, 0x01, 0x5f, 0x90})},
    };
    for (const auto& [name, header] : headers) {
        for (std::size_t cut = 0; cut < header.size(); ++cut) {
            SCOPED_TRACE(name + " cut " + std::to_string(cut));
            const Bytes prefix(header.begin(), header.begin() + static_cast<std::ptrdiff_t>(cut));
            Cursor input(prefix);
            EXPECT_EQ(error_code(decode_datagram_body(input)), DecodeErrorCode::ProtocolViolation);
            EXPECT_EQ(input.remaining(), prefix.size());
        }
    }
    // A 2-byte varint whose second byte is missing, then nothing.
    EXPECT_EQ(error_code(decode(bytes({0x00, 0x41}))), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Datagram, TheBodyLimitIs1200Bytes) {
    EXPECT_EQ(kMaxDatagramBody, 1200u);
    Bytes exact = bytes({0x00, 0x00, 0x00});
    exact.resize(1200, std::byte{0x41});
    const auto ok = decode(exact);
    ASSERT_TRUE(std::holds_alternative<DatagramBody>(ok));
    EXPECT_EQ(std::get<DatagramBody>(ok).payload.size(), 1197u);

    Bytes over = exact;
    over.push_back(std::byte{0x41});
    EXPECT_EQ(error_code(decode(over)), DecodeErrorCode::LengthExceedsLimit);
    // Checked before the header is read: a 1201 byte datagram of garbage is over the limit, not malformed.
    EXPECT_EQ(error_code(decode(Bytes(1201, std::byte{0xff}))), DecodeErrorCode::LengthExceedsLimit);

    ByteWriter out(2048);
    EXPECT_FALSE(encode_datagram_body(DatagramBody{0, 0, 0, Bytes(1197, std::byte{1})}, out).has_value());
    EXPECT_EQ(out.size(), 1200u);
    ByteWriter refused(2048);
    EXPECT_EQ(encode_datagram_body(DatagramBody{0, 0, 0, Bytes(1198, std::byte{1})}, refused),
              std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(refused.size(), 0u);
}

TEST(Moqlite06Datagram, ErrorsLeaveTheCursorUnmovedAndHugeOffsetsDoNotThrow) {
    const Bytes cut = bytes({0x01, 0x41});
    Cursor input(cut);
    EXPECT_EQ(error_code(decode_datagram_body(input)), DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.remaining(), cut.size());

    const Bytes wire = vectors()[1].wire;
    Cursor huge(wire, std::numeric_limits<std::size_t>::max());
    EXPECT_NO_THROW({
        const auto result = decode_datagram_body(huge);
        EXPECT_FALSE(std::holds_alternative<DatagramBody>(result));
    });
}

TEST(Moqlite06Datagram, EncoderRefusesBadValuesAndWritesNothing) {
    ByteWriter out(64);
    EXPECT_EQ(encode_datagram_body(DatagramBody{kMaxVarint + 1, 0, 0, {}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_datagram_body(DatagramBody{0, kMaxVarint + 1, 0, {}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    EXPECT_EQ(encode_datagram_body(DatagramBody{0, 0, kMaxVarint + 1, {}}, out),
              std::optional<EncodeError>(EncodeError::InvalidValue));
    ByteWriter tiny(2);
    EXPECT_EQ(encode_datagram_body(DatagramBody{0, 0, 0, {}}, tiny), std::optional<EncodeError>(EncodeError::OutputCapacity));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_EQ(tiny.size(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
