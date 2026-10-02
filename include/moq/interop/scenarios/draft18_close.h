#pragma once
#include "moq/interop/scenarios/raw_probe.h"
#include <string_view>
namespace moq::interop::scenarios {
struct Draft18CloseProfile {
    std::string_view requirement_id;
    std::string_view scenario_id;
    std::string_view evaluator_id;
    std::optional<std::uint64_t> expected_close;
    bool webtransport_only{false};
    bool native_only{false};
};
std::span<const Draft18CloseProfile> draft18_close_profiles();
RawProbeDefinition draft18_close_probe(std::string_view scenario_id,
                                      std::chrono::milliseconds deadline);
}  // namespace moq::interop::scenarios
