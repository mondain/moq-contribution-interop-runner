#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {
struct RangeFilterProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    RawProbeDefinition definition;
};
std::vector<RangeFilterProbe> draft21_range_filter_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool> evaluate_range_filter_probe(
    const RawProbeTranscript& transcript, const RangeFilterProbe& probe);
}  // namespace moq::interop::scenarios
