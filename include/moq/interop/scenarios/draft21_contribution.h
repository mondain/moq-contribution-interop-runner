#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace moq::interop::scenarios {

// Draft-21 contribution profiles. Each probe binds one catalog requirement to
// one raw scenario and one evaluator. Several probes may share a scenario id
// (and therefore one transcript) when a single context exercises several
// requirements. All profiles need a configured track fixture because each
// context proves the session stayed usable with a request for that track.
// A credential for one Token Type the publisher under test is configured to
// understand (Section 8.9). The operator supplies it; the runner never invents one.
struct Draft21TokenCredential {
    std::uint64_t token_type{0};
    std::vector<std::byte> value;
};
// invalid: well-formed but not acceptable to the publisher (D21-8-9-MUST-270).
// expired: acceptable in form but already past its lifetime (D21-8-9-MUST-273).
// A scenario whose credential is absent sends nothing and stays unscored.
struct Draft21TokenCredentials {
    std::optional<Draft21TokenCredential> invalid;
    std::optional<Draft21TokenCredential> expired;
};

struct Draft21ContributionProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft{21};
    RawProbeDefinition definition;
};

std::vector<Draft21ContributionProbe> draft21_contribution_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}},
    // Credential the publisher's authorization policy is configured to refuse
    // (Section 8.9); empty selects the documented default contract value.
    std::string denied_token = {},
    // Operator-supplied invalid and expired credentials (D21-8-9-MUST-270, -273).
    Draft21TokenCredentials credentials = {});

// Rows whose named contribution scenarios are alternative ways to reach the
// precondition, so any one passing context settles them (D21-9-9-MUST-365:
// a subscription with no data streams, with or without datagram delivery).
bool draft21_contribution_scenarios_are_alternatives(const std::string& requirement_id);

// Returns true or false only when the actual transcript proves the canonical
// stimulus and contains conclusive evidence; absent or ambiguous evidence
// yields no value so the requirement stays unscored.
std::optional<bool> evaluate_draft21_contribution_probe(
    const RawProbeTranscript& transcript,
    const Draft21ContributionProbe& probe);

}  // namespace moq::interop::scenarios
