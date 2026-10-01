#pragma once

#include "moq/interop/requirements/scoring.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/session/publisher_session.h"

#include <span>
#include <string>
#include <vector>

namespace moq::interop::requirements {

struct ScenarioContext {
    std::string scenario_id;
    // Completion includes a response timeout; evidence loss or harness failure
    // must leave this false so missing publisher behavior is not blamed on it.
    bool complete{false};
    // True only after the relay's stimulus was accepted by the transport.
    bool stimulus_delivered{false};
    std::vector<session::EvidenceEvent> evidence;
    bool webtransport{false};
};

std::vector<Outcome> evaluate_draft18(
    const RequirementCatalog& catalog,
    std::span<const ScenarioContext> scenarios);

std::vector<ExecutableBinding> draft18_executable_bindings();

}  // namespace moq::interop::requirements
