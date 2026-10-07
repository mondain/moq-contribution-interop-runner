#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/varint.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace moq::interop::wire::moqlite06 {
namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

struct VarintVector {
    std::uint64_t value;
    Bytes wire;
};

std::vector<VarintVector> varint_vectors() {
    return {
        {0, bytes({0x00})},
        {63, bytes({0x3f})},
        {64, bytes({0x40, 0x40})},
        {100, bytes({0x40, 0x64})},
        {16383, bytes({0x7f, 0xff})},
        {16384, bytes({0x80, 0x00, 0x40, 0x00})},
        {(std::uint64_t{1} << 30) - 1, bytes({0xbf, 0xff, 0xff, 0xff})},
        {std::uint64_t{1} << 30, bytes({0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00})},
        {kMaxVarint, bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
    };
}

Bytes encode_varint(std::uint64_t value) {
    ByteWriter out(16);
    EXPECT_TRUE(write_varint(value, out));
    return Bytes(out.bytes().begin(), out.bytes().end());
}

TEST(Moqlite06Varint, GoldenVectorsDecodeAndEncode) {
    for (const auto& vector : varint_vectors()) {
        Cursor input(vector.wire);
        const auto result = read_varint(input);
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(result)) << vector.value;
        EXPECT_EQ(std::get<std::uint64_t>(result), vector.value);
        EXPECT_EQ(input.offset(), vector.wire.size());
        EXPECT_EQ(input.remaining(), 0u);

        EXPECT_EQ(encode_varint(vector.value), vector.wire) << vector.value;
        EXPECT_EQ(varint_size(vector.value), vector.wire.size()) << vector.value;
    }
}

TEST(Moqlite06Varint, ValueAboveMaximumIsRefusedAndWritesNothing) {
    ByteWriter out(16);
    EXPECT_FALSE(write_varint(kMaxVarint + 1, out));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_FALSE(write_varint(~std::uint64_t{0}, out));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_EQ(varint_size(kMaxVarint + 1), 0u);
}

TEST(Moqlite06Varint, WriteWithoutCapacityWritesNothing) {
    ByteWriter out(1);
    EXPECT_FALSE(write_varint(64, out));  // needs 2 bytes
    EXPECT_EQ(out.size(), 0u);
    EXPECT_TRUE(write_varint(63, out));
    EXPECT_FALSE(write_varint(0, out));
    EXPECT_EQ(out.size(), 1u);
}

// RFC 9000 section 16 allows a sender to use more bytes than needed, so the decoder accepts
// non-minimal forms and advances by the wire width; only the encoder is required to be minimal.
TEST(Moqlite06Varint, NonMinimalFormsAreAccepted) {
    {
        const auto data = bytes({0x40, 0x01});
        Cursor input(data);
        const auto result = read_varint(input);
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(result));
        EXPECT_EQ(std::get<std::uint64_t>(result), 1u);
        EXPECT_EQ(input.offset(), 2u);
    }
    {
        const auto data = bytes({0x80, 0x00, 0x00, 0x01, 0x77});
        Cursor input(data);
        const auto result = read_varint(input);
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(result));
        EXPECT_EQ(std::get<std::uint64_t>(result), 1u);
        EXPECT_EQ(input.offset(), 4u);
        EXPECT_EQ(input.remaining(), 1u);
    }
}

TEST(Moqlite06Varint, EveryStrictPrefixIsNeedMoreWithCursorUnchanged) {
    for (const auto& vector : varint_vectors()) {
        for (std::size_t length = 0; length < vector.wire.size(); ++length) {
            const Bytes prefix(vector.wire.begin(),
                               vector.wire.begin() + static_cast<std::ptrdiff_t>(length));
            Cursor input(prefix);
            const auto result = read_varint(input);
            ASSERT_TRUE(std::holds_alternative<NeedMore>(result))
                << vector.value << " prefix " << length;
            const auto& need = std::get<NeedMore>(result);
            EXPECT_EQ(need.offset, 0u);
            // With no bytes the width is unknown, so only the first byte is required.
            EXPECT_EQ(need.required, length == 0 ? std::size_t{1} : vector.wire.size());
            EXPECT_EQ(need.available, length);
            EXPECT_EQ(input.offset(), 0u);
            EXPECT_EQ(input.remaining(), length);
        }
    }
}

