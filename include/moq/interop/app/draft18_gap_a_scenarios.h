#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace moq::interop::app {

// Scenario IDs executed by the draft-18 gap-A raw probes
// (include/moq/interop/scenarios/draft18_gap_a.h). Keep in step with
// gap_a::*_entries(); tests/protocol/draft18_gap_a_registry_test.cpp checks it.
inline constexpr std::array<std::string_view, 7> kDraft18GapAScenarios{
    "receive-setup-with-unknown-option",
    "complete-publisher-requests-while-session-remains-open",
    "establish-moqt-with-datagram-capable-peer",
    "native-quic-publisher-client-setup-from-moqt-uri",
    "register-delete-then-use-token-alias",
    "register-valid-token-then-use-alias-in-later-request",
    "register-token-in-rejected-request-then-use-alias",
};

// The subset that names a configured track fixture in its stimulus.
inline constexpr std::array<std::string_view, 5> kDraft18GapATrackScenarios{
    "receive-setup-with-unknown-option",
    "complete-publisher-requests-while-session-remains-open",
    "register-delete-then-use-token-alias",
    "register-valid-token-then-use-alias-in-later-request",
    "register-token-in-rejected-request-then-use-alias",
};

template <std::size_t A, std::size_t B>
constexpr std::array<std::string_view, A + B> concat_scenarios(
    const std::array<std::string_view, A>& first,
    const std::array<std::string_view, B>& second) {
    std::array<std::string_view, A + B> result{};
    for (std::size_t i = 0; i < A; ++i) result[i] = first[i];
    for (std::size_t i = 0; i < B; ++i) result[A + i] = second[i];
    return result;
}

}  // namespace moq::interop::app
