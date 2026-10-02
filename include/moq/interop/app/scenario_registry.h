#pragma once

#include "moq/interop/app/draft18_gap_a_scenarios.h"
#include "moq/interop/app/scenario_registry_d21a.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace moq::interop::app {

inline constexpr std::array<std::string_view, 119> kDraft18ExecutableScenarios{
    "subscribe-to-publisher-track",
    "subscribe-again-to-established-publisher-track",
    "fetch-publisher-track-range",
    "subscribe-namespace-at-publisher",
    "subscribe-tracks-at-publisher",
    "receive-subgroup-types-with-subgroup-id-mode-three",
    "receive-invalid-subgroup-header-type-0x80",
    "receive-unknown-datagram-type",
    "receive-datagram-types-0x22-0x23-0x26-0x27-0x2a-0x2b-0x2e-0x2f",
    "receive-invalid-object-datagram-type-0x10",
    "receive-key-value-type-overflow",
    "receive-key-value-length-over-65535",
    "receive-understood-key-value-invalid-serialization",
    "receive-zero-length-namespace-field",
    "receive-namespace-with-33-fields",
    "receive-track-namespace-over-4096-bytes",
    "receive-full-track-name-over-4096-bytes",
    "receive-disallowed-initial-bidirectional-message",
    "receive-unknown-unidirectional-stream-type",
    "receive-undefined-subscription-filter-type",
    "receive-unknown-message-type",
    "receive-known-message-with-mismatched-payload-length",
    "receive-request-id-wrong-peer-parity",
    "receive-duplicate-request-id-across-request-streams",
    "receive-message-parameter-type-delta-overflow",
    "receive-unnegotiated-unknown-message-parameter",
    "receive-duplicate-nonrepeatable-message-parameter",
    "receive-message-parameter-on-disallowed-message-native-quic",
    "receive-undecodable-authorization-token-structure",
    "receive-group-order-zero-or-greater-than-two",
    "receive-forward-outside-zero-one",
    "receive-server-setup-with-authority",
    "receive-webtransport-setup-with-authority",
    "receive-server-setup-with-path",
    "receive-webtransport-setup-with-path",
    "receive-two-goaways-on-control-stream",
    "receive-goaway-uri-length-8193",
    "receive-control-goaway-with-wrong-receiver-request-id-parity",
    "receive-fetch-with-unknown-type",
    "receive-subscribe-namespace-with-33-prefix-fields",
    "receive-subscribe-tracks-with-33-prefix-fields",
    "absolute-range-end-group-overflow",
    "register-same-peer-token-alias-twice-without-delete",
    "register-request-token-exceeding-advertised-cache-size",
    "request-track-in-single-period-namespace",
    "request-empty-track-name-in-session-namespace",
    "request-unrecognized-session-level-name",
    "receive-use-alias-for-unregistered-token",
    "receive-delete-for-unregistered-token",
    "receive-joining-fetch-with-unrelated-or-wrong-state-request-id",
    "receive-reason-phrase-length-over-1024",
    "receive-publish-request-ok-with-track-properties",
    "receive-request-update-ok-with-track-properties",
    "cancel-fetch-request-with-open-data-stream",
    "cancel-subscribe-with-multiple-open-subgroups",
    "reject-request-update-for-open-fetch",
    "reject-subscription-request-update",
    "reject-subscribe-namespace-request-update",
    "receive-publish-namespace-ok-with-track-properties",
    "receive-publish-namespace-redirect-with-nonempty-track-name",
    "publisher-recovery-track-status-reply-with-invalid-default-group-order",
    "publisher-recovery-track-status-reply-with-dynamic-groups-two",
    "publisher-recovery-track-status-reply-with-invalid-default-group-order-in-immutable-wrapper",
    "update-namespace-subscription-prefix-to-overlap-active-namespace-subscription",
    "update-track-subscription-prefix-to-overlap-active-track-subscription",
    "receive-overlapping-subscribe-namespace-in-same-session",
    "receive-overlapping-subscribe-tracks-in-same-session",
    "fetch-known-first-object-with-nonzero-group-and-object-ids",
    "fetch-multiple-published-groups-in-each-explicit-order",
    "receive-two-goaways-on-same-request-stream",
    "publish-and-retrieve-same-object-and-track-immutable-properties",
    "repeat-immutable-property-with-alternative-varint-encodings-available",
    "publish-object-with-immutable-properties",
    // Draft-18 publisher-contribution probe families (gap-closing block).
    "observe-publisher-setup-options",
    "observe-webtransport-publisher-setup",
    "receive-setup-with-unknown-option",
    "receive-setup-with-duplicate-unknown-options",
    "setup-unknown-grease-options-and-duplicates",
    "receive-setup-token-register-exceeding-cache-limit",
    "receive-oversize-setup-register-then-use-its-alias",
    "accept-subscribe-for-known-publisher-track",
    "accept-forward-one-subscribe-then-publish-matching-object",
    "receive-one-request-update-on-established-publisher-request",
    "receive-multiple-successful-request-updates-on-one-stream",
    "publisher-ends-subscription-with-no-data-streams",
    "receive-joining-fetch-for-forward-zero-subscription",
    "receive-forward-state-update-then-joining-fetch",
    "receive-joining-fetch-for-track-with-no-published-objects",
    "receive-standalone-fetch-for-track-with-no-published-objects",
    "receive-fetch-start-beyond-largest-published-object",
    "publisher-redirects-subscribe-namespace",
    "publish-and-withdraw-namespace-during-discovery",
    "subscribe-unknown-auth-token-type",
    "publisher-recovery-track-status-unknown-optional-properties",
    "publisher-recovery-track-status-unknown-before-invalid-property",
    "publisher-request-rejected-with-unknown-error",
    "publisher-request-stream-reset-with-unknown-code",
    "observe-publisher-padding-stream",
    "observe-publisher-padding-datagram",
    "publish-object-with-prior-group-id-gap",
    "publish-object-with-prior-object-id-gap",
    "publish-end-of-group-and-end-of-track-status-objects",
    "publish-complete-finite-subgroup-with-start-location-filter",
    "observe-publisher-client-goaway",
    "send-new-request-after-publisher-control-goaway",
    "publisher-control-goaway-with-pending-request-at-cutoff",
    "connect-publisher-to-native-uri-with-authority",
    "connect-publisher-to-native-uri-with-path",
    "connect-publisher-to-native-uri-with-query",
    "cancel-subscription-before-next-subgroup-object-is-produced",
    "advance-start-location-while-subgroup-remains-incomplete",
    "pause-forwarding-with-an-unsent-subgroup-object",
    "publisher-terminates-subgroup-before-final-object-production",
    "fetch-object-previously-observed-as-datagram",
    "publish-two-simultaneous-tracks",
    "redeliver-previously-observed-object-in-later-subscription",
    "receive-subscribe-namespace-denied-by-configured-authorization-policy",
    "receive-subscribe-tracks-denied-by-configured-authorization-policy",
};

