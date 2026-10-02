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
inline constexpr std::array<std::string_view, 10> kDraft21GapAnnouncementScenarios{
    "d21-publisher-request-stream-openers",
    "d21-publisher-setup-option-multiplicity",
    "d21-webtransport-publisher-setup",
    "d21-webtransport-server-sends-authority",
    "d21-webtransport-server-sends-path",
    "d21-native-publisher-uri-options",
    "d21-native-publisher-uri-query",
    "d21-native-publisher-empty-query",
    "d21-native-quic-required-setup-options",
    "d21-webtransport-required-setup-options",
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
           scenario == "d21-webtransport-server-sends-path" ||
           scenario == "d21-webtransport-required-setup-options";
}

inline bool gap_native_only_scenario(std::string_view scenario) {
    return scenario == "d21-native-publisher-uri-options" ||
           scenario == "d21-native-publisher-uri-query" ||
           scenario == "d21-native-publisher-empty-query" ||
           scenario == "d21-native-quic-required-setup-options";
}

// Path and query of the moqt URI a driven native publisher is given
// (Sections 9.1.1 and 9.1.2): the default path, a non-empty query, or a
// present-but-empty query.
inline std::string_view gap_native_uri_path_and_query(std::string_view scenario) {
    if (scenario == "d21-native-publisher-uri-query") return "/moq?run=1";
    if (scenario == "d21-native-publisher-empty-query") return "/moq?";
    return "/moq";
}

}  // namespace moq::interop::app
