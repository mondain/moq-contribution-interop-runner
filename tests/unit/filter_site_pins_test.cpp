#include "draft21_contribution_filter_testing.h"
#include "draft21_gap_a_testing.h"
#include "inline_filter_sites_testing.h"

#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/location_filter_param.h"

#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace moq::interop::scenarios {
namespace {

namespace d21c = ::moq::interop::scenarios::d21c;

using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;

// Fixture: namespace ["n"], track name "x". Track encoding T = 01 (1 field) 01 6e (len 1, 'n') 01 78.
const Namespace kNamespace{Bytes{std::byte{'n'}}};
const Bytes kName{std::byte{'x'}};

std::string hex(const Bytes& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) {
        out.push_back(digits[static_cast<unsigned>(b) >> 4u]);
        out.push_back(digits[static_cast<unsigned>(b) & 15u]);
    }
    return out;
}

// Every expected string below is computed by hand from the draft 21 section 9.20.10 layout:
//   frame = vi(type) + u16 length + body; params = vi(count) then per parameter vi(delta from the previous
//   type) + value. FORWARD is 0x10 (value one vi64). LOCATION_FILTER is 0x21: delta, then vi(len) and
//   the vi64 fields (wire 21) or Type plus the fields with no length (wire 22).
// For the fixed gap_a values ({7,9}, {7,0,0}, {7,9,0,9}) the field count equals the draft 22 Type value
// (Absolute 2, AbsoluteBounded 3, AbsoluteRange 4) and every value is a single byte, so wire 21 (length
// byte) and wire 22 (Type byte) coincide. The wire 22 pin test below therefore only guards framing; the
// discriminating cases are the location_filter seam tests, whose inputs make the two drafts differ.
struct Pins {
    // SUBSCRIBE (frame type 03), Request ID 1: body = 01 | T | count | params.
    const char* open_fwd1 = "03000d01" "01016e0178" "02" "1001" "11020709";
    const char* bounded_fwd0 = "03000f01" "01016e0178" "02" "1000" "1104" "07090009";
    const char* whole_fwd1 = "03000e01" "01016e0178" "02" "1001" "1103" "070000";
    const char* none_fwd1 = "03000901" "01016e0178" "01" "1001";
    const char* none_fwd0 = "03000901" "01016e0178" "01" "1000";
    // REQUEST_UPDATE (frame type 02), Request ID 3.
    const char* bounded_update = "02000a03" "02" "1001" "1104" "07090009";
    const char* raise_start = "02000603" "01" "21" "02" "070a";
    // FETCH (frame type 16), Request ID 5: body = 05 | T | 01 | 21 04 07 09 00 09.
    const char* fetch = "16000d05" "01016e0178" "01" "21" "04" "07090009";
};

TEST(FilterSitePins, Draft21BytesOfEveryGapABuildSite) {
    const Pins p;
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, true, GapASubscribeFilter::OpenFromObject)), p.open_fwd1);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, false, GapASubscribeFilter::BoundedObject)), p.bounded_fwd0);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, true, GapASubscribeFilter::WholeGroup)), p.whole_fwd1);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, true, GapASubscribeFilter::None)), p.none_fwd1);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, false, GapASubscribeFilter::None)), p.none_fwd0);
    EXPECT_EQ(hex(gap_a_bounded_update_for_test()), p.bounded_update);
    EXPECT_EQ(hex(gap_a_raise_start_update_for_test()), p.raise_start);
    EXPECT_EQ(hex(gap_a_fetch_for_test(kNamespace, kName)), p.fetch);
}

TEST(FilterSitePins, Draft22CarriesTypeAndFieldsWithoutLength) {
    ScopedWireDraft wire22(22);
    const Pins p;
    // Draft 22: delta byte 0x11 (0x21 - 0x10) then Type (2/3/4) then the fields, no length byte.
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, true, GapASubscribeFilter::OpenFromObject)), p.open_fwd1);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, false, GapASubscribeFilter::BoundedObject)), p.bounded_fwd0);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, true, GapASubscribeFilter::WholeGroup)), p.whole_fwd1);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, true, GapASubscribeFilter::None)), p.none_fwd1);
    EXPECT_EQ(hex(gap_a_subscribe_for_test(kNamespace, kName, false, GapASubscribeFilter::None)), p.none_fwd0);
    // bounded_update: ... 0x10 01, 0x21-0x10 = 0x11, Type 4, 7, 9, 0, 9.
    EXPECT_EQ(hex(gap_a_bounded_update_for_test()), p.bounded_update);
    // raise_start: delta 0x21, Type 2 (Absolute), 7, 10.
    EXPECT_EQ(hex(gap_a_raise_start_update_for_test()), p.raise_start);
    EXPECT_EQ(hex(gap_a_fetch_for_test(kNamespace, kName)), p.fetch);
}