inline constexpr std::array<std::string_view, 108> kDraft21ExecutableScenarios{
    "d21-publisher-request-stream-placement",
    "d21-setup-unknown-options",
    "d21-setup-duplicate-unknown-options",
    "d21-server-sends-authority",
    "d21-server-sends-path",
    "d21-invalid-bidirectional-request-stream-opener",
    "d21-unknown-unidirectional-stream-type",
    "d21-unknown-datagram-type",
    "d21-unknown-control-message",
    "d21-message-body-length-mismatch",
    "d21-request-id-wrong-sender-parity",
    "d21-duplicate-request-id-across-streams",
    "d21-duplicate-request-update-id",
    "d21-setup-key-value-type-overflow",
    "d21-setup-key-value-declared-length-overflow",
    "d21-setup-known-key-value-malformed-value",
    "d21-subscribe-empty-namespace-field",
    "d21-subscribe-33-namespace-fields",
    "d21-subscribe-namespace-prefix-too-many-fields",
    "d21-subscribe-tracks-prefix-too-many-fields",
    "d21-subscribe-tracks-oversized-namespace",
    "d21-subscribe-oversized-full-track-name",
    "d21-duplicate-control-goaway",
    "d21-goaway-uri-length-boundary",
    "d21-parameter-type-delta-overflow",
    "d21-request-undecodable-authorization-token",
    "d21-unknown-message-parameter",
    "d21-unexpected-duplicate-message-parameter",
    "d21-parameter-invalid-message-scope",
    "d21-group-order-in-subscription-update",
    "d21-group-order-zero",
    "d21-location-filter-end-group-overflow",
    "d21-forward-value-two",
    "d21-include-properties-value-two",
    "d21-fill-forbidden-nested-authorization",
    "d21-fill-forbidden-track-property-filter",
    "d21-fill-recursive-parameter",
    "d21-duplicate-request-goaway",
    "d21-update-on-track-status",
    "d21-responder-update-on-publish-namespace",
    "d21-subscriber-update-on-publish",
    "d21-failed-subscription-update-cleanup",
    "d21-cancel-fetch-with-open-request-and-data-streams",
    "d21-cancel-subscribe-with-open-streams",
    "d21-failed-fetch-update-data-reset",
    "d21-fetch-accepted",
    "d21-fetch-rejected",
    "d21-subscribe-accepted",
    "d21-subscribe-rejected",
    "d21-subscribe-namespace-accepted",
    "d21-subscribe-namespace-rejected",
    "d21-subscribe-tracks-accepted",
    "d21-subscribe-tracks-rejected",
    "d21-duplicate-range-filter-key-in-update",
    "d21-range-filter-total-exceeds-negotiated-limit",
    "d21-range-filter-with-zero-negotiated-limit",
    "d21-range-filter-total-limit",
    "d21-range-filter-default-zero-limit",
    "d21-range-filter-update-total-limit",
    "d21-failed-subscribe-namespace-update-close",
    "d21-failed-subscribe-tracks-update-close",
    "d21-token-duplicate-registration",
    "d21-request-token-cache-overflow",
    "d21-request-single-period-namespace",
    "d21-session-namespace-empty-track-request",
    "d21-session-namespace-unknown-track-request",
    "d21-session-namespace-unknown-namespace-request",
    "d21-range-filter-start-delta-overflow",
    "d21-range-filter-end-delta-overflow",
    "d21-duplicate-range-filter-key-in-request",
    "d21-priority-filter-start-above-255",
    "d21-priority-filter-end-above-255",
    "d21-object-property-filter-odd-property-type",
    "d21-track-property-filter-odd-property-type",
    "d21-request-unknown-token-alias",
    "d21-unknown-request-stream-message",
    "d21-request-message-truncated-at-fin",
    "d21-group-order-above-two",
    "d21-fill-invalid-group-order",
    "d21-fill-location-filter-end-group-overflow",
    "d21-forward-value-255",
    "d21-discovery-update-invalid-forward",
    "d21-include-properties-value-255",
    "d21-request-alias-registration-with-default-zero-cache",
    "d21-fill-timeout-outside-fill-or-fetch",
    "d21-publish-request-error-oversized-reason",
    "d21-publish-ok-with-track-properties",
    "d21-publish-update-ok-with-track-properties",
    "d21-publish-namespace-ok-with-track-properties",
    "d21-publish-namespace-redirect-nonempty-track-name",
    "d21-subscribe-namespace-overlap",
    "d21-subscribe-tracks-overlap",
    "d21-discovery-independent-overlap-spaces",
    "d21-namespace-prefix-update-overlap",
    "d21-track-prefix-update-overlap",
    "d21-discovery-update-independent-overlap-spaces",
    "d21-fetch-first-object-flags",
    "d21-fetch-ascending-groups",
    "d21-fetch-descending-groups",
    "d21-fetch-default-group-order",
    "d21-publish-state-notify-on-namespace-request",
    "d21-publish-state-notify-on-fetch",
    "d21-subscriber-sends-publish-state-notify",
    "d21-publish-established-subscriber-sends-publish-state-notify",
    "d21-goaway-on-distinct-request-streams",
    "d21-immutable-property-repeat",
    "d21-repeat-object-retrieval",
    "d21-object-immutable-property-singleton",
};

