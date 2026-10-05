// The Message Parameter readers of the draft 21 contribution scenarios. Wire draft 21 carries
// LOCATION_FILTER (0x21) length-prefixed; wire draft 22 carries it as Type + fields with no length, so a
// reader that still framed it by a length would mis-read every parameter after it.

#include "draft21_contribution_walk_testing.h"

#include "moq/interop/scenarios/parameter_walk.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/cursor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using Bytes = std::vector<std::byte>;
using d21c::BlockForTest;
using d21c::BlockParameterForTest;
using d21c::SessionWalkForTest;

// Hex digits, spaces ignored.
Bytes h(std::string_view text) {
    Bytes out;
    int high = -1;
    for (const char c : text) {
        if (c == ' ') continue;
        const int digit = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        if (high < 0) {
            high = digit;
        } else {
            out.push_back(static_cast<std::byte>(high * 16 + digit));
            high = -1;
        }
    }
    return out;
}

BlockParameterForTest number(std::uint64_t type, std::uint64_t value) { return {type, value, std::nullopt, {}}; }
BlockParameterForTest raw(std::uint64_t type, std::string_view hex) { return {type, std::nullopt, std::nullopt, h(hex)}; }

// d21b's reader over "count + parameters".
BlockForTest block(std::string_view count_hex, std::string_view params_hex) {
    const auto body = h(std::string(count_hex) + std::string(params_hex));
    return d21c::d21b_parse_block_for_test(body);
}

SessionWalkForTest session(std::string_view params_hex, std::uint64_t count) {
    return d21c::session_walk_parameters_for_test(h(params_hex), count);
}

// Where an aborted walk leaves the cursor is not observable to either caller, so it is not compared.
SessionWalkForTest aborted(std::string_view params_hex, std::uint64_t count) {
    auto walk = session(params_hex, count);
    walk.consumed = 0;
    return walk;
}

SessionWalkForTest complete_walk(std::size_t count, std::size_t consumed) {
    SessionWalkForTest walk;
    walk.declared = count;
    walk.parsed = count;
    walk.consumed = consumed;
    return walk;
}

SessionWalkForTest broken_walk(std::size_t count, std::size_t parsed) {
    SessionWalkForTest walk;
    walk.declared = count;
    walk.parsed = parsed;
    walk.structure = false;
    return walk;
}

// ---- wire draft 21: the readers' behavior before the shared walk, pinned ------------------------

// FORWARD (0x10) 1, LOCATION_FILTER len 2 {7,9}, GROUP_ORDER (0x22) 1.
constexpr std::string_view kW21Between = "10 01  11 02 0709  01 01";
// LOCATION_FILTER len 4 {200,300}, GROUP_ORDER 1.
constexpr std::string_view kW21FirstMultiByte = "21 04 80c8 812c  01 01";
// EXPIRES (0x08) 5, LOCATION_FILTER len 0.
constexpr std::string_view kW21Last = "08 05  19 00";
// FILL_PARAMETERS len 6 { LOCATION_FILTER len 2 {7,9}, GROUP_ORDER 1 }, then INCLUDE_PROPERTIES (0x35) 1.
constexpr std::string_view kW21Nested = "23 06 21020709 0101  12 01";

TEST(ParameterWalkReaders, Wire21FilterBetweenOtherParameters) {
    const auto parsed = block("03", kW21Between);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{number(0x10, 1), raw(0x21, "0709"), number(0x22, 1)}));
    EXPECT_EQ(parsed.consumed, 9u);
    EXPECT_EQ(session(kW21Between, 3), complete_walk(3, 8));
}

TEST(ParameterWalkReaders, Wire21FilterFirstWithMultiByteFields) {
    const auto parsed = block("02", kW21FirstMultiByte);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{raw(0x21, "80c8812c"), number(0x22, 1)}));
    EXPECT_EQ(session(kW21FirstMultiByte, 2), complete_walk(2, 8));
}

TEST(ParameterWalkReaders, Wire21FilterLast) {
    const auto parsed = block("02", kW21Last);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{number(0x08, 5), raw(0x21, "")}));
    EXPECT_EQ(session(kW21Last, 2), complete_walk(2, 4));
}