// Discriminating cases. vi64 widths: 0..127 one byte; 200 = 0x00c8 -> 80 c8; 300 = 0x012c -> 81 2c.
// Delta from previous 0x10 to 0x21 is 0x11; from previous 0 it is 0x21.
struct Case {
    std::uint64_t previous;
    std::vector<std::uint64_t> fields;
    const char* wire21;
    const char* wire22;
};

const Case kCases[] = {
    // {0,0}: wire 21 = 21, len 02, 00 00. Wire 22 NextObject = 21, Type 05 and nothing else.
    {0, {0, 0}, "21020000", "2105"},
    // {200,300}: wire 21 = 11, len 04 (four value bytes), 80c8 812c. Wire 22 Absolute = 11, Type 02, 80c8 812c.
    {0x10, {200, 300}, "1104" "80c8812c", "1102" "80c8812c"},
    // {200,300,0,9}: wire 21 len 06 (80c8 812c 00 09). Wire 22 AbsoluteRange Type 04.
    {0x10, {200, 300, 0, 9}, "1106" "80c8812c0009", "1104" "80c8812c0009"},
    // {200,0,0}: values 80c8 00 00 are four bytes, so wire 21 len is 04 while wire 22 AbsoluteBounded is Type 03.
    {0x10, {200, 0, 0}, "1104" "80c80000", "1103" "80c80000"},
};

TEST(FilterSitePins, LocationFilterSeamDiffersBetweenDrafts) {
    for (const auto& c : kCases) {
        {
            ScopedWireDraft wire21(21);
            EXPECT_EQ(hex(gap_a_location_filter_for_test(c.previous, c.fields)), c.wire21);
        }
        {
            ScopedWireDraft wire22(22);
            EXPECT_EQ(hex(gap_a_location_filter_for_test(c.previous, c.fields)), c.wire22);
        }
        EXPECT_STRNE(c.wire21, c.wire22);
    }
}

// ---- contribution scenario sites (d21c) ------------------------------------------------------------
// Each seam returns encode_params({parameter}): vi(delta from 0) then the value, so a LOCATION_FILTER
// (0x21) starts "21". Wire 21: vi(len) + vi64 fields. Wire 22: Type + fields, no length.
// vi64 widths: 0..127 one byte; 200 = 80c8; 300 = 812c; 1000000 = cf4240 (three bytes).

TEST(FilterSitePins, Draft21BytesOfEveryContributionSite) {
    // objects: start {7,9}; range {7,9,0,9}.
    EXPECT_EQ(hex(d21c::objects_start_filter_for_test()), "21" "02" "0709");
    EXPECT_EQ(hex(d21c::objects_target_range_filter_for_test()), "21" "04" "07090009");
    // residual_subscription: location_range and bounded_filter (= range with End Group delta 0).
    EXPECT_EQ(hex(d21c::residual_location_range_for_test(0, 0, 1, 1)), "21" "04" "00000101");
    EXPECT_EQ(hex(d21c::residual_location_range_for_test(200, 300, 0, 9)), "21" "06" "80c8812c0009");
    EXPECT_EQ(hex(d21c::residual_bounded_filter_for_test(0, 1, 1)), "21" "04" "00010001");
    EXPECT_EQ(hex(d21c::residual_bounded_filter_for_test(200, 1, 9)), "21" "05" "80c8010009");
    // fill_whole_track: FILL_PARAMETERS 0x23, length 2, nested "21 00" (delta 0x21, zero-length filter).
    EXPECT_EQ(hex(d21c::residual_fill_whole_track_for_test()), "23" "02" "2100");
    // d21b done_without_streams: start group 1000000, object 0.
    EXPECT_EQ(hex(d21c::d21b_future_start_filter_for_test()), "21" "04" "cf4240" "00");
    // session fetch_range_spec: start group 2^62, object 0.
    EXPECT_EQ(hex(d21c::session_far_start_filter_for_test()), "21" "0a" "ff" "4000000000000000" "00");
    // residual_token: whole group {0,0,0}.
    EXPECT_EQ(hex(d21c::token_whole_group_filter_for_test()), "21" "03" "000000");
}

