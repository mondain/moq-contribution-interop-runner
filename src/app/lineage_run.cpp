#include "moq/interop/app/lineage_run.h"

#include "moq/interop/requirements/lineage_translate.h"

#include <map>
#include <string>

namespace moq::interop::app {

std::vector<requirements::Outcome> lineage_outcomes(const requirements::RequirementCatalog& draft22,
                                                    std::span<const requirements::Outcome> draft21_outcomes) {
    auto translated = requirements::translate_shared_outcomes(draft21_outcomes);
    std::map<std::string, std::size_t> by_id;
    for (std::size_t index = 0; index < translated.size(); ++index)
        by_id.emplace(translated[index].requirement_id, index);
    std::vector<requirements::Outcome> outcomes;
    outcomes.reserve(draft22.requirements.size());
    for (const auto& row : draft22.requirements) {
        const auto found = by_id.find(row.id);
        if (found != by_id.end()) {
            outcomes.push_back(translated[found->second]);
            by_id.erase(found);
            continue;
        }
        auto state = requirements::OutcomeState::NotRun;
        if (row.applicability != requirements::Applicability::Applicable)
            state = requirements::OutcomeState::NotApplicable;
        else if (row.testability == requirements::Testability::NotTestable)
            state = requirements::OutcomeState::NotTestable;
        outcomes.push_back({row.id, state});
    }
    for (const auto& outcome : translated)
        if (by_id.contains(outcome.requirement_id)) outcomes.push_back(outcome);
    return outcomes;
}

requirements::ScoreSummary score_lineage(const requirements::RequirementCatalog& draft22,
                                         std::span<const requirements::Outcome> outcomes) {
    if (draft22.complete) return requirements::score(draft22, outcomes);
    auto scored = draft22;
    scored.complete = true;
    auto summary = requirements::score(scored, outcomes);
    if (summary.verdict == requirements::RunVerdict::Pass) summary.verdict = requirements::RunVerdict::Incomplete;
    return summary;
}

}  // namespace moq::interop::app