TEST(ParameterWalkReaders, Wire21FilterNestedInFillParameters) {
    const auto parsed = block("02", kW21Nested);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{raw(0x23, "21020709 0101"), number(0x35, 1)}));
    EXPECT_EQ(session(kW21Nested, 2), complete_walk(2, 10));
}

TEST(ParameterWalkReaders, Wire21TruncatedFilterAborts) {
    EXPECT_FALSE(block("01", "21 05 07").ok);
    EXPECT_EQ(aborted("21 05 07", 1), broken_walk(1, 0));
    // A Length beyond 65535 is refused like a truncation.
    EXPECT_FALSE(block("01", "21 c10000").ok);
    EXPECT_EQ(aborted("21 c10000", 1), broken_walk(1, 0));
}

TEST(ParameterWalkReaders, Wire21UnknownTypeOverflowAndRepeat) {
    EXPECT_FALSE(block("02", "10 01  07 00").ok);
    auto unknown = session("10 01  07 00", 2);
    EXPECT_TRUE(unknown.unknown);
    EXPECT_TRUE(unknown.structure);
    EXPECT_EQ(unknown.parsed, 1u);

    EXPECT_FALSE(block("02", "10 01  ffffffffffffffffff").ok);
    auto overflow = session("10 01  ffffffffffffffffff", 2);
    EXPECT_TRUE(overflow.overflow);
    EXPECT_EQ(overflow.parsed, 1u);

    // A repeated FORWARD whose value is missing: the repeat is recorded before the value is read.
    auto repeat = session("10 01  00", 2);
    EXPECT_TRUE(repeat.forbidden_repeat);
    EXPECT_FALSE(repeat.structure);
    // An unknown type stops the walk at its first occurrence, before any repeat of it.
    auto unknown_repeat = session("07  00", 2);
    EXPECT_TRUE(unknown_repeat.unknown);
    EXPECT_FALSE(unknown_repeat.forbidden_repeat);
    // Repeated tokens are permitted.
    EXPECT_EQ(session("03 01 aa  00 01 bb", 2), complete_walk(2, 6));
    // d21b caps the Number of Parameters at 64.
    EXPECT_FALSE(block("41", "").ok);
}

// ---- wire draft 22: LOCATION_FILTER is Type + fields with no length -----------------------------

// FORWARD 1, LOCATION_FILTER Type 5 (Next Object), GROUP_ORDER 1. A length-based walk reads Length 5 and
// runs off the end.
constexpr std::string_view kW22Between = "10 01  11 05  01 01";
// LOCATION_FILTER Type 2 {200,300}, GROUP_ORDER 1. A length-based walk reads Length 2, takes "80c8", and
// then reads delta 300 (an unknown type).
constexpr std::string_view kW22FirstMultiByte = "21 02 80c8 812c  01 01";
// EXPIRES 5, LOCATION_FILTER Type 4 {200,300,0,9}.
constexpr std::string_view kW22Last = "08 05  19 04 80c8 812c 00 09";
// FILL_PARAMETERS len 6 { LOCATION_FILTER Type 2 {200,300} }, then INCLUDE_PROPERTIES 1.
constexpr std::string_view kW22Nested = "23 06 21 02 80c8 812c  12 01";

TEST(ParameterWalkReaders, Wire22FilterBetweenOtherParameters) {
    ScopedWireDraft wire22(22);
    const auto parsed = block("03", kW22Between);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{number(0x10, 1), raw(0x21, "05"), number(0x22, 1)}));
    EXPECT_EQ(parsed.consumed, 7u);
    EXPECT_EQ(session(kW22Between, 3), complete_walk(3, 6));
}

TEST(ParameterWalkReaders, Wire22FilterFirstWithMultiByteFields) {
    ScopedWireDraft wire22(22);
    const auto parsed = block("02", kW22FirstMultiByte);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{raw(0x21, "02 80c8 812c"), number(0x22, 1)}));
    EXPECT_EQ(session(kW22FirstMultiByte, 2), complete_walk(2, 8));
}

