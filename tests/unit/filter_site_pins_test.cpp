#include "draft21_gap_a_testing.h"

#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace moq::interop::scenarios {
namespace {

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
// For {7,9}: wire 21 is 02 07 09; wire 22 Absolute is Type 02 then 07 09: the same bytes, because the field
// count equals the Type value for Absolute (2), AbsoluteBounded (3) and AbsoluteRange (4). So these builders'
// wire 22 output equals their wire 21 output; the wire 22 test asserts that, and that a length byte of a
// different value would not be there (see the type-vs-length note in the task report).
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
    // bounded_update: ... 0x10 01, 0x21-0x10 = 0x11, Type 4, 7, 9, 0, 9.
    EXPECT_EQ(hex(gap_a_bounded_update_for_test()), p.bounded_update);
    // raise_start: delta 0x21, Type 2 (Absolute), 7, 10.
    EXPECT_EQ(hex(gap_a_raise_start_update_for_test()), p.raise_start);
    EXPECT_EQ(hex(gap_a_fetch_for_test(kNamespace, kName)), p.fetch);
}

}  // namespace
}  // namespace moq::interop::scenarios