TEST(Moqlite06Varint, EmptyInputIsNeedMore) {
    const Bytes empty;
    Cursor input(empty);
    const auto result = read_varint(input);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
    EXPECT_EQ(std::get<NeedMore>(result).required, 1u);
    EXPECT_EQ(std::get<NeedMore>(result).available, 0u);
}

TEST(Moqlite06String, DecodesAbc) {
    const auto data = bytes({0x03, 0x61, 0x62, 0x63, 0x99});
    Cursor input(data);
    const auto result = read_string(input, 16);
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), "abc");
    EXPECT_EQ(input.remaining(), 1u);
}

TEST(Moqlite06String, EmptyString) {
    const auto data = bytes({0x00});
    Cursor input(data);
    const auto result = read_string(input, 0);
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_TRUE(std::get<std::string>(result).empty());
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06String, LengthAboveLimitIsRejectedBeforeReadingBytes) {
    // Declared length 5 with no body bytes present: the limit check wins over NeedMore.
    const auto data = bytes({0x05});
    Cursor input(data);
    const auto result = read_string(input, 4);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Moqlite06String, HugeDeclaredLengthIsRejected) {
    const auto data = bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    Cursor input(data);
    const auto result = read_string(input, 1u << 16);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Moqlite06String, ShortBodyIsNeedMore) {
    const auto data = bytes({0x03, 0x61, 0x62});
    Cursor input(data);
    const auto result = read_string(input, 16);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Moqlite06String, ShortLengthPrefixIsNeedMore) {
    const auto data = bytes({0x40});
    Cursor input(data);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(read_string(input, 16)));
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Moqlite06String, WriteReadRoundTripIncludingTwoByteLength) {
    for (const std::string value : {std::string{}, std::string("abc"), std::string(63, 'x'),
                                    std::string(64, 'y'), std::string("a\0b\xff", 4)}) {
        ByteWriter out(1024);
        ASSERT_TRUE(write_string(value, out));
        const Bytes wire(out.bytes().begin(), out.bytes().end());
        const std::size_t prefix = value.size() < 64 ? 1u : 2u;
        EXPECT_EQ(wire.size(), prefix + value.size());
        if (value.size() == 64) {
            EXPECT_EQ(wire[0], std::byte{0x40});
            EXPECT_EQ(wire[1], std::byte{0x40});
        }
        Cursor input(wire);
        const auto result = read_string(input, 1024);
        ASSERT_TRUE(std::holds_alternative<std::string>(result));
        EXPECT_EQ(std::get<std::string>(result), value);
        EXPECT_EQ(input.remaining(), 0u);
    }
}

TEST(Moqlite06String, WriteWithoutCapacityWritesNothing) {
    ByteWriter out(3);
    EXPECT_FALSE(write_string("abc", out));  // needs 4
    EXPECT_EQ(out.size(), 0u);
}

TEST(Moqlite06Framing, ReadFramedBodyReturnsBodyAndLeavesTrailingByte) {
    const auto data = bytes({0x05, 0x01, 0x02, 0x03, 0x04, 0x05, 0x99});
    Cursor input(data);
    const auto result = read_framed_body(input);
    ASSERT_TRUE(std::holds_alternative<std::span<const std::byte>>(result));
    const auto body = std::get<std::span<const std::byte>>(result);
    EXPECT_EQ(Bytes(body.begin(), body.end()), bytes({0x01, 0x02, 0x03, 0x04, 0x05}));
    EXPECT_EQ(input.offset(), 6u);
    EXPECT_EQ(input.remaining(), 1u);
}

TEST(Moqlite06Framing, ZeroLengthBodyIsLegalAtFramingLevel) {
    const auto data = bytes({0x00});
    Cursor input(data);
    const auto result = read_framed_body(input);
    ASSERT_TRUE(std::holds_alternative<std::span<const std::byte>>(result));
    EXPECT_TRUE(std::get<std::span<const std::byte>>(result).empty());
    EXPECT_EQ(input.remaining(), 0u);
}

