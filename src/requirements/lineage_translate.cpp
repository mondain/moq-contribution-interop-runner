#include "moq/interop/requirements/lineage_translate.h"

#include "moq/interop/requirements/draft22_lineage_data.h"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>

namespace moq::interop::requirements {
namespace {

int severity(OutcomeState state) {
    switch (state) {
        case OutcomeState::Fail: return 4;
        case OutcomeState::NotRun: return 3;
        case OutcomeState::Pass: return 2;
        case OutcomeState::NotTestable: return 1;
        case OutcomeState::NotApplicable: return 0;
    }
    return 0;
}

}  // namespace

std::vector<Outcome> translate_shared_outcomes(std::span<const Outcome> draft21_outcomes) {
    std::vector<Outcome> result;
    std::map<std::string, std::size_t> index;
    const auto& pairs = lineage_data::kSharedRows;  // sorted by (d21, d22)
    for (const auto& outcome : draft21_outcomes) {
        const std::string_view id = outcome.requirement_id;
        auto it = std::lower_bound(pairs.begin(), pairs.end(), id,
                                   [](const lineage_data::RowPair& pair, std::string_view value) {
                                       return pair.d21 < value;
                                   });
        for (; it != pairs.end() && it->d21 == id; ++it) {
            const std::string target(it->d22);
            const auto found = index.find(target);
            if (found == index.end()) {
                index.emplace(target, result.size());
                result.push_back({target, outcome.state});
            } else if (severity(outcome.state) > severity(result[found->second].state)) {
                result[found->second].state = outcome.state;
            }
        }
    }
    return result;
}

}  // namespace moq::interop::requirements
