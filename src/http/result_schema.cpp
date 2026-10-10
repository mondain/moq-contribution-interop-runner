#include "moq/interop/http/result_schema.h"

#include "detail.h"
#include "json.h"
#include "moq/interop/app/publisher_capabilities.h"
#include "moq/interop/app/unscored_probe_event_22.h"

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
    // An unreviewed row of a staged catalog (classification pending) carries exactly one NotRun outcome.
    if (!requirement.reviewed)
        return states.size() == 1 && states.front() == requirements::OutcomeState::NotRun
                   ? nlohmann::json("not_run") : nlohmann::json("error");
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
    // A scored row the run declared not applicable (publisher capability) is reported alone.
    if (states.size() == 1 && states.front() == requirements::OutcomeState::NotApplicable)
        return "not_applicable";
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
        // Why a scored row is not_applicable for this run, so the result explains itself.
        const auto reason = app::row_not_applicable_reason(
            static_cast<unsigned>(run.config.draft), requirement, run.config.publisher_capabilities);
        row["not_applicable_reason"] =
            (reason && row.at("outcome") == "not_applicable") ? nlohmann::json(*reason) : nlohmann::json(nullptr);
        row["observations"] = nlohmann::json::array();
        for (const auto state : states)
            row["observations"].push_back(outcome_name(state));
        row["evidence_sequences"] = evidence_sequences[requirement.id];
        // Only an unreviewed row (a staged catalog's) carries the flag, as in the catalog file.
        if (!requirement.reviewed) row["reviewed"] = false;
        rows.push_back(std::move(row));
    }
    // Scenarios the publisher's declaration ruled out; they were never started.
    nlohmann::json skipped = nlohmann::json::array();
    for (const auto& scenario : run.config.scenario_ids) {
        if (const auto reason = app::scenario_skip_reason(static_cast<unsigned>(run.config.draft), scenario,
                                                          run.config.publisher_capabilities))
            skipped.push_back({{"scenario_id", scenario}, {"reason", *reason}});
    }
    // Unscored draft 22 probes: their verdicts are evidence only, listed apart from the rows and never scored.
    nlohmann::json unscored = nlohmann::json::array();
    for (const auto& event : run.events) {
        if (event.kind != app::kUnscoredProbeVerdictEvent) continue;
        const auto fields = app::parse_unscored_probe_detail_22(event.detail);
        if (!fields) continue;
        unscored.push_back({{"scenario_id", event.scenario_id ? nlohmann::json(*event.scenario_id) : nlohmann::json(nullptr)},
                            {"verdict", fields->verdict},
                            {"reason", fields->reason}});
    }
    nlohmann::json result = {{"schema_version", 1},
            {"draft_source_sha256", catalog.source_sha256},
            {"run", detail::run_json(run)},
            {"publisher_capabilities", {{"fetch", run.config.publisher_capabilities.fetch}}},
            {"skipped_scenarios", std::move(skipped)},
            {"requirements", std::move(rows)},
            {"evidence", std::move(evidence)}};
    // Present only when the run recorded such a verdict, so a result without one (every draft 18/21 result) is
    // unchanged.
    if (!unscored.empty()) result["unscored_probes"] = std::move(unscored);
    // Present only for a staged (moq-lite) catalog, so every MoQ Transport result is unchanged.
    if (detail::staged_catalog(catalog)) {
        result["staged"] = true;
        result["staged_note"] = detail::kStagedNote;
    }
    return result;
}

}  // namespace moq::interop::http