TEST(Moqlite06Framing, ShortBodyIsNeedMoreAndCursorUnchanged) {
    const auto data = bytes({0x05, 0x01, 0x02, 0x03, 0x04});
    Cursor input(data);
    const auto result = read_framed_body(input);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
    EXPECT_EQ(input.offset(), 0u);
    EXPECT_EQ(input.remaining(), 5u);
}

TEST(Moqlite06Framing, ShortLengthPrefixIsNeedMore) {
    const auto data = bytes({0x80, 0x00});
    Cursor input(data);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(read_framed_body(input)));
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Moqlite06Framing, LengthAboveLimitIsRejectedWithoutBody) {
    // A 2^62-1 length with no body: must be a limit error, never NeedMore or an allocation.
    const auto data = bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    Cursor input(data);
    const auto result = read_framed_body(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(input.offset(), 0u);

    DecodeLimits small;
    small.max_message_length = 4;
    const auto data2 = bytes({0x05, 1, 2, 3, 4, 5});
    Cursor input2(data2);
    const auto result2 = read_framed_body(input2, small);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result2));
    EXPECT_EQ(std::get<DecodeError>(result2).code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(input2.offset(), 0u);

    // Exactly at the limit is accepted.
    const auto data3 = bytes({0x04, 1, 2, 3, 4});
    Cursor input3(data3);
    EXPECT_TRUE(std::holds_alternative<std::span<const std::byte>>(read_framed_body(input3, small)));
}

TEST(Moqlite06Framing, DefaultLimitsMatchThePlan) {
    EXPECT_EQ(kDefaultLimits.max_message_length, std::size_t{1} << 20);
    EXPECT_EQ(kDefaultLimits.max_string_length, std::size_t{1} << 16);
    EXPECT_EQ(kDefaultLimits.max_parameters, 64u);
    EXPECT_EQ(kDefaultLimits.max_hops, 1024u);
}

TEST(Moqlite06Framing, WriteFramedMessageRoundTrip) {
    const auto body = bytes({0xaa, 0xbb, 0xcc});
    ByteWriter out(64);
    ASSERT_TRUE(write_framed_message(body, out));
    EXPECT_EQ(Bytes(out.bytes().begin(), out.bytes().end()), bytes({0x03, 0xaa, 0xbb, 0xcc}));

    const Bytes wire(out.bytes().begin(), out.bytes().end());
    Cursor input(wire);
    const auto result = read_framed_body(input);
    ASSERT_TRUE(std::holds_alternative<std::span<const std::byte>>(result));
    const auto decoded = std::get<std::span<const std::byte>>(result);
    EXPECT_EQ(Bytes(decoded.begin(), decoded.end()), body);
}

TEST(Moqlite06Framing, WriteFramedMessageRefusesWhenItDoesNotFit) {
    const Bytes body(10, std::byte{1});
    ByteWriter out(10);  // needs 11
    EXPECT_FALSE(write_framed_message(body, out));
    EXPECT_EQ(out.size(), 0u);

    const Bytes empty;
    ByteWriter none(0);
    EXPECT_FALSE(write_framed_message(empty, none));
    EXPECT_EQ(none.size(), 0u);
}

TEST(Moqlite06Framing, ExpectBodyConsumed) {
    const auto data = bytes({0x01, 0x02});
    Cursor body(data);
    const auto trailing = expect_body_consumed(body);
    ASSERT_TRUE(trailing.has_value());
    EXPECT_EQ(trailing->code, DecodeErrorCode::ProtocolViolation);

    ASSERT_TRUE(std::holds_alternative<std::span<const std::byte>>(read_bytes(body, 2)));
    EXPECT_FALSE(expect_body_consumed(body).has_value());
}

