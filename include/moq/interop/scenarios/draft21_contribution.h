#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace moq::interop::scenarios {

// Draft-21 contribution profiles. Each probe binds one catalog requirement to
// one raw scenario and one evaluator. Several probes may share a scenario id
// (and therefore one transcript) when a single context exercises several
// requirements. All profiles need a configured track fixture because each
// context proves the session stayed usable with a request for that track.
struct Draft21ContributionProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft{21};
    RawProbeDefinition definition;
};

std::vector<Draft21ContributionProbe> draft21_contribution_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});

// Returns true or false only when the actual transcript proves the canonical
// stimulus and contains conclusive evidence; absent or ambiguous evidence
// yields no value so the requirement stays unscored.
std::optional<bool> evaluate_draft21_contribution_probe(
    const RawProbeTranscript& transcript,
    const Draft21ContributionProbe& probe);

}  // namespace moq::interop::scenarios
