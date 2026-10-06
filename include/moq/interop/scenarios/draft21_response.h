#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

enum class Draft21ResponseExpectation {
    PermittedPublishUpdate,
    FailedSubscriptionCleanup,
    FailedDiscoveryCleanup,
};

struct Draft21ResponseProbe {
    std::string requirement_id;
    std::string evaluator_id;
    Draft21ResponseExpectation expectation;
    bool namespace_scoped;
    RawProbeDefinition definition;
};

// On wire draft 22 (current_wire_draft()) d21-failed-subscription-update-cleanup subscribes to the run's
// track (`track_namespace`, `track_name`) when one is given. On wire draft 21 both arguments are ignored.
std::vector<Draft21ResponseProbe> draft21_response_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {}, std::vector<std::byte> track_name = {});
std::optional<bool> evaluate_draft21_response_probe(
    const RawProbeTranscript& transcript, const Draft21ResponseProbe& profile);

}  // namespace moq::interop::scenarios
