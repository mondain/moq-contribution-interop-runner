#include "moq/interop/app/lineage_run.h"

#include "moq/interop/requirements/lineage_translate.h"

#include <map>
#include <stdexcept>
#include <string>

namespace moq::interop::app {

std::vector<requirements::Outcome> lineage_outcomes(const requirements::RequirementCatalog& draft22,
                                                    std::span<const requirements::Outcome> draft21_outcomes,
                                                    std::span<const requirements::Outcome> own_outcomes) {
    auto translated = requirements::translate_shared_outcomes(draft21_outcomes);
    std::map<std::string, std::size_t> by_id;
    for (std::size_t index = 0; index < translated.size(); ++index)
        by_id.emplace(translated[index].requirement_id, index);
    std::map<std::string, std::size_t> own_by_id;
    for (std::size_t index = 0; index < own_outcomes.size(); ++index) {
        const auto& id = own_outcomes[index].requirement_id;
        if (by_id.contains(id) || !own_by_id.emplace(id, index).second)
            throw std::logic_error("own draft 22 outcome duplicates another outcome for row " + id);
    }
    std::vector<requirements::Outcome> outcomes;
    outcomes.reserve(draft22.requirements.size());
    for (const auto& row : draft22.requirements) {
        const auto found = by_id.find(row.id);
        if (found != by_id.end()) {
            auto outcome = translated[found->second];
            // A draft 21 NotApplicable must not drop a draft 22 row that applies and is testable.
            if (outcome.state == requirements::OutcomeState::NotApplicable &&
                row.applicability == requirements::Applicability::Applicable &&
                row.testability != requirements::Testability::NotTestable)
                outcome.state = requirements::OutcomeState::NotRun;
            outcomes.push_back(outcome);
            by_id.erase(found);
            continue;
        }
        if (const auto own = own_by_id.find(row.id); own != own_by_id.end()) {
            outcomes.push_back(own_outcomes[own->second]);
            own_by_id.erase(own);
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
    for (const auto& outcome : own_outcomes)
        if (own_by_id.contains(outcome.requirement_id)) outcomes.push_back(outcome);
    return outcomes;
}

std::string stamp_scenario_id(DraftVersion wire_draft, std::string_view scenario_id) {
    if (wire_draft != DraftVersion::Draft22 || !scenario_id.starts_with("d21-")) return std::string(scenario_id);
    // Built once on first use (thread-safe static initialization); kSharedScenarios is sorted by d22.
    static const std::map<std::string_view, std::string_view, std::less<>> by_d21 = [] {
        std::map<std::string_view, std::string_view, std::less<>> reverse;
        for (const auto& pair : requirements::lineage_data::kSharedScenarios)
            if (!reverse.emplace(pair.d21, pair.d22).second)
                throw std::logic_error("draft 21 scenario " + std::string(pair.d21) +
                                       " implements more than one draft 22 scenario");
        return reverse;
    }();
    const auto found = by_d21.find(scenario_id);
    if (found == by_d21.end())
        throw std::logic_error("draft 21 scenario " + std::string(scenario_id) +
                               " implements no shared draft 22 scenario");
    return std::string(found->second);
}

}  // namespace moq::interop::app
