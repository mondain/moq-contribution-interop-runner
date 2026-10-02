#pragma once

// Draft-21 completeness-gap scenarios (slice A). These are kept apart from the
// original registry so the two lists can evolve independently.

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::app {

// Scenarios executed by the announcement controller (one scenario per run,
// not raw probes). Their evidence is the publisher's SETUP and request-stream
// openings.
inline constexpr std::array<std::string_view, 19> kDraft21GapAnnouncementScenarios{
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
    "d21-attempt-unregistered-period-namespace-publication",
    "d21-attempt-single-period-namespace-use",
    "d21-attempt-single-period-track-publication",
    "d21-attempt-single-period-namespace-publication",
    "d21-application-track-publication-under-session",
    "d21-application-namespace-publication-under-session",
    "d21-publisher-key-value-type-deltas",
    "d21-publisher-emitted-namespace-fields",
    "d21-publisher-namespace-routing-announcement",
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

// Fixture rule for the reserved-namespace attempt scenarios (Sections 2.4.2
// and 6.5): the configured namespace is what the publisher is told to publish.
// Other announcement scenarios accept any fixture.
inline bool gap_fixture_valid(std::string_view scenario,
                              const std::vector<std::string>& namespace_fields) {
    const auto first = namespace_fields.empty() ? std::string_view{}
                                                : std::string_view{namespace_fields.front()};
    if (scenario == "d21-attempt-unregistered-period-namespace-publication")
        return first.size() > 1 && first.front() == '.' && first != ".session";
    if (scenario == "d21-attempt-single-period-namespace-use" ||
        scenario == "d21-attempt-single-period-track-publication" ||
        scenario == "d21-attempt-single-period-namespace-publication")
        return first == ".";
    if (scenario == "d21-application-track-publication-under-session" ||
        scenario == "d21-application-namespace-publication-under-session")
        return first == ".session";
    return true;
}

}  // namespace moq::interop::app
