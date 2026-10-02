#pragma once

#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/draft18/messages.h"

namespace moq::interop::scenarios {

bool draft18_track_properties_valid(const wire::draft18::TrackProperties& properties);
bool draft18_request_error_valid(const wire::draft18::RequestErrorMessage& error,
                                 bool namespace_scoped = false);

enum class Draft18ResponseExpectation {
    FailedSubscriptionCleanup,
    FailedDiscoveryCleanup,
};

struct Draft18ResponseProbe {
    std::string requirement_id;
    std::string evaluator_id;
    Draft18ResponseExpectation expectation;
    bool namespace_scoped;
    RawProbeDefinition definition;
};

std::vector<Draft18ResponseProbe> draft18_response_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000});
std::optional<bool> evaluate_draft18_response_probe(
    const RawProbeTranscript& transcript, const Draft18ResponseProbe& profile);

}  // namespace moq::interop::scenarios
