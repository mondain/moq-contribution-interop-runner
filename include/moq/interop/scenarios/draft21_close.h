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

// `track_namespace`/`track_name` are the run's track for the probes that open with a SUBSCRIBE, FETCH or
// PUBLISH of it (validated: std::invalid_argument). On wire draft 22 (current_wire_draft()) the probes whose
// SUBSCRIBE or TRACK_STATUS names a track only because the message needs one (namespace (), track "x" on
// wire 21) name `request_namespace`/`request_name` instead when `request_name` is nonempty; both are
// ignored on wire 21. Names too large for a probe frame throw std::invalid_argument.
std::vector<Draft21CloseProbe> draft21_close_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}},
    std::vector<std::vector<std::byte>> request_namespace = {},
    std::vector<std::byte> request_name = {});

std::optional<bool> evaluate_draft21_close_probe(
    const RawProbeTranscript& transcript, const Draft21CloseProbe& probe);

}  // namespace moq::interop::scenarios