TEST(ParameterWalkReaders, Wire22FilterLast) {
    ScopedWireDraft wire22(22);
    const auto parsed = block("02", kW22Last);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{number(0x08, 5), raw(0x21, "04 80c8 812c 00 09")}));
    EXPECT_EQ(parsed.consumed, 11u);
    EXPECT_EQ(session(kW22Last, 2), complete_walk(2, 10));
}

TEST(ParameterWalkReaders, Wire22FilterNestedInFillParameters) {
    ScopedWireDraft wire22(22);
    const auto parsed = block("02", kW22Nested);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.values, (std::vector{raw(0x23, "21 02 80c8 812c"), number(0x35, 1)}));
    EXPECT_EQ(session(kW22Nested, 2), complete_walk(2, 10));
}

TEST(ParameterWalkReaders, Wire22TruncatedFilterAborts) {
    ScopedWireDraft wire22(22);
    // Type 4 needs four fields; only two follow. A length-based walk would accept "80c8812c" as the value.
    EXPECT_FALSE(block("01", "21 04 80c8 812c").ok);
    EXPECT_EQ(aborted("21 04 80c8 812c", 1), broken_walk(1, 0));
    // The parameter before it was parsed.
    EXPECT_EQ(aborted("10 01  11 04 80c8 812c", 2), broken_walk(2, 1));
}

TEST(ParameterWalkReaders, Wire22UndefinedFilterTypeAborts) {
    ScopedWireDraft wire22(22);
    // Type 6 is undefined. A length-based walk would read six bytes as the value and succeed.
    EXPECT_FALSE(block("01", "21 06 010203040506").ok);
    const auto walk = aborted("21 06 010203040506", 1);
    EXPECT_EQ(walk, broken_walk(1, 0));
    EXPECT_FALSE(walk.unknown);  // a bad value, not an unknown parameter
}

TEST(ParameterWalkReaders, Wire22FilterOverflowAborts) {
    ScopedWireDraft wire22(22);
    // Type 3: StartGroup 2^64-1 + EndGroupDelta 1 overflows.
    EXPECT_FALSE(block("01", "21 03 ff ffffffffffffffff 00 01").ok);
    EXPECT_EQ(aborted("21 03 ff ffffffffffffffff 00 01", 1), broken_walk(1, 0));
}

// ---- the shared walk itself ------------------------------------------------------------------------

struct Seen {
    std::uint64_t type{0};
    ParameterValueKind kind{ParameterValueKind::Byte};
    Bytes value;
    Bytes payload;
    std::optional<ParameterWalkStatus> nested;
};

struct Walked {
    ParameterWalkResult result;
    std::vector<Seen> seen;
    std::size_t consumed{0};
};

ParameterVisitor collect(std::vector<Seen>& seen) {
    return [&seen](const WalkedParameter& parameter) {
        seen.push_back({parameter.type, parameter.kind, Bytes(parameter.value.begin(), parameter.value.end()),
                        Bytes(parameter.payload.begin(), parameter.payload.end()), parameter.nested});
    };
}

Walked walk(std::string_view params_hex, std::uint64_t count) {
    const auto body = h(params_hex);
    wire::Cursor cursor(body);
    Walked walked;
    walked.result = walk_message_parameters(cursor, count, collect(walked.seen));
    walked.consumed = cursor.offset();
    return walked;
}

Walked nested(std::string_view payload_hex) {
    const auto payload = h(payload_hex);
    Walked walked;
    walked.result = walk_nested_parameters(payload, collect(walked.seen));
    return walked;
}

std::vector<std::uint64_t> types(const Walked& walked) {
    std::vector<std::uint64_t> out;
    for (const auto& seen : walked.seen) out.push_back(seen.type);
    return out;
}

