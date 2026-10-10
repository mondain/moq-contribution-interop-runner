#pragma once

// The executable moq-lite-06 scenarios (L1d, L2a): the 27 scenario ids of requirements/moq-lite-06.json with their
// track-fixture trait. Free of scenario code, like own_scenarios_22.h, so every registry predicate links without
// the scenarios library. The probe of each id is built by src/scenarios/lite06_*.cpp (its builder's
// requires_track equals the trait here; tests/unit/lite_evaluators_test.cpp checks it).

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace moq::interop::app {

struct LiteScenarioTraits {
    std::string_view id;
    // The probe names the configured broadcast path and track (RunConfig::track_fixture), decision (d).
    bool requires_track{false};
};

inline constexpr auto kLiteExecutableScenarios = std::to_array<LiteScenarioTraits>({
    {"l06-setup-stream", false},
    {"l06-setup-unknown-parameter", false},
    {"l06-setup-duplicate-parameter", false},
    {"l06-setup-duplicate-stream", false},
    {"l06-setup-server-path", false},
    {"l06-setup-server-role", false},
    {"l06-setup-client-path", false},
    {"l06-announce-prefix", true},
    {"l06-announce-lifecycle", true},
    {"l06-session-stream-close", true},
    {"l06-subscribe-latest", true},
    {"l06-subscribe-refused", true},
    {"l06-subscribe-invalid-frame-bounds", true},
    {"l06-subscribe-group-floor", true},
    {"l06-subscribe-abutting-frame-start", true},
    {"l06-errors-unknown-stream-type", false},
    {"l06-errors-unknown-reset-code", true},
    {"l06-errors-reserved-reset-code", true},
    {"l06-errors-code-space", false},
    // L2a: Track, Fetch, Probe and Goaway.
    {"l06-track-info", true},
    {"l06-fetch-group", true},
    {"l06-fetch-unknown-group", true},
    {"l06-probe-report", false},
    {"l06-datagram-size", true},
    {"l06-goaway-single", true},
    {"l06-goaway-duplicate", false},
    {"l06-goaway-oversize", false},
});

// The ids of kLiteExecutableScenarios, in the same order (what executable_scenarios(106) lists).
inline constexpr auto kLiteExecutableScenarioIds = [] {
    std::array<std::string_view, kLiteExecutableScenarios.size()> ids{};
    for (std::size_t index = 0; index < ids.size(); ++index) ids[index] = kLiteExecutableScenarios[index].id;
    return ids;
}();

inline std::optional<LiteScenarioTraits> lite_executable_scenario(std::string_view id) {
    const auto found = std::find_if(kLiteExecutableScenarios.begin(), kLiteExecutableScenarios.end(),
                                    [&](const LiteScenarioTraits& traits) { return traits.id == id; });
    if (found == kLiteExecutableScenarios.end()) return std::nullopt;
    return *found;
}

}  // namespace moq::interop::app
