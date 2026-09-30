#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace moq::interop::app {

inline constexpr std::array<std::string_view, 5> kDraft18ExecutableScenarios{
    "subscribe-to-publisher-track",
    "subscribe-again-to-established-publisher-track",
    "fetch-publisher-track-range",
    "subscribe-namespace-at-publisher",
    "subscribe-tracks-at-publisher",
};

inline constexpr std::array<std::string_view, 5> kDraft21ExecutableScenarios{
    "d21-publisher-request-stream-placement",
    "d21-setup-unknown-options",
    "d21-setup-duplicate-unknown-options",
    "d21-server-sends-authority",
    "d21-server-sends-path",
};

inline std::span<const std::string_view> executable_scenarios(unsigned draft) {
    if (draft == 18) return kDraft18ExecutableScenarios;
    if (draft == 21) return kDraft21ExecutableScenarios;
    return {};
}

inline bool executable_scenario(unsigned draft, std::string_view scenario) {
    const auto known = executable_scenarios(draft);
    return std::find(known.begin(), known.end(), scenario) != known.end();
}

}  // namespace moq::interop::app
