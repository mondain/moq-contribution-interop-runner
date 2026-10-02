#pragma once
#include "moq/interop/scenarios/raw_probe.h"
namespace moq::interop::scenarios {
struct DiscoveryOverlapProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    RawProbeDefinition definition;
};
bool discovery_overlap_namespace_valid(const std::vector<std::vector<std::byte>>& track_namespace);
std::vector<DiscoveryOverlapProbe> draft18_discovery_overlap_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {});
std::vector<DiscoveryOverlapProbe> draft21_discovery_overlap_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {});
std::optional<bool> evaluate_discovery_overlap_probe(
    const RawProbeTranscript& transcript, const DiscoveryOverlapProbe& profile);
}
