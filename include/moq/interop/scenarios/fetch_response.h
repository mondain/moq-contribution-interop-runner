#pragma once
#include "moq/interop/scenarios/raw_probe.h"
namespace moq::interop::scenarios {
struct FetchResponseProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    RawProbeDefinition definition;
};
std::vector<FetchResponseProbe>
draft21_fetch_response_probes(std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
                              std::vector<std::vector<std::byte>> track_namespace = {},
                              std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool> evaluate_fetch_response_probe(const RawProbeTranscript& transcript,
                                                  const FetchResponseProbe& profile);
} // namespace moq::interop::scenarios