// Wire 22. Sites whose values are fixed AND coincide with wire 21 (field count == Type value, one-byte
// values) are pinned for framing only and prove nothing about routing; Task 6's static source guard covers
// them: objects start {7,9}, objects range {7,9,0,9}, residual {0,0,1,1}/{0,1,0,1}, token {0,0,0}, and
// fill_whole_track (the nested None filter "21 00" is identical in both drafts). The next test's cases
// differ from wire 21 and so do prove routing.
TEST(FilterSitePins, Draft22ContributionSitesCoincidingWithDraft21) {
    ScopedWireDraft wire22(22);
    EXPECT_EQ(hex(d21c::objects_start_filter_for_test()), "21" "02" "0709");
    EXPECT_EQ(hex(d21c::objects_target_range_filter_for_test()), "21" "04" "07090009");
    EXPECT_EQ(hex(d21c::residual_location_range_for_test(0, 0, 1, 1)), "21" "04" "00000101");
    EXPECT_EQ(hex(d21c::residual_bounded_filter_for_test(0, 1, 1)), "21" "04" "00010001");
    EXPECT_EQ(hex(d21c::token_whole_group_filter_for_test()), "21" "03" "000000");
    // fill_whole_track: FILL_PARAMETERS 0x23, length 2, nested delta 0x21 then Type None (00).
    EXPECT_EQ(hex(d21c::residual_fill_whole_track_for_test()), "23" "02" "2100");
}

TEST(FilterSitePins, Draft22ContributionSitesDifferFromDraft21) {
    ScopedWireDraft wire22(22);
    // Wire 21 lengths were 06 / 05; wire 22 carries the Type (4 AbsoluteRange) with no length.
    EXPECT_EQ(hex(d21c::residual_location_range_for_test(200, 300, 0, 9)), "21" "04" "80c8812c0009");
    EXPECT_EQ(hex(d21c::residual_bounded_filter_for_test(200, 1, 9)), "21" "04" "80c8010009");
    // d21b: wire 21 "2104cf424000"; wire 22 Absolute (Type 2) then cf4240 00.
    EXPECT_EQ(hex(d21c::d21b_future_start_filter_for_test()), "21" "02" "cf4240" "00");
    // session: wire 21 length 0a; wire 22 Absolute (Type 2), the nine-byte vi64, then 00.
    EXPECT_EQ(hex(d21c::session_far_start_filter_for_test()), "21" "02" "ff" "4000000000000000" "00");
}


// ---- inline FETCH / SUBSCRIBE sites ------------------------------------------------------------------
// FETCH (frame type 16) body = request id | T | parameter count | parameters. T = 01 01 6e 01 78.

const char* const kFetchProbeWire21 = "16" "0014" "01" "01016e0178" "01" "21" "0b" "0000" "ff" "ffffffffffffffff";
const char* const kFetchProbeWire22 = "16" "0014" "01" "01016e0178" "01" "21" "03" "0000" "ff" "ffffffffffffffff";
const char* const kFetchFirstObject = "16" "000d" "01" "01016e0178" "01" "21" "04" "07090009";
const char* const kGroupOrderDefault = "16" "000d" "01" "01016e0178" "01" "21" "04" "07000209";
const char* const kGroupOrderAscending = "16" "000f" "01" "01016e0178" "02" "21" "04" "07000209" "0101";
const char* const kGroupOrderDescending = "16" "000f" "01" "01016e0178" "02" "21" "04" "07000209" "0102";
const char* const kImmutableRepeat = "16" "000f" "05" "01016e0178" "02" "21" "04" "07090009" "1401";
// SUBSCRIBE: FORWARD=1 (10 01) then the filter (delta 0x11): 02 10 01 11 02 07 09.
const char* const kObjectRepeat = "03" "000d" "01" "01" "016e" "0178" "02" "1001" "11" "02" "0709";

TEST(FilterSitePins, Draft21BytesOfEveryInlineSite) {
    EXPECT_EQ(hex(fetch_probe_fetch_for_test(kNamespace, kName)), kFetchProbeWire21);
    EXPECT_EQ(hex(fetch_response_fetch_for_test(kNamespace, kName)), kFetchProbeWire21);
    EXPECT_EQ(hex(fetch_first_object_fetch_for_test(kNamespace, kName)), kFetchFirstObject);
    EXPECT_EQ(hex(fetch_group_order_fetch_for_test(kNamespace, kName, false, false)), kGroupOrderDefault);
    EXPECT_EQ(hex(fetch_group_order_fetch_for_test(kNamespace, kName, true, false)), kGroupOrderAscending);
    EXPECT_EQ(hex(fetch_group_order_fetch_for_test(kNamespace, kName, true, true)), kGroupOrderDescending);
    EXPECT_EQ(hex(immutable_repeat_fetch_for_test(kNamespace, kName, 5)), kImmutableRepeat);
    EXPECT_EQ(hex(object_repeat_subscribe_for_test(kNamespace, kName)), kObjectRepeat);
}

