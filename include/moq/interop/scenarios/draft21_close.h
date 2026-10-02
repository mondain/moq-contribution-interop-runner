#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <optional>
#include <string>
#include <vector>

namespace moq::interop::scenarios {

struct Draft21CloseProbe {
    std::string requirement_id;
    std::string evaluator_id;
    std::optional<std::uint64_t> expected_close;
    RawProbeDefinition definition;
};

std::vector<Draft21CloseProbe> draft21_close_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});

std::optional<bool> evaluate_draft21_close_probe(
    const RawProbeTranscript& transcript, const Draft21CloseProbe& probe);

}  // namespace moq::interop::scenarios