// Draft-18 gap-A probes extend the core list; they are all raw probes.
inline constexpr auto kDraft18AllExecutableScenarios =
    concat_scenarios(kDraft18ExecutableScenarios, kDraft18GapAScenarios);
// Draft-21 contribution profiles (src/scenarios/draft21_contribution_*.cpp).
// Kept apart from the list above; a unit test keeps it equal to the profile
// sources, and executable_scenarios(21) exposes both lists as one.
inline constexpr std::string_view kDraft21ContributionScenarios[]{
    "d21-setup-register-exceeds-token-cache",
    "d21-setup-register-default-zero-cache",
    "d21-grease-setup-options",
    "d21-grease-request-error",
    "d21-single-request-update-response",
    "d21-coalesced-successful-update-responses",
    "d21-coalesced-failed-update-response",
    "d21-request-update-overrun",
    "d21-request-update-independent-streams",
    "d21-request-update-unlimited",
    "d21-successful-subscribe-response",
    "d21-fetch-start-beyond-largest-object",
    "d21-fetch-track-with-no-published-objects",
    "d21-namespace-discovery-withdrawal-order",
    "d21-publisher-parameter-serialization",
    "d21-publisher-parameter-negotiation",
    "d21-publisher-parameter-multiplicity",
    "d21-inbound-padding-stream",
    "d21-inbound-padding-datagram",
    "d21-successful-subscribe-object-delivery",
    "d21-successful-subscribe-forward-zero",
    "d21-subscribe-parameters-preserve-payload",
    "d21-fetch-parameters-preserve-payload",
    "d21-prior-group-gap-repeat",
    "d21-prior-group-gap-singleton",
    "d21-prior-object-gap-repeat",
    "d21-prior-object-gap-singleton",
    "d21-subscription-forwarding-preference",
    "d21-fetch-datagram-preference",
    "d21-object-datagram-flags",
    "d21-subgroup-header-flags",
    "d21-complete-subgroup-fin",
    "d21-subgroup-start-location-fin",
    "d21-subgroup-premature-close-reset",
};

