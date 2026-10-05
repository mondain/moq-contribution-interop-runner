#include "moq/interop/app/own_scenario_dispatch_22.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

namespace moq::interop::app {
namespace {

// Production probes for the ids in kOwnScenarioTraits22 (Tasks 9-10 add them together).
const std::vector<OwnScenario22>& production_scenarios() {
    static const std::vector<OwnScenario22> table;
    return table;
}

// Production own draft 22 evaluators (Tasks 9-10).
const std::vector<OwnEvaluator22>& production_evaluators() {
    static const std::vector<OwnEvaluator22> table;
    return table;
}

std::optional<OwnEvaluator22> own_evaluator(std::string_view id) {
    if (auto registered = OwnScenarioRegistry22::instance().registered_evaluator(id)) return registered;
    for (const auto& entry : production_evaluators())
        if (entry.id == id) return entry;
    return std::nullopt;
}

}  // namespace

std::optional<scenarios::RawProbeDefinition> own_probe_22(std::string_view id, const RunConfig& execution) {
    auto scenario = OwnScenarioRegistry22::instance().registered_scenario(id);
    if (!scenario) {
        for (const auto& entry : production_scenarios())
            if (entry.traits.id == id) scenario = entry;
    }
    if (!scenario || !scenario->probe) return std::nullopt;
    return scenario->probe(execution);
}

std::vector<requirements::Outcome> evaluate_own_draft22(const requirements::RequirementCatalog& draft22,
                                                        std::span<const scenarios::RawProbeTranscript> transcripts) {
    using requirements::OutcomeState;
    std::vector<requirements::Outcome> outcomes;
    for (const auto& row : draft22.requirements) {
        if (row.applicability != requirements::Applicability::Applicable ||
            row.testability != requirements::Testability::Testable)
            continue;
        std::vector<OwnEvaluator22> evaluators;
        for (const auto& id : row.evaluators)
            if (auto evaluator = own_evaluator(id)) evaluators.push_back(std::move(*evaluator));
        if (evaluators.empty()) continue;
        auto state = OutcomeState::NotRun;
        std::map<std::string, std::size_t> contexts;
        std::map<std::string, std::size_t> successful;
        std::set<std::string, std::less<>> exercised;
        for (const auto& transcript : transcripts) {
            if (std::find(row.scenarios.begin(), row.scenarios.end(), transcript.scenario_id) == row.scenarios.end())
                continue;
            ++contexts[transcript.scenario_id];
            // Evidence cut at a recording limit is never scored (as for draft 21 rows).
            if (transcript.event_limit_reached) continue;
            bool verdict = false;
            for (const auto& evaluator : evaluators) {
                if (!evaluator.evaluate) continue;
                const auto result = evaluator.evaluate(transcript);
                if (!result) continue;
                if (!*result) {
                    state = OutcomeState::Fail;
                    break;
                }
                verdict = true;
                exercised.emplace(evaluator.id);
            }
            if (state == OutcomeState::Fail) break;
            if (verdict) ++successful[transcript.scenario_id];
        }
        const bool scenarios_settled = !row.scenarios.empty() &&
            std::all_of(row.scenarios.begin(), row.scenarios.end(), [&](const auto& scenario) {
                return contexts[scenario] == 1 && successful[scenario] == 1;
            });
        if (state != OutcomeState::Fail && scenarios_settled &&
            std::all_of(row.evaluators.begin(), row.evaluators.end(),
                        [&](const auto& evaluator) { return exercised.contains(evaluator); }))
            state = OutcomeState::Pass;
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

}  // namespace moq::interop::app
