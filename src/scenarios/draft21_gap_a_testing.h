#pragma once

#include <cstddef>
#include <vector>

namespace moq::interop::scenarios {

// Test seam: the draft21_gap_a request builders are file-local; these wrappers expose their bytes so a
// test can pin them. Not used by production code.
enum class GapASubscribeFilter { OpenFromObject, BoundedObject, WholeGroup, None };

std::vector<std::byte> gap_a_subscribe_for_test(const std::vector<std::vector<std::byte>>& track_namespace,
                                                const std::vector<std::byte>& track_name, bool forwarding,
                                                GapASubscribeFilter filter);
std::vector<std::byte> gap_a_bounded_update_for_test();
std::vector<std::byte> gap_a_raise_start_update_for_test();
std::vector<std::byte> gap_a_fetch_for_test(const std::vector<std::vector<std::byte>>& track_namespace,
                                            const std::vector<std::byte>& track_name);

}  // namespace moq::interop::scenarios
