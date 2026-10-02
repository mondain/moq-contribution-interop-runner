#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {

// Draft-18 publisher-contribution probes. Each probe is a raw-probe
// definition plus an observation function over the resulting transcript.
// An observation returns true for a pass, false for a definite violation and
// no value when the evidence cannot establish either (the row stays NOT_RUN).
struct Draft18ContributionProbe {
    std::string requirement_id;
    std::string evaluator_id;
    RawProbeDefinition definition;
    // True when the first write names the configured track fixture.
    bool requires_track{false};
    std::function<std::optional<bool>(const RawProbeTranscript&, bool webtransport)> observe;
};

std::vector<Draft18ContributionProbe> draft18_contribution_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});

// The transcript carries the exact stimulus; the expected definition is
// rebuilt from the fixture recovered from it before any observation is made.
std::optional<bool> evaluate_draft18_contribution_probe(
    const RawProbeTranscript& transcript, const Draft18ContributionProbe& profile,
    bool webtransport = false);

// Query component the runner appends to the publisher's connection URI for a
// scenario, or an empty view (section 10.3.1.2).
std::string_view draft18_contribution_connection_query(std::string_view id);

// The scenario that names a moqt URI with an empty host (section 3.1.1). A
// publisher that refuses to connect is the expected, unscored outcome.
bool draft18_contribution_empty_host_scenario(std::string_view id);

bool draft18_contribution_scenario(std::string_view id);
bool draft18_contribution_requires_track(std::string_view id);
bool draft18_contribution_fixture_valid(
    const std::vector<std::vector<std::byte>>& track_namespace,
    const std::vector<std::byte>& track_name);

}  // namespace moq::interop::scenarios
