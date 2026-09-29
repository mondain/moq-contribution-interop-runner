#pragma once

#include "moq/interop/scenarios/engine.h"

#include <chrono>
#include <cstdint>

namespace moq::interop::scenarios {

ScenarioDefinition subscribe_to_publisher_track(
    wire::draft18::TrackNamespace track_namespace,
    wire::draft18::TrackName track_name,
    std::uint64_t request_id,
    std::chrono::milliseconds response_deadline,
    std::chrono::milliseconds duplicate_window);

}  // namespace moq::interop::scenarios