inline bool draft21_contribution_scenario(unsigned draft, std::string_view scenario) {
    return draft == 21 &&
        std::find(std::begin(kDraft21ContributionScenarios), std::end(kDraft21ContributionScenarios),
                  scenario) != std::end(kDraft21ContributionScenarios);
}

inline std::span<const std::string_view> executable_scenarios(unsigned draft) {
    if (draft == 18) return kDraft18AllExecutableScenarios;
    if (draft == 18) return kDraft18ExecutableScenarios;
    if (draft == 21) {
        static const std::vector<std::string_view> combined = [] {
            std::vector<std::string_view> all(kDraft21ExecutableScenarios.begin(),
                                              kDraft21ExecutableScenarios.end());
            all.insert(all.end(), kDraft21GapAnnouncementScenarios.begin(),
                       kDraft21GapAnnouncementScenarios.end());
            all.insert(all.end(), kDraft21GapRawScenarios.begin(),
                       kDraft21GapRawScenarios.end());
            all.insert(all.end(), std::begin(kDraft21ContributionScenarios),
                       std::end(kDraft21ContributionScenarios));
            return all;
        }();
        return combined;
    }
    return {};
}

inline bool fetch_first_object_scenario(unsigned draft, std::string_view scenario) {
    return (draft == 18 && scenario == "fetch-known-first-object-with-nonzero-group-and-object-ids") ||
           (draft == 21 && scenario == "d21-fetch-first-object-flags");
}

