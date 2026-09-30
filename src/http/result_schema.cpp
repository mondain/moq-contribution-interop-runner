#include "moq/interop/http/result_schema.h"

#include "detail.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace moq::interop::http {
namespace {

const char* outcome_name(requirements::OutcomeState state) {
    switch (state) {
        case requirements::OutcomeState::Pass: return "pass";
        case requirements::OutcomeState::Fail: return "fail";
        case requirements::OutcomeState::NotRun: return "not_run";
        case requirements::OutcomeState::NotTestable: return "not_testable";
        case requirements::OutcomeState::NotApplicable: return "not_applicable";
    }
    return "unknown";
}

nlohmann::json aggregate(const requirements::Requirement& requirement,
                         const std::vector<requirements::OutcomeState>& states) {
    if (states.empty()) return nullptr;
    const bool eligible =
        requirement.applicability == requirements::Applicability::Applicable &&
        requirement.testability == requirements::Testability::Testable;
    if (!eligible) {
        const auto expected =
            requirement.applicability == requirements::Applicability::Applicable
                ? requirements::OutcomeState::NotTestable
                : requirements::OutcomeState::NotApplicable;
        return states.size() == 1 && states.front() == expected
                   ? nlohmann::json(outcome_name(expected))
                   : nlohmann::json("error");
    }
    const auto has = [&](requirements::OutcomeState state) {
        return std::find(states.begin(), states.end(), state) != states.end();
    };
    if (has(requirements::OutcomeState::NotTestable) ||
        has(requirements::OutcomeState::NotApplicable))
        return "error";
    if (has(requirements::OutcomeState::NotRun))
        return states.size() == 1 ? "not_run" : "error";
    if (has(requirements::OutcomeState::Fail)) return "fail";
    if (has(requirements::OutcomeState::Pass)) return "pass";
    return "error";
}

}  // namespace

nlohmann::json serialize_result(const storage::RunRecord& run,
                                const requirements::RequirementCatalog& catalog) {
    if (static_cast<unsigned>(run.config.draft) != catalog.draft)
        throw std::invalid_argument("run and catalog drafts differ");

    std::map<std::string, std::vector<requirements::OutcomeState>> observations;
    for (const auto& outcome : run.outcomes)
        observations[outcome.requirement_id].push_back(outcome.state);
    std::map<std::string, std::vector<std::uint64_t>> evidence_sequences;
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& event : run.events) {
        evidence.push_back(detail::event_json(event));
        if (event.requirement_id)
            evidence_sequences[*event.requirement_id].push_back(event.sequence);
    }

    nlohmann::json rows = nlohmann::json::array();
    for (const auto& requirement : catalog.requirements) {
        auto row = detail::requirement_json(requirement);
        row["weight"] = requirements::score_weight(requirement.strength);
        row["required"] = requirement.strength == requirements::Strength::Must ||
                          requirement.strength == requirements::Strength::MustNot;
        row["score_eligible"] =
            requirement.applicability == requirements::Applicability::Applicable &&
            requirement.testability == requirements::Testability::Testable;
        const auto& states = observations[requirement.id];
        row["outcome"] = aggregate(requirement, states);
        row["observations"] = nlohmann::json::array();
        for (const auto state : states)
            row["observations"].push_back(outcome_name(state));
        row["evidence_sequences"] = evidence_sequences[requirement.id];
        rows.push_back(std::move(row));
    }
    return {{"schema_version", 1},
            {"draft_source_sha256", catalog.source_sha256},
            {"run", detail::run_json(run)},
            {"requirements", std::move(rows)},
            {"evidence", std::move(evidence)}};
}

}  // namespace moq::interop::http
