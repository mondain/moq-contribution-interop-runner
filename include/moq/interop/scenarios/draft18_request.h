#pragma once

#include "moq/interop/scenarios/request_probe.h"

#include <vector>

namespace moq::interop::scenarios {
std::vector<RequestProbeProfile> draft18_request_profiles(
    std::chrono::milliseconds deadline = std::chrono::milliseconds(1000));
}  // namespace moq::interop::scenarios