inline bool immutable_repeat_scenario(unsigned draft, std::string_view scenario) {
    return (draft == 18 && (scenario == "publish-and-retrieve-same-object-and-track-immutable-properties" ||
                           scenario == "repeat-immutable-property-with-alternative-varint-encodings-available")) ||
           (draft == 21 && scenario == "d21-immutable-property-repeat");
}

inline bool object_repeat_scenario(unsigned draft, std::string_view scenario) {
    return (draft == 18 && scenario == "publish-object-with-immutable-properties") ||
           (draft == 21 && (scenario == "d21-repeat-object-retrieval" ||
                           scenario == "d21-object-immutable-property-singleton"));
}

inline bool subscriber_notify_scenario(unsigned draft, std::string_view scenario) {
    return draft == 21 && (scenario == "d21-subscriber-sends-publish-state-notify" ||
                          scenario == "d21-publish-established-subscriber-sends-publish-state-notify");
}

inline bool request_goaway_scenario(unsigned draft, std::string_view scenario) {
    return (draft == 18 && scenario == "receive-two-goaways-on-same-request-stream") ||
        (draft == 21 && (scenario == "d21-duplicate-request-goaway" ||
                        scenario == "d21-goaway-on-distinct-request-streams"));
}

inline bool fetch_group_order_scenario(unsigned draft, std::string_view scenario) {
    return (draft == 18 && scenario == "fetch-multiple-published-groups-in-each-explicit-order") ||
        (draft == 21 && (scenario == "d21-fetch-ascending-groups" ||
                        scenario == "d21-fetch-descending-groups" ||
                        scenario == "d21-fetch-default-group-order"));
}

inline bool discovery_overlap_scenario(unsigned draft, std::string_view scenario) {
    const bool discovery_overlap =
        scenario == "update-namespace-subscription-prefix-to-overlap-active-namespace-subscription"
        || scenario == "update-track-subscription-prefix-to-overlap-active-track-subscription"
        || scenario == "receive-overlapping-subscribe-namespace-in-same-session"
        || scenario == "receive-overlapping-subscribe-tracks-in-same-session"
        || scenario == "d21-subscribe-namespace-overlap"
        || scenario == "d21-subscribe-tracks-overlap"
        || scenario == "d21-discovery-independent-overlap-spaces"
        || scenario == "d21-namespace-prefix-update-overlap"
        || scenario == "d21-track-prefix-update-overlap"
        || scenario == "d21-discovery-update-independent-overlap-spaces";
    return discovery_overlap && std::find(executable_scenarios(draft).begin(),executable_scenarios(draft).end(),scenario) != executable_scenarios(draft).end();
}

// Draft-18 publisher-contribution probes whose first write names the fixture.
inline constexpr std::array<std::string_view, 26> kDraft18ContributionTrackScenarios{
    "accept-subscribe-for-known-publisher-track",
    "accept-forward-one-subscribe-then-publish-matching-object",
    "receive-one-request-update-on-established-publisher-request",
    "receive-multiple-successful-request-updates-on-one-stream",
    "publisher-ends-subscription-with-no-data-streams",
    "receive-joining-fetch-for-forward-zero-subscription",
    "receive-forward-state-update-then-joining-fetch",
    "receive-joining-fetch-for-track-with-no-published-objects",
    "receive-standalone-fetch-for-track-with-no-published-objects",
    "receive-fetch-start-beyond-largest-published-object",
    "publisher-redirects-subscribe-namespace",
    "publish-and-withdraw-namespace-during-discovery",
    "subscribe-unknown-auth-token-type",
    "publish-object-with-prior-group-id-gap",
    "publish-object-with-prior-object-id-gap",
    "publish-end-of-group-and-end-of-track-status-objects",
    "publish-complete-finite-subgroup-with-start-location-filter",
    "cancel-subscription-before-next-subgroup-object-is-produced",
    "advance-start-location-while-subgroup-remains-incomplete",
    "pause-forwarding-with-an-unsent-subgroup-object",
    "publisher-terminates-subgroup-before-final-object-production",
    "fetch-object-previously-observed-as-datagram",
    "publish-two-simultaneous-tracks",
    "redeliver-previously-observed-object-in-later-subscription",
    "receive-subscribe-namespace-denied-by-configured-authorization-policy",
    "receive-subscribe-tracks-denied-by-configured-authorization-policy",
};

