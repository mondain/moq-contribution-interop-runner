#include "moq/interop/requirements/draft22_evaluators.h"

#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_lineage_data.h"

#include <algorithm>
#include <map>
#include <string_view>
#include <tuple>
#include <utility>

namespace moq::interop::requirements {
namespace {

using Reverse = std::multimap<std::string_view, std::string_view>;

// d21 -> d22 for a NamePair table ({d22, d21}, sorted by d22); a d21 id may have several d22 counterparts.
template <typename Table>
Reverse reverse(const Table& table) {
    Reverse map;
    for (const auto& pair : table) map.emplace(pair.d21, pair.d22);
    return map;
}

const Reverse& scenarios() {
    static const Reverse map = reverse(lineage_data::kSharedScenarios);
    return map;
}

const Reverse& evaluators() {
    static const Reverse map = reverse(lineage_data::kSharedEvaluators);
    return map;
}

std::vector<std::string_view> targets(const Reverse& map, std::string_view d21) {
    std::vector<std::string_view> out;
    const auto [first, last] = map.equal_range(d21);
    for (auto it = first; it != last; ++it) out.push_back(it->second);
    return out;
}

std::vector<std::string_view> rows(std::string_view d21) {
    std::vector<std::string_view> out;
    const auto& pairs = lineage_data::kSharedRows;  // sorted by (d21, d22)
    auto it = std::lower_bound(pairs.begin(), pairs.end(), d21,
                               [](const lineage_data::RowPair& pair, std::string_view value) { return pair.d21 < value; });
    for (; it != pairs.end() && it->d21 == d21; ++it) out.push_back(it->d22);
    return out;
}

bool own_row_ancestor(std::string_view d21) {
    return std::binary_search(kDraft21AncestorsOfOwnRows22.begin(), kDraft21AncestorsOfOwnRows22.end(), d21);
}

}  // namespace

Draft22Translation translate_draft21_bindings(std::span<const ExecutableBinding> draft21) {
    Draft22Translation result;
    std::map<std::tuple<std::string, std::string, std::string>, ExecutableBinding> shared;
    for (const auto& binding : draft21) {
        const auto row_targets = rows(binding.requirement_id);
        const auto scenario_targets = targets(scenarios(), binding.scenario_id);
        const auto evaluator_targets = targets(evaluators(), binding.evaluator_id);
        const char* reason = nullptr;
        if (row_targets.empty()) reason = own_row_ancestor(binding.requirement_id) ? "own_row" : "dropped_row";
        else if (scenario_targets.empty()) reason = "own_scenario";
        else if (evaluator_targets.empty()) reason = "own_evaluator";
        if (reason) {
            result.untranslated.push_back({binding, reason});
            continue;
        }
        for (const auto row : row_targets)
            for (const auto scenario : scenario_targets)
                for (const auto evaluator : evaluator_targets) {
                    ExecutableBinding translated{22, std::string(row), std::string(scenario), std::string(evaluator),
                                                 binding.evidence_kinds};
                    // A second draft 21 source of the same draft 22 binding (a one-to-many row) collapses onto
                    // the first; it is never emitted twice.
                    shared.try_emplace({translated.requirement_id, translated.scenario_id, translated.evaluator_id},
                                       std::move(translated));
                }
    }
    result.shared.reserve(shared.size());
    for (auto& [key, binding] : shared) result.shared.push_back(std::move(binding));
    return result;
}

std::vector<ExecutableBinding> draft22_shared_bindings() {
    return translate_draft21_bindings(draft21_executable_bindings()).shared;
}

std::vector<UntranslatedBinding> draft22_untranslated_bindings() {
    return translate_draft21_bindings(draft21_executable_bindings()).untranslated;
}

std::vector<ExecutableBinding> draft22_executable_bindings() {
    return draft22_shared_bindings();
}

}  // namespace moq::interop::requirements
