#include "moq/interop/wire/cursor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::wire {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

void expect_bytes(std::span<const std::byte> actual, const std::vector<std::byte>& expected) {
    EXPECT_TRUE(std::ranges::equal(actual, expected));
}

std::uint64_t require_value(const DecodeResult<std::uint64_t>& result) {
    EXPECT_TRUE(std::holds_alternative<std::uint64_t>(result));
    return std::get<std::uint64_t>(result);
}

struct Vi64Vector {
    std::uint64_t value;
    std::vector<std::byte> encoded;
};

const std::vector<Vi64Vector>& boundary_vectors() {
    static const std::vector<Vi64Vector> vectors{
        {0, bytes({0x00})},
        {127, bytes({0x7f})},
        {128, bytes({0x80, 0x80})},
        {16'383, bytes({0xbf, 0xff})},
        {16'384, bytes({0xc0, 0x40, 0x00})},
        {2'097'151, bytes({0xdf, 0xff, 0xff})},
        {2'097'152, bytes({0xe0, 0x20, 0x00, 0x00})},
        {268'435'455, bytes({0xef, 0xff, 0xff, 0xff})},
        {268'435'456, bytes({0xf0, 0x10, 0x00, 0x00, 0x00})},
        {34'359'738'367ULL, bytes({0xf7, 0xff, 0xff, 0xff, 0xff})},
        {34'359'738'368ULL, bytes({0xf8, 0x08, 0x00, 0x00, 0x00, 0x00})},
        {4'398'046'511'103ULL, bytes({0xfb, 0xff, 0xff, 0xff, 0xff, 0xff})},
        {4'398'046'511'104ULL, bytes({0xfc, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00})},
        {562'949'953'421'311ULL, bytes({0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
        {562'949'953'421'312ULL,
         bytes({0xfe, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})},
        {72'057'594'037'927'935ULL,
         bytes({0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
        {72'057'594'037'927'936ULL,
         bytes({0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})},
        {std::numeric_limits<std::uint64_t>::max(),
         bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
    };
    return vectors;
}

const std::vector<Vi64Vector>& draft_table_vectors() {
    static const std::vector<Vi64Vector> vectors{
        {37, bytes({0x25})},
        {37, bytes({0x80, 0x25})},
        {15'293, bytes({0xbb, 0xbd})},
        {226'442'877, bytes({0xed, 0x7f, 0x3e, 0x7d})},
        {2'893'212'287'960ULL, bytes({0xfa, 0xa1, 0xa0, 0xe4, 0x03, 0xd8})},
        {151'288'809'941'952ULL, bytes({0xfc, 0x89, 0x98, 0xab, 0xc6, 0x6b, 0xc0})},
        {70'423'237'261'249'041ULL,
         bytes({0xfe, 0xfa, 0x31, 0x8f, 0xa8, 0xe3, 0xca, 0x11})},
        {std::numeric_limits<std::uint64_t>::max(),
         bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
    };
    return vectors;
}

TEST(CursorTest, DecodesEveryWidthBoundaryAndWritesItsShortestEncoding) {
    for (const auto& vector : boundary_vectors()) {
        SCOPED_TRACE(vector.value);
        Cursor input(vector.encoded);
        EXPECT_EQ(require_value(read_vi64(input)), vector.value);
        EXPECT_EQ(input.offset(), vector.encoded.size());
        EXPECT_EQ(input.remaining(), 0u);

        ByteWriter output(9);
        ASSERT_TRUE(write_vi64(vector.value, output));
        expect_bytes(output.bytes(), vector.encoded);
    }
}

TEST(CursorTest, DecodesEveryDraft18Table2ExampleFromLiteralBytes) {
    for (const auto& vector : draft_table_vectors()) {
        SCOPED_TRACE(vector.value);
        Cursor input(vector.encoded);
        EXPECT_EQ(require_value(read_vi64(input)), vector.value);
        EXPECT_EQ(input.remaining(), 0u);

        ByteWriter shortest(9);
        ASSERT_TRUE(write_vi64(vector.value, shortest));
        Cursor round_trip(shortest.bytes());
        EXPECT_EQ(require_value(read_vi64(round_trip)), vector.value);
    }
}

TEST(CursorTest, AcceptsRepresentativeNonMinimalEncodingAtEveryWiderWidth) {
    const std::vector<std::vector<std::byte>> encodings{
        bytes({0x25}),
        bytes({0x80, 0x25}),
        bytes({0xc0, 0x00, 0x25}),
        bytes({0xe0, 0x00, 0x00, 0x25}),
        bytes({0xf0, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xf8, 0x00, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25}),
    };
    for (const auto& encoded : encodings) {
        SCOPED_TRACE(encoded.size());
        Cursor input(encoded);
        EXPECT_EQ(require_value(read_vi64(input)), 37u);
        EXPECT_EQ(input.offset(), encoded.size());
    }
}

TEST(CursorTest, TruncationAtEveryByteDoesNotAdvanceCursor) {
    const std::vector<std::vector<std::byte>> encodings{
        bytes({0x80, 0x25}),
        bytes({0xc0, 0x00, 0x25}),
        bytes({0xe0, 0x00, 0x00, 0x25}),
        bytes({0xf0, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xf8, 0x00, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25}),
        bytes({0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25}),
    };
    for (const auto& encoded : encodings) {
        for (std::size_t available = 0; available < encoded.size(); ++available) {
            SCOPED_TRACE(::testing::Message() << "width=" << encoded.size()
                                              << " available=" << available);
            Cursor input(std::span<const std::byte>(encoded).first(available), 100);
            const auto result = read_vi64(input);
            ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
            const auto& need = std::get<NeedMore>(result);
            EXPECT_EQ(need.offset, 100u);
            EXPECT_EQ(need.required, available == 0 ? 1u : encoded.size());
            EXPECT_EQ(need.available, available);
            EXPECT_EQ(input.offset(), 100u);
        }
    }
}

TEST(CursorTest, ReadsSequentialValuesAndTracksAbsoluteOffsets) {
    const auto encoded = bytes({0x25, 0x80, 0x25, 0xbb, 0xbd});
    Cursor input(encoded, 50);
    EXPECT_EQ(require_value(read_vi64(input)), 37u);
    EXPECT_EQ(input.offset(), 51u);
    EXPECT_EQ(require_value(read_vi64(input)), 37u);
    EXPECT_EQ(input.offset(), 53u);
    EXPECT_EQ(require_value(read_vi64(input)), 15'293u);
    EXPECT_EQ(input.offset(), 55u);
    EXPECT_EQ(input.remaining(), 0u);

    const auto result = read_vi64(input);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(result));
    EXPECT_EQ(std::get<NeedMore>(result).offset, 55u);
    EXPECT_EQ(input.offset(), 55u);
}

TEST(CursorTest, EmptyInputNeedsOneByteWithoutAdvancingAnyDecoder) {
    const std::array<std::byte, 0> empty{};

    Cursor integer_input(empty, 12);
    const auto integer_result = read_vi64(integer_input);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(integer_result));
    EXPECT_EQ(std::get<NeedMore>(integer_result).offset, 12u);
    EXPECT_EQ(std::get<NeedMore>(integer_result).required, 1u);
    EXPECT_EQ(std::get<NeedMore>(integer_result).available, 0u);
    EXPECT_EQ(integer_input.offset(), 12u);

    Cursor bytes_input(empty, 12);
    const auto bytes_result = read_length_prefixed_bytes(bytes_input, 0);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(bytes_result));
    EXPECT_EQ(std::get<NeedMore>(bytes_result).required, 1u);
    EXPECT_EQ(bytes_input.offset(), 12u);

    Cursor string_input(empty, 12);
    const auto string_result = read_length_prefixed_string(string_input, 0);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(string_result));
    EXPECT_EQ(std::get<NeedMore>(string_result).required, 1u);
    EXPECT_EQ(string_input.offset(), 12u);
}

TEST(CursorTest, RawReadNeedMoreAndWriterLimitFailuresAreAtomic) {
    const auto encoded = bytes({0x01, 0x02});
    Cursor input(encoded, 40);
    const auto missing = read_bytes(input, 3);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(missing));
    EXPECT_EQ(std::get<NeedMore>(missing).offset, 40u);
    EXPECT_EQ(input.offset(), 40u);
    const auto first = read_bytes(input, 1);
    ASSERT_TRUE(std::holds_alternative<std::span<const std::byte>>(first));
    expect_bytes(std::get<std::span<const std::byte>>(first), bytes({0x01}));
    EXPECT_EQ(input.offset(), 41u);

    ByteWriter output(2);
    ASSERT_TRUE(output.append_byte(std::byte{0xaa}));
    const auto before = std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
    EXPECT_FALSE(output.append_bytes(bytes({0xbb, 0xcc})));
    expect_bytes(output.bytes(), before);

    ByteWriter too_small(1);
    EXPECT_FALSE(write_vi64(128, too_small));
    EXPECT_TRUE(too_small.bytes().empty());
}

TEST(CursorTest, ReadsBoundedEmptyAndEmbeddedNullFieldsWithoutUtf8Policy) {
    const auto empty = bytes({0x00});
    Cursor empty_input(empty);
    const auto empty_value = read_length_prefixed_bytes(empty_input, 16);
    ASSERT_TRUE(std::holds_alternative<std::span<const std::byte>>(empty_value));
    EXPECT_TRUE(std::get<std::span<const std::byte>>(empty_value).empty());
    EXPECT_EQ(empty_input.offset(), 1u);

    Cursor empty_string_input(empty);
    const auto empty_string = read_length_prefixed_string(empty_string_input, 0);
    ASSERT_TRUE(std::holds_alternative<std::string>(empty_string));
    EXPECT_TRUE(std::get<std::string>(empty_string).empty());

    const auto opaque = bytes({0x03, 0x61, 0x00, 0xff});
    Cursor byte_input(opaque);
    const auto byte_value = read_length_prefixed_bytes(byte_input, 3);
    ASSERT_TRUE(std::holds_alternative<std::span<const std::byte>>(byte_value));
    expect_bytes(std::get<std::span<const std::byte>>(byte_value), bytes({0x61, 0x00, 0xff}));

    const auto text = bytes({0x03, 0x61, 0x00, 0x62});
    Cursor string_input(text);
    const auto string_value = read_length_prefixed_string(string_input, 3);
    ASSERT_TRUE(std::holds_alternative<std::string>(string_value));
    EXPECT_EQ(std::get<std::string>(string_value), std::string("a\0b", 3));
}

TEST(CursorTest, LengthPrefixTruncationAndPayloadTruncationRollBack) {
    const auto prefix_only = bytes({0x80});
    Cursor prefix_input(prefix_only, 10);
    const auto prefix_result = read_length_prefixed_bytes(prefix_input, 1'000);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(prefix_result));
    EXPECT_EQ(std::get<NeedMore>(prefix_result).offset, 10u);
    EXPECT_EQ(std::get<NeedMore>(prefix_result).required, 2u);
    EXPECT_EQ(prefix_input.offset(), 10u);

    const auto short_payload = bytes({0x03, 0x61});
    Cursor payload_input(short_payload, 20);
    const auto payload_result = read_length_prefixed_bytes(payload_input, 3);
    ASSERT_TRUE(std::holds_alternative<NeedMore>(payload_result));
    const auto& need = std::get<NeedMore>(payload_result);
    EXPECT_EQ(need.offset, 21u);
    EXPECT_EQ(need.required, 3u);
    EXPECT_EQ(need.available, 1u);
    EXPECT_EQ(payload_input.offset(), 20u);
}

TEST(CursorTest, RejectsHugeOrOverflowingDeclaredLengthsBeforeCopy) {
    const auto four_gib = bytes({0xf1, 0x00, 0x00, 0x00, 0x00});
    Cursor limited(four_gib, 70);
    const auto limited_result = read_length_prefixed_bytes(limited, 1u << 20);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(limited_result));
    const auto& limit_error = std::get<DecodeError>(limited_result);
    EXPECT_EQ(limit_error.code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(limit_error.offset, 70u);
    EXPECT_EQ(limit_error.detail, "declared length exceeds configured limit");
    EXPECT_EQ(limited.offset(), 70u);

    const auto one_byte = bytes({0x01, 0x78});
    Cursor overflowing(one_byte, std::numeric_limits<std::size_t>::max() - 1u);
    const auto overflow_result = read_length_prefixed_bytes(overflowing, 1);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(overflow_result));
    const auto& overflow_error = std::get<DecodeError>(overflow_result);
    EXPECT_EQ(overflow_error.code, DecodeErrorCode::OffsetOverflow);
    EXPECT_EQ(overflow_error.detail, "declared length overflows absolute cursor offset");
    EXPECT_EQ(overflowing.offset(), std::numeric_limits<std::size_t>::max() - 1u);

    if constexpr (std::numeric_limits<std::size_t>::max() <
                  std::numeric_limits<std::uint64_t>::max()) {
        const auto maximum = bytes(
            {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
        Cursor narrowing(maximum);
        const auto narrowing_result =
            read_length_prefixed_bytes(narrowing, std::numeric_limits<std::size_t>::max());
        ASSERT_TRUE(std::holds_alternative<DecodeError>(narrowing_result));
        EXPECT_EQ(std::get<DecodeError>(narrowing_result).code,
                  DecodeErrorCode::LengthNotRepresentable);
        EXPECT_EQ(narrowing.offset(), 0u);
    }
}

TEST(CursorTest, WritesLengthPrefixedValuesAtomically) {
    ByteWriter empty_output(1);
    ASSERT_TRUE(write_length_prefixed_string({}, empty_output));
    expect_bytes(empty_output.bytes(), bytes({0x00}));

    ByteWriter bytes_output(4);
    ASSERT_TRUE(write_length_prefixed_bytes(bytes({0x61, 0x00, 0xff}), bytes_output));
    expect_bytes(bytes_output.bytes(), bytes({0x03, 0x61, 0x00, 0xff}));

    ByteWriter text_output(4);
    ASSERT_TRUE(write_length_prefixed_string(std::string_view("a\0b", 3), text_output));
    expect_bytes(text_output.bytes(), bytes({0x03, 0x61, 0x00, 0x62}));

    ByteWriter limited(4);
    ASSERT_TRUE(limited.append_byte(std::byte{0xaa}));
    const auto before = std::vector<std::byte>(limited.bytes().begin(), limited.bytes().end());
    EXPECT_FALSE(write_length_prefixed_string("abc", limited));
    expect_bytes(limited.bytes(), before);
}

TEST(CursorTest, WriterSupportsAliasedSelfAppendAndSubspans) {
    ByteWriter direct(6);
    ASSERT_TRUE(direct.append_bytes(bytes({0x61, 0x62, 0x63})));
    const auto entire_value = direct.bytes();
    ASSERT_TRUE(direct.append_bytes(entire_value));
    expect_bytes(direct.bytes(), bytes({0x61, 0x62, 0x63, 0x61, 0x62, 0x63}));

    ByteWriter subspan(6);
    ASSERT_TRUE(subspan.append_bytes(bytes({0x61, 0x62, 0x63, 0x64})));
    const auto middle = subspan.bytes().subspan(1, 2);
    ASSERT_TRUE(subspan.append_bytes(middle));
    expect_bytes(subspan.bytes(), bytes({0x61, 0x62, 0x63, 0x64, 0x62, 0x63}));

    const auto empty = subspan.bytes().subspan(subspan.size(), 0);
    ASSERT_TRUE(subspan.append_bytes(empty));
    expect_bytes(subspan.bytes(), bytes({0x61, 0x62, 0x63, 0x64, 0x62, 0x63}));
}

TEST(CursorTest, AliasedWritesRemainAtomicAtTheOutputLimit) {
    ByteWriter output(5);
    ASSERT_TRUE(output.append_bytes(bytes({0x61, 0x62, 0x63})));
    const auto alias = output.bytes();
    const auto before = std::vector<std::byte>(alias.begin(), alias.end());
    EXPECT_FALSE(output.append_bytes(alias));
    expect_bytes(output.bytes(), before);

    EXPECT_FALSE(write_length_prefixed_bytes(alias, output));
    expect_bytes(output.bytes(), before);
}

TEST(CursorTest, LengthPrefixesAliasedWriterBytesBeforeAppendingTheirCopy) {
    ByteWriter output(7);
    ASSERT_TRUE(output.append_bytes(bytes({0x61, 0x62, 0x63})));
    const auto alias = output.bytes();
    ASSERT_TRUE(write_length_prefixed_bytes(alias, output));
    expect_bytes(output.bytes(),
                 bytes({0x61, 0x62, 0x63, 0x03, 0x61, 0x62, 0x63}));
}

}  // namespace
}  // namespace moq::interop::wire
