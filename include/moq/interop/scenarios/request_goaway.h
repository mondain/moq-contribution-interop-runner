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
// On wire draft 22 (current_wire_draft()) the requests name the run's namespace (`track_namespace`) when one
// is given; on wire draft 21 it is ignored.
std::vector<RequestGoawayProbe> draft21_request_goaway_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {});
std::optional<bool> evaluate_request_goaway_probe(
    const RawProbeTranscript& transcript, const RequestGoawayProbe& profile);
}  // namespace moq::interop::scenarios
