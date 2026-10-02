#pragma once
#include "moq/interop/scenarios/raw_probe.h"
namespace moq::interop::scenarios {
struct RequestResponseProbe {
  std::string requirement_id;
  std::string evaluator_id;
  unsigned draft;
  RawProbeDefinition definition;
};
// Returns six profiles for a valid full track fixture. If only the namespace
// prefix is valid, returns the four discovery profiles; invalid prefixes throw.
std::vector<RequestResponseProbe> draft21_request_response_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool>
evaluate_request_response_probe(const RawProbeTranscript &transcript,
                                const RequestResponseProbe &profile);
} // namespace moq::interop::scenarios
