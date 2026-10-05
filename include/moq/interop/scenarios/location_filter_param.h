#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace moq::interop::scenarios {

// The draft 21 field list a scenario writes today: {} (none), {sg,so}, {sg,so,end_group_delta},
// {sg,so,end_group_delta,end_object}.
using FilterFields = std::vector<std::uint64_t>;

// The bytes that follow a LOCATION_FILTER (0x21) parameter's type delta. Wire draft 21 (and 18): vi(len)
// followed by each field as a vi64, exactly the bytes scenarios wrote before. Wire draft 22: the draft 22
// Location Filter Type plus its fields, with no length prefix. Mapping under wire 22: {} -> None;
// {0,0} -> NextObject; {sg,so} -> Absolute; 3 fields -> AbsoluteBounded; 4 fields -> AbsoluteRange; any
// other size -> std::logic_error. Also throws std::logic_error when the draft 22 encoder refuses the fields
// (StartGroup + EndGroupDelta overflow).
std::vector<std::byte> filter_param_value(const FilterFields& fields);

// The same bytes for a nested parameter value (FILL_PARAMETERS and similar); a separate name for readability.
std::vector<std::byte> nested_filter_param_value(const FilterFields& fields);

}  // namespace moq::interop::scenarios
