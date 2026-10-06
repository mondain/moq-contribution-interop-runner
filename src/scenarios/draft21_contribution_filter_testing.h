#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace moq::interop::scenarios::d21c {

// Test seams: the contribution scenarios' LOCATION_FILTER builders are file-local; these wrappers expose
// their bytes so a test can pin them. Every result is encode_params({parameter}): vi(type delta from 0)
// then the parameter value. Not used by production code.
std::vector<std::byte> objects_start_filter_for_test();
std::vector<std::byte> objects_target_range_filter_for_test();
std::vector<std::byte> residual_location_range_for_test(std::uint64_t start_group, std::uint64_t start_object,
                                                        std::uint64_t end_group_delta, std::uint64_t end_object);
std::vector<std::byte> residual_bounded_filter_for_test(std::uint64_t group, std::uint64_t first,
                                                        std::uint64_t last);
// FILL_PARAMETERS (0x23) holding the nested filter: vi(0x23) vi(len) then the nested parameters; the
// contents carry no Number of Parameters (count).
std::vector<std::byte> residual_fill_whole_track_for_test();
std::vector<std::byte> d21b_future_start_filter_for_test();
std::vector<std::byte> session_far_start_filter_for_test();
std::vector<std::byte> token_whole_group_filter_for_test();

}  // namespace moq::interop::scenarios::d21c
