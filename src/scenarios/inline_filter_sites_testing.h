#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace moq::interop::scenarios {

// Test seams: the inline FETCH and SUBSCRIBE builders in these scenario files are file-local; these wrappers
// expose their full frame bytes so a test can pin the embedded LOCATION_FILTER. Not used by production code.
using SiteBytes = std::vector<std::byte>;
using SiteNamespace = std::vector<SiteBytes>;

SiteBytes fetch_probe_fetch_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name);
SiteBytes fetch_response_fetch_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name);
SiteBytes fetch_first_object_fetch_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name);
SiteBytes fetch_group_order_fetch_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name,
                                           bool explicit_order, bool descending);
SiteBytes immutable_repeat_fetch_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name,
                                          std::uint64_t request_id);
SiteBytes object_repeat_subscribe_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name);

}  // namespace moq::interop::scenarios
