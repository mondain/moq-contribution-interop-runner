#pragma once

#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/app/types.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/scoring.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::app {

// Why a scenario cannot run against a publisher with these capabilities, or nothing when
// it can. The text is recorded as evidence and shown in reports.
inline std::optional<std::string> scenario_skip_reason(unsigned draft, std::string_view scenario,
                                                       const PublisherCapabilities& capabilities) {
    if (!capabilities.fetch && scenario_requires_fetch(draft, scenario))
        return "publisher declared no FETCH support";
    return std::nullopt;
}

// Capability name for the typed API error when a run selects only skipped scenarios.
inline std::optional<std::string_view> missing_capability(unsigned draft, std::string_view scenario,
                                                          const PublisherCapabilities& capabilities) {
    if (!scenario_skip_reason(draft, scenario, capabilities)) return std::nullopt;
    return scenario_required_capability(draft, scenario);
}

// A scored catalog row is reported not_applicable for a run when the publisher declared a
// capability absent and EVERY scenario the row names needs that capability. A row that
// also names a scenario that does not need it keeps the ordinary "every named scenario
// must run" semantics and is never dropped. Rows that name no scenario are untouched.
inline std::optional<std::string> row_not_applicable_reason(
    unsigned draft, const requirements::Requirement& row, const PublisherCapabilities& capabilities) {
    if (row.applicability != requirements::Applicability::Applicable ||
        row.testability != requirements::Testability::Testable || row.scenarios.empty())
        return std::nullopt;
    if (!std::all_of(row.scenarios.begin(), row.scenarios.end(), [&](const auto& scenario) {
            return scenario_skip_reason(draft, scenario, capabilities).has_value();
        }))
        return std::nullopt;
    std::string reason = "publisher declared no FETCH support; every scenario this row names requires FETCH (";
    for (std::size_t index = 0; index < row.scenarios.size(); ++index) {
        if (index != 0) reason += ", ";
        reason += row.scenarios[index];
    }
    return reason + ")";
}

// Turns the NotRun outcome of every such row into NotApplicable. A row that somehow has
// evidence (Pass or Fail) is left alone: a declaration never hides an observation.
inline void apply_publisher_capabilities(unsigned draft, const requirements::RequirementCatalog& catalog,
                                         const PublisherCapabilities& capabilities,
                                         std::vector<requirements::Outcome>& outcomes) {
    std::map<std::string, const requirements::Requirement*> rows;
    for (const auto& row : catalog.requirements)
        if (row_not_applicable_reason(draft, row, capabilities)) rows.emplace(row.id, &row);
    if (rows.empty()) return;
    for (auto& outcome : outcomes)
        if (outcome.state == requirements::OutcomeState::NotRun && rows.contains(outcome.requirement_id))
            outcome.state = requirements::OutcomeState::NotApplicable;
}

}  // namespace moq::interop::app
