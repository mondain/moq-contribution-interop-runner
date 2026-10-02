#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {
struct RequestGoawayProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    RawProbeDefinition definition;
    bool duplicate;
};

std::vector<RequestGoawayProbe> draft18_request_goaway_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000});
std::vector<RequestGoawayProbe> draft21_request_goaway_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000});
std::optional<bool> evaluate_request_goaway_probe(
    const RawProbeTranscript& transcript, const RequestGoawayProbe& profile);
}  // namespace moq::interop::scenarios
