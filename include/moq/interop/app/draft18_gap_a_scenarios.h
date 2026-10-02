#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace moq::interop::app {

// Scenario IDs executed by the draft-18 gap-A raw probes
// (include/moq/interop/scenarios/draft18_gap_a.h). Keep in step with
// gap_a::*_entries(); tests/protocol/draft18_gap_a_registry_test.cpp checks it.
inline constexpr std::array<std::string_view, 30> kDraft18GapAScenarios{
    "receive-setup-with-unknown-option",
    "complete-publisher-requests-while-session-remains-open",
    "establish-moqt-with-datagram-capable-peer",
    "native-quic-publisher-client-setup-from-moqt-uri",
    "register-delete-then-use-token-alias",
    "register-valid-token-then-use-alias-in-later-request",
    "register-token-in-rejected-request-then-use-alias",
    "publish-with-multiple-message-parameter-types",
    "publish-with-multiple-configured-parameters",
    "subscribe-namespace-at-publisher-with-matching-namespace",
    "discover-authoritative-publisher-namespace-by-exact-and-prefix-subscriptions",
    "publish-track-namespace-fields",
    "initiate-track-publication",
    "initiate-namespace-publication",
    "publish-namespace-for-relay-subscription-routing",
    "receive-subscribe-before-outstanding-publish-response",
    "retrieve-same-object-at-distinct-times",
    "subscribe-to-track-after-observed-object-publication",
    "publish-existing-track-after-observed-object-publication",
    "accepted-subscription-update-after-observed-object-publication",
    "accepted-track-status-after-observed-object-publication",
    "publish-new-subgroup",
    "subscribe-to-subgroup-without-reset-or-upstream-reordering",
    "publisher-rejects-subscribe-request",
    "publish-objects-before-within-and-after-subscription-range",
    "joining-fetch-after-forward-enabled-and-track-advanced",
    // Slice B (src/scenarios/draft18_gap_b.cpp).
    "publisher-queries-track-status-before-resuming-publication",
    "publish-with-and-without-parameter-extension-negotiation",
    "retrieve-same-object-with-different-subscribe-publish-ok-and-fetch-parameters",
    "withhold-use-alias-response-while-publisher-retires-token",
};

// The subset that names a configured track fixture in its stimulus.
inline constexpr std::array<std::string_view, 24> kDraft18GapATrackScenarios{
    "receive-setup-with-unknown-option",
    "complete-publisher-requests-while-session-remains-open",
    "register-delete-then-use-token-alias",
    "register-valid-token-then-use-alias-in-later-request",
    "register-token-in-rejected-request-then-use-alias",
    "publish-with-multiple-message-parameter-types",
    "publish-with-multiple-configured-parameters",
    "subscribe-namespace-at-publisher-with-matching-namespace",
    "discover-authoritative-publisher-namespace-by-exact-and-prefix-subscriptions",
    "publish-track-namespace-fields",
    "initiate-track-publication",
    "receive-subscribe-before-outstanding-publish-response",
    "retrieve-same-object-at-distinct-times",
    "subscribe-to-track-after-observed-object-publication",
    "publish-existing-track-after-observed-object-publication",
    "accepted-subscription-update-after-observed-object-publication",
    "accepted-track-status-after-observed-object-publication",
    "publish-new-subgroup",
    "subscribe-to-subgroup-without-reset-or-upstream-reordering",
    "publisher-rejects-subscribe-request",
    "publish-objects-before-within-and-after-subscription-range",
    "joining-fetch-after-forward-enabled-and-track-advanced",
    "publish-with-and-without-parameter-extension-negotiation",
    "retrieve-same-object-with-different-subscribe-publish-ok-and-fetch-parameters",
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