// fetch_probe and fetch_response carry {0,0,u64max}: the nine-byte maximum makes the wire 21 length (0b)
// differ from the wire 22 AbsoluteBounded Type (03), so this pair proves routing.
TEST(FilterSitePins, Draft22InlineSiteWhereDraftsDiffer) {
    ScopedWireDraft wire22(22);
    EXPECT_EQ(hex(fetch_probe_fetch_for_test(kNamespace, kName)), kFetchProbeWire22);
    EXPECT_EQ(hex(fetch_response_fetch_for_test(kNamespace, kName)), kFetchProbeWire22);
    EXPECT_STRNE(kFetchProbeWire21, kFetchProbeWire22);
}

// fetch_first_object {7,9,0,9}, fetch_group_order {7,0,2,9}, immutable_repeat {7,9,0,9} and object_repeat
// {7,9} have fixed values whose field count equals the draft 22 Type (4, 4, 4, 2) with one-byte varints, so
// wire 22 bytes equal wire 21 bytes. This only guards framing; it does not prove routing, which Task 6's
// static source guard covers.
TEST(FilterSitePins, Draft22InlineSitesCoincidingWithDraft21) {
    ScopedWireDraft wire22(22);
    EXPECT_EQ(hex(fetch_first_object_fetch_for_test(kNamespace, kName)), kFetchFirstObject);
    EXPECT_EQ(hex(fetch_group_order_fetch_for_test(kNamespace, kName, false, false)), kGroupOrderDefault);
    EXPECT_EQ(hex(fetch_group_order_fetch_for_test(kNamespace, kName, true, false)), kGroupOrderAscending);
    EXPECT_EQ(hex(fetch_group_order_fetch_for_test(kNamespace, kName, true, true)), kGroupOrderDescending);
    EXPECT_EQ(hex(immutable_repeat_fetch_for_test(kNamespace, kName, 5)), kImmutableRepeat);
    EXPECT_EQ(hex(object_repeat_subscribe_for_test(kNamespace, kName)), kObjectRepeat);
}

// ---- draft 21 close probes: the End Group overflow filter {u64max,0,1} ---------------------------------
Bytes close_probe_write(const char* scenario_id) {
    for (const auto& probe : draft21_close_probes())
        if (probe.definition.id == scenario_id) return probe.definition.writes.front().bytes;
    ADD_FAILURE() << "missing close probe " << scenario_id;
    return {};
}

TEST(FilterSitePins, Draft21BytesOfTheOverflowFilterSites) {
    // Top-level: 21, len 0b, ff + eight ff (u64max), 00, 01.
    EXPECT_EQ(hex(close_probe_write("d21-location-filter-end-group-overflow")),
              "03" "0012" "01000178" "01" "21" "0b" "ffffffffffffffffff" "0001");
    // Nested in FILL_PARAMETERS: 23, len 0d, then the same filter.
    EXPECT_EQ(hex(close_probe_write("d21-fill-location-filter-end-group-overflow")),
              "03" "0014" "01000178" "01" "23" "0d" "21" "0b" "ffffffffffffffffff" "0001");
}

// Draft 22 cannot represent StartGroup u64max + EndGroupDelta 1 (the builder throws std::logic_error), so
// under wire 22 the two overflow probes are omitted instead of emitting a wrong byte or failing the whole
// list. These scenario ids stay `own` and are never run as shared; Task 10 supplies their replacements.
TEST(FilterSitePins, Draft22OmitsTheOverflowFilterProbes) {
    const auto count21 = draft21_close_probes().size();
    ScopedWireDraft wire22(22);
    const auto probes = draft21_close_probes();
    EXPECT_EQ(probes.size() + 2, count21);
    for (const auto& probe : probes) {
        EXPECT_NE(probe.definition.id, "d21-location-filter-end-group-overflow");
        EXPECT_NE(probe.definition.id, "d21-fill-location-filter-end-group-overflow");
    }
    EXPECT_THROW(filter_param_value({std::numeric_limits<std::uint64_t>::max(), 0, 1}), std::logic_error);
}

}  // namespace
}  // namespace moq::interop::scenarios