TEST(Moqlite06StreamType, WriteReadKnownTypes) {
    for (const std::uint64_t value : {std::uint64_t{0x1}, std::uint64_t{0x6}}) {
        ByteWriter out(8);
        ASSERT_TRUE(write_stream_type(value, out));
        EXPECT_EQ(out.size(), 1u);
        const Bytes wire(out.bytes().begin(), out.bytes().end());
        Cursor input(wire);
        const auto result = read_stream_type(input);
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(result));
        EXPECT_EQ(std::get<std::uint64_t>(result), value);
    }
}

TEST(Moqlite06StreamType, ClassificationAndUnknownTypes) {
    EXPECT_EQ(as_bidi_stream_type(0x1), BidiStreamType::Announce);
    EXPECT_EQ(as_bidi_stream_type(0x2), BidiStreamType::Subscribe);
    EXPECT_EQ(as_bidi_stream_type(0x3), BidiStreamType::Fetch);
    EXPECT_EQ(as_bidi_stream_type(0x4), BidiStreamType::Probe);
    EXPECT_EQ(as_bidi_stream_type(0x5), BidiStreamType::Goaway);
    EXPECT_EQ(as_bidi_stream_type(0x6), BidiStreamType::Track);
    EXPECT_EQ(as_bidi_stream_type(0x0), std::nullopt);
    EXPECT_EQ(as_bidi_stream_type(0x7), std::nullopt);
    EXPECT_EQ(as_uni_stream_type(0x0), UniStreamType::Group);
    EXPECT_EQ(as_uni_stream_type(0x1), UniStreamType::Setup);
    EXPECT_EQ(as_uni_stream_type(0x2), std::nullopt);
}

TEST(Moqlite06StreamType, ValueAboveOneByteRoundTripsAsTwoByteVarint) {
    ByteWriter out(8);
    ASSERT_TRUE(write_stream_type(0x40, out));
    const Bytes wire(out.bytes().begin(), out.bytes().end());
    EXPECT_EQ(wire, bytes({0x40, 0x40}));
    Cursor input(wire);
    const auto result = read_stream_type(input);
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(result));
    EXPECT_EQ(std::get<std::uint64_t>(result), 0x40u);
    EXPECT_EQ(std::get<std::uint64_t>(result) > 0x3f, true);
}

TEST(Moqlite06StreamType, ShortInputIsNeedMore) {
    const auto data = bytes({0x40});
    Cursor input(data);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(read_stream_type(input)));
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Moqlite06EncodeError, EnumHasTheAgreedMembers) {
    EXPECT_NE(EncodeError::InvalidValue, EncodeError::OutputCapacity);
    EXPECT_NE(EncodeError::OutputCapacity, EncodeError::LimitExceeded);
}

template <class T>
void expect_cursor_unmoved_unless_success(const DecodeResult<T>& result, const Cursor& input,
                                          std::size_t before) {
    if (!std::holds_alternative<T>(result)) EXPECT_EQ(input.offset(), before);
}

TEST(Moqlite06Robustness, RandomInputsNeverCrashOrAdvanceOnFailure) {
    std::mt19937_64 rng(0x6d6f71u);
    for (int iteration = 0; iteration < 20000; ++iteration) {
        Bytes data(static_cast<std::size_t>(rng() % 17));
        for (auto& byte : data) byte = static_cast<std::byte>(rng() & 0xffu);

        {
            Cursor input(data);
            const auto result = read_varint(input);
            expect_cursor_unmoved_unless_success(result, input, 0);
            EXPECT_FALSE(std::holds_alternative<DecodeError>(result));
        }
        {
            Cursor input(data);
            const auto result = read_string(input, 8);
            expect_cursor_unmoved_unless_success(result, input, 0);
        }
        {
            Cursor input(data);
            const auto result = read_framed_body(input);
            expect_cursor_unmoved_unless_success(result, input, 0);
            if (const auto* body = std::get_if<std::span<const std::byte>>(&result)) {
                EXPECT_LE(body->size(), data.size());
            }
        }
        {
            Cursor input(data);
            const auto result = read_stream_type(input);
            expect_cursor_unmoved_unless_success(result, input, 0);
        }
    }
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
