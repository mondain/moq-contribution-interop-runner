#pragma once

#include "moq/interop/requirements/draft22_lineage_data.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>

namespace moq::interop::app {

inline std::span<const std::string_view> shared_scenario_ids_22() {
    return requirements::lineage_data::kSharedScenarioIds22;
}

// The draft 21 scenario that implements a shared draft 22 scenario; nullopt for own or unknown ids.
inline std::optional<std::string_view> implementation_scenario_id(std::string_view d22_id) {
    const auto& pairs = requirements::lineage_data::kSharedScenarios;
    const auto it = std::lower_bound(pairs.begin(), pairs.end(), d22_id,
                                     [](const auto& pair, std::string_view id) { return pair.d22 < id; });
    if (it == pairs.end() || it->d22 != d22_id) return std::nullopt;
    return it->d21;
}

}  // namespace moq::interop::app
