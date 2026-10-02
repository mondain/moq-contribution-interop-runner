#pragma once

// Draft-21 completeness-gap scenarios (slice A). These are kept apart from the
// original registry so the two lists can evolve independently.

#include <algorithm>
#include <array>
#include <string_view>

namespace moq::interop::app {

// Scenarios executed by the announcement controller (one scenario per run,
// not raw probes). Their evidence is the publisher's SETUP and request-stream
// openings.
inline constexpr std::array<std::string_view, 5> kDraft21GapAnnouncementScenarios{
    "d21-publisher-request-stream-openers",
    "d21-publisher-setup-option-multiplicity",
    "d21-webtransport-publisher-setup",
    "d21-webtransport-server-sends-authority",
    "d21-webtransport-server-sends-path",
};

// Scenarios that execute as raw probe contexts.
inline constexpr std::array<std::string_view, 0> kDraft21GapRawScenarios{};

inline bool announcement_gap_scenario(unsigned draft, std::string_view scenario) {
    return draft == 21 &&
        std::find(kDraft21GapAnnouncementScenarios.begin(),
                  kDraft21GapAnnouncementScenarios.end(), scenario) !=
            kDraft21GapAnnouncementScenarios.end();
}

// Scenarios that only make sense over one transport.
inline bool gap_webtransport_only_scenario(std::string_view scenario) {
    return scenario == "d21-webtransport-publisher-setup" ||
           scenario == "d21-webtransport-server-sends-authority" ||
           scenario == "d21-webtransport-server-sends-path";
}

inline bool gap_native_only_scenario(std::string_view) { return false; }

}  // namespace moq::interop::app