TEST(ParameterWalk, Wire21FilterIsLengthPrefixed) {
    const auto walked = walk(kW21Between, 3);
    EXPECT_EQ(walked.result.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(walked.result.visited, 3u);
    EXPECT_EQ(walked.consumed, 8u);
    ASSERT_EQ(types(walked), (std::vector<std::uint64_t>{0x10, 0x21, 0x22}));
    EXPECT_EQ(walked.seen[1].kind, ParameterValueKind::LengthPrefixed);
    EXPECT_EQ(walked.seen[1].value, h("02 0709"));
    EXPECT_EQ(walked.seen[1].payload, h("0709"));
    EXPECT_EQ(walked.seen[0].value, h("01"));
    EXPECT_EQ(walked.seen[0].payload, h("01"));
}

TEST(ParameterWalk, Wire22FilterIsTypeAndFieldsWithNoLength) {
    ScopedWireDraft wire22(22);
    const auto between = walk(kW22Between, 3);
    EXPECT_EQ(between.result.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(between.consumed, 6u);
    ASSERT_EQ(types(between), (std::vector<std::uint64_t>{0x10, 0x21, 0x22}));
    EXPECT_EQ(between.seen[1].kind, ParameterValueKind::LocationFilter);
    EXPECT_EQ(between.seen[1].value, h("05"));
    EXPECT_EQ(between.seen[1].payload, h("05"));

    const auto first = walk(kW22FirstMultiByte, 2);
    EXPECT_EQ(first.result.status, ParameterWalkStatus::Complete);
    ASSERT_EQ(types(first), (std::vector<std::uint64_t>{0x21, 0x22}));
    EXPECT_EQ(first.seen[0].value, h("02 80c8 812c"));

    const auto last = walk(kW22Last, 2);
    EXPECT_EQ(last.result.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(last.consumed, 10u);
    ASSERT_EQ(types(last), (std::vector<std::uint64_t>{0x08, 0x21}));
    EXPECT_EQ(last.seen[1].value, h("04 80c8 812c 00 09"));
}

// The same Type 2 {200,300} bytes framed by a Length: the draft 21 walk mis-reads them. If the walk still
// treated 0x21 as length-prefixed under wire 22, the test above would see this instead.
TEST(ParameterWalk, Wire21WalkMisFramesADraft22Filter) {
    const auto walked = walk(kW22FirstMultiByte, 2);
    EXPECT_EQ(walked.result.status, ParameterWalkStatus::UnknownType);
    EXPECT_EQ(walked.result.failed_type, std::optional<std::uint64_t>{0x21 + 300});
}

TEST(ParameterWalk, Wire22FilterNestedInFillParameters) {
    ScopedWireDraft wire22(22);
    const auto outer = walk(kW22Nested, 2);
    EXPECT_EQ(outer.result.status, ParameterWalkStatus::Complete);
    ASSERT_EQ(types(outer), (std::vector<std::uint64_t>{0x23, 0x35}));
    EXPECT_EQ(outer.seen[0].kind, ParameterValueKind::LengthPrefixed);
    EXPECT_EQ(outer.seen[0].payload, h("21 02 80c8 812c"));
    EXPECT_EQ(outer.seen[0].nested, std::optional{ParameterWalkStatus::Complete});
    EXPECT_EQ(outer.seen[1].nested, std::nullopt);

    const auto inner = nested("21 02 80c8 812c");
    EXPECT_EQ(inner.result.status, ParameterWalkStatus::Complete);
    ASSERT_EQ(types(inner), (std::vector<std::uint64_t>{0x21}));
    EXPECT_EQ(inner.seen[0].kind, ParameterValueKind::LocationFilter);
    EXPECT_EQ(inner.seen[0].value, h("02 80c8 812c"));

    // Nested filter first, then another nested parameter: Type 5 is a single byte.
    const auto followed = nested("21 05  01 01");
    EXPECT_EQ(followed.result.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(types(followed), (std::vector<std::uint64_t>{0x21, 0x22}));
    // Nested filter last, after FILL_TIMEOUT (0x0a).
    const auto trailing = nested("0a 05  17 04 80c8 812c 00 09");
    EXPECT_EQ(trailing.result.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(types(trailing), (std::vector<std::uint64_t>{0x0a, 0x21}));
}

TEST(ParameterWalk, Wire21FilterNestedInFillParameters) {
    const auto outer = walk(kW21Nested, 2);
    EXPECT_EQ(outer.result.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(outer.seen[0].nested, std::optional{ParameterWalkStatus::Complete});
    const auto inner = nested("21020709 0101");
    EXPECT_EQ(types(inner), (std::vector<std::uint64_t>{0x21, 0x22}));
    EXPECT_EQ(inner.seen[0].payload, h("0709"));
    // A draft 22 nested filter read as draft 21 mis-frames (delta 300 is unknown).
    EXPECT_EQ(nested("21 02 80c8 812c").result.status, ParameterWalkStatus::UnknownType);
}

TEST(ParameterWalk, NestedFailureDoesNotAbortTheOuterWalk) {
    ScopedWireDraft wire22(22);
    // FILL_PARAMETERS is length-bounded in both drafts, so a bad nested filter leaves the outer framing intact.
    const auto outer = walk("23 03 21 04 01  12 01", 2);
    EXPECT_EQ(outer.result.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(outer.seen[0].nested, std::optional{ParameterWalkStatus::Malformed});
    EXPECT_EQ(nested("21 04 01").result.failed_type, std::optional<std::uint64_t>{0x21});
    // A partial trailing delta is malformed.
    EXPECT_EQ(nested("10 01  80").result.status, ParameterWalkStatus::Malformed);
    EXPECT_EQ(nested("").result.status, ParameterWalkStatus::Complete);
}

TEST(ParameterWalk, Wire22TruncatedAndUndefinedFilterAbort) {
    ScopedWireDraft wire22(22);
    const auto truncated = walk("10 01  11 04 80c8 812c", 2);
    EXPECT_EQ(truncated.result.status, ParameterWalkStatus::Malformed);
    EXPECT_EQ(truncated.result.visited, 1u);
    EXPECT_EQ(truncated.result.failed_type, std::optional<std::uint64_t>{0x21});

    const auto undefined = walk("21 06 010203040506", 1);
    EXPECT_EQ(undefined.result.status, ParameterWalkStatus::Malformed);
    EXPECT_EQ(undefined.result.visited, 0u);
    EXPECT_EQ(undefined.result.failed_type, std::optional<std::uint64_t>{0x21});
}

TEST(ParameterWalk, UnknownTypeAndTypeOverflow) {
    const auto unknown = walk("10 01  07 00", 2);
    EXPECT_EQ(unknown.result.status, ParameterWalkStatus::UnknownType);
    EXPECT_EQ(unknown.result.visited, 1u);
    EXPECT_EQ(unknown.result.failed_type, std::optional<std::uint64_t>{0x17});

    const auto overflow = walk("10 01  ffffffffffffffffff", 2);
    EXPECT_EQ(overflow.result.status, ParameterWalkStatus::TypeOverflow);
    EXPECT_EQ(overflow.result.failed_type, std::nullopt);
}

TEST(ParameterWalk, ValuesAreDecodedForNumericAndLocationTypes) {
    const auto body = h("08 05  01 0302  17 01");  // EXPIRES 5, LARGEST_OBJECT {3,2}, PRIORITY 1
    wire::Cursor cursor(body);
    std::vector<WalkedParameter> seen;
    const auto result = walk_message_parameters(cursor, 3, [&](const WalkedParameter& p) { seen.push_back(p); });
    EXPECT_EQ(result.status, ParameterWalkStatus::Complete);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].kind, ParameterValueKind::Varint);
    EXPECT_EQ(seen[0].number, std::optional<std::uint64_t>{5});
    EXPECT_EQ(seen[1].kind, ParameterValueKind::Location);
    EXPECT_EQ(seen[1].location, (std::optional<std::pair<std::uint64_t, std::uint64_t>>{{3, 2}}));
    EXPECT_EQ(seen[2].kind, ParameterValueKind::Byte);
    EXPECT_EQ(seen[2].number, std::optional<std::uint64_t>{1});
}

}  // namespace
}  // namespace moq::interop::scenarios