inline bool scenario_requires_track(unsigned draft, std::string_view scenario) {
    if (draft == 18 && std::find(kDraft18GapATrackScenarios.begin(), kDraft18GapATrackScenarios.end(),
                                 scenario) != kDraft18GapATrackScenarios.end()) return true;
    if (draft == 18 && std::find(kDraft18ContributionTrackScenarios.begin(),
                                 kDraft18ContributionTrackScenarios.end(),
                                 scenario) != kDraft18ContributionTrackScenarios.end()) return true;
    if (draft21_contribution_scenario(draft, scenario)) return true;
    if (immutable_repeat_scenario(draft,scenario) || object_repeat_scenario(draft,scenario)) return true;
    if (fetch_first_object_scenario(draft,scenario) || fetch_group_order_scenario(draft,scenario)) return true;
    if (subscriber_notify_scenario(draft,scenario) ||
        (draft == 21 && scenario == "d21-publish-state-notify-on-fetch")) return true;
    if (discovery_overlap_scenario(draft,scenario)) return true;
    if (announcement_gap_scenario(draft, scenario) || gap_raw_scenario(draft, scenario)) return true;
    const auto original = executable_scenarios(draft).first(
        draft == 18 || draft == 21 ? 5 : 0);
    return std::find(original.begin(), original.end(), scenario) != original.end() ||
        (draft == 18 && (scenario == "cancel-fetch-request-with-open-data-stream" ||
                         scenario == "reject-request-update-for-open-fetch" ||
                         scenario == "cancel-subscribe-with-multiple-open-subgroups")) ||
        (draft == 21 && (scenario == "d21-cancel-fetch-with-open-request-and-data-streams" ||
                         scenario == "d21-failed-fetch-update-data-reset" ||
                         scenario == "d21-fetch-accepted" ||
                         scenario == "d21-fetch-rejected" ||
                         scenario == "d21-subscribe-accepted" ||
                         scenario == "d21-subscribe-rejected" ||
                         scenario == "d21-subscribe-namespace-accepted" ||
                         scenario == "d21-subscribe-namespace-rejected" ||
                         scenario == "d21-subscribe-tracks-accepted" ||
                         scenario == "d21-subscribe-tracks-rejected" ||
                         scenario == "d21-duplicate-range-filter-key-in-update" ||
                         scenario == "d21-range-filter-total-exceeds-negotiated-limit" ||
                         scenario == "d21-range-filter-with-zero-negotiated-limit" ||
                         scenario == "d21-range-filter-total-limit" ||
                         scenario == "d21-range-filter-default-zero-limit" ||
                         scenario == "d21-range-filter-update-total-limit" ||
                         scenario == "d21-cancel-subscribe-with-open-streams"));
}

inline bool executable_scenario(unsigned draft, std::string_view scenario) {
    const auto known = executable_scenarios(draft);
    return std::find(known.begin(), known.end(), scenario) != known.end();
}

inline bool raw_probe_scenario(unsigned draft, std::string_view scenario) {
    if (announcement_gap_scenario(draft, scenario)) return false;
    const auto known = executable_scenarios(draft);
    // The first five entries use the original typed controllers. All later
    // entries execute raw probes and can contribute an independent transcript.
    return known.size() > 5 &&
        std::find(known.begin() + 5, known.end(), scenario) != known.end();
}

}  // namespace moq::interop::app
