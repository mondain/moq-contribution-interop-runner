#include "moq/interop/scenarios/location_filter_param.h"

#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using Bytes = std::vector<std::byte>;
using Type = wire::draft22::LocationFilterType;
constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();

void vi(Bytes& out, std::uint64_t value) {
    wire::ByteWriter writer(9);
    ASSERT_TRUE(wire::write_vi64(value, writer));
    out.insert(out.end(), writer.bytes().begin(), writer.bytes().end());
}

Bytes expected_draft21(const FilterFields& fields) {
    Bytes payload;
    for (const auto f : fields) vi(payload, f);
    Bytes out;
    vi(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

wire::draft22::LocationFilter decode_all(const Bytes& bytes) {
    wire::Cursor cursor(bytes);
    auto result = wire::draft22::decode_location_filter(cursor);
    EXPECT_TRUE(std::holds_alternative<wire::draft22::LocationFilter>(result));
    EXPECT_EQ(cursor.remaining(), 0u);
    return std::holds_alternative<wire::draft22::LocationFilter>(result)
               ? std::get<wire::draft22::LocationFilter>(result)
               : wire::draft22::LocationFilter{};
}

TEST(LocationFilterParam, Draft21BytesAreLengthPrefixedVarints) {
    const std::vector<FilterFields> cases = {{}, {7, 9}, {0, 0}, {7, 0, 0}, {7, 9, 0, 9}, {kMax, 0, 1}};
    for (const auto& fields : cases) {
        EXPECT_EQ(filter_param_value(fields), expected_draft21(fields));
        EXPECT_EQ(nested_filter_param_value(fields), expected_draft21(fields));
    }
}

TEST(LocationFilterParam, Draft22MapsEachFieldList) {
    ScopedWireDraft wire22(22);
    {
        const auto bytes = filter_param_value({});
        ASSERT_EQ(bytes.size(), 1u);
        EXPECT_EQ(decode_all(bytes).type, Type::None);
    }
    {
        const auto bytes = filter_param_value({0, 0});
        EXPECT_EQ(bytes.size(), 1u);
        EXPECT_EQ(decode_all(bytes).type, Type::NextObject);
    }
    {
        const auto bytes = filter_param_value({7, 9});
        EXPECT_EQ(bytes[0], static_cast<std::byte>(2));  // the Type, not a length
        const auto f = decode_all(bytes);
        EXPECT_EQ(f.type, Type::Absolute);
        EXPECT_EQ(f.start_group, 7u);
        EXPECT_EQ(f.start_object, 9u);
    }
    {
        const auto bytes = filter_param_value({7, 0, 0});
        EXPECT_EQ(bytes[0], static_cast<std::byte>(3));
        const auto f = decode_all(bytes);
        EXPECT_EQ(f.type, Type::AbsoluteBounded);
        EXPECT_EQ(f.start_group, 7u);
        EXPECT_EQ(f.start_object, 0u);
        EXPECT_EQ(f.end_group_delta, std::optional<std::uint64_t>(0));
    }
    {
        const auto bytes = filter_param_value({7, 9, 0, 9});
        EXPECT_EQ(bytes[0], static_cast<std::byte>(4));
        const auto f = decode_all(bytes);
        EXPECT_EQ(f.type, Type::AbsoluteRange);
        EXPECT_EQ(f.start_group, 7u);
        EXPECT_EQ(f.start_object, 9u);
        EXPECT_EQ(f.end_group_delta, std::optional<std::uint64_t>(0));
        EXPECT_EQ(f.end_object, std::optional<std::uint64_t>(9));
    }
    EXPECT_EQ(nested_filter_param_value({7, 9}), filter_param_value({7, 9}));
}

TEST(LocationFilterParam, Draft22RefusesUnrepresentableSizes) {
    ScopedWireDraft wire22(22);
    EXPECT_THROW(filter_param_value({1, 2, 3, 4, 5}), std::logic_error);
    EXPECT_THROW(filter_param_value({1}), std::logic_error);
}

// Verification pins (observed behavior).
// Draft 22 section 9.20.9: StartGroup + EndGroupDelta overflow is a PROTOCOL_VIOLATION. The builder
// refuses it up front (the draft 22 encoder returns InvalidValue), so no wrong byte is ever produced;
// the decoder independently reports ProtocolViolation for the hand-built bytes.
TEST(LocationFilterParam, PinOverflowIsRefusedByBuilderAndDecoder) {
    ScopedWireDraft wire22(22);
    EXPECT_THROW(filter_param_value({kMax, 0, 1}), std::logic_error);

    Bytes raw;
    vi(raw, 3);
    vi(raw, kMax);
    vi(raw, 0);
    vi(raw, 1);
    wire::Cursor cursor(raw);
    const auto result = wire::draft22::decode_location_filter(cursor);
    ASSERT_TRUE(std::holds_alternative<wire::DecodeError>(result));
    EXPECT_EQ(std::get<wire::DecodeError>(result).code, wire::DecodeErrorCode::ProtocolViolation);
}

// {0,0,UINT64_MAX}: StartGroup 0 + delta UINT64_MAX does not overflow, so it encodes and decodes.
TEST(LocationFilterParam, PinMaxDeltaFromGroupZeroEncodes) {
    ScopedWireDraft wire22(22);
    const auto bytes = filter_param_value({0, 0, kMax});
    const auto f = decode_all(bytes);
    EXPECT_EQ(f.type, Type::AbsoluteBounded);
    EXPECT_EQ(f.end_group_delta, std::optional<std::uint64_t>(kMax));
}

// {} nested: encodes as the single Type byte 0 (None), same as top level; under wire 21 it is the single
// byte 0 length prefix, which is the same octet.
TEST(LocationFilterParam, PinEmptyNestedIsSingleZeroByte) {
    const Bytes zero = {std::byte{0}};
    EXPECT_EQ(nested_filter_param_value({}), zero);
    ScopedWireDraft wire22(22);
    EXPECT_EQ(nested_filter_param_value({}), zero);
}

}  // namespace
}  // namespace moq::interop::scenarios
