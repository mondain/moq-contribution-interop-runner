#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {

// Draft-18 probes that close completeness-audit gaps for required rows. Each
// profile names one catalog requirement, one scenario and one evaluator.
struct Draft18GapAProbe {
    std::string requirement_id;
    std::string evaluator_id;
    // Scored only on native QUIC (WebTransport carries no SETUP path/authority).
    bool native_only;
    RawProbeDefinition definition;
};

std::vector<Draft18GapAProbe> draft18_gap_a_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
bool draft18_gap_a_scenario(std::string_view scenario_id);
bool draft18_gap_a_requires_track(std::string_view scenario_id);
bool draft18_gap_a_native_only(std::string_view scenario_id);
// Returns no value unless the stimulus was fully delivered and the evidence
// supports a verdict. webtransport suppresses native-only requirements.
std::optional<bool> evaluate_draft18_gap_a_probe(
    const RawProbeTranscript& transcript, const Draft18GapAProbe& profile,
    bool webtransport = false);

}  // namespace moq::interop::scenarios
