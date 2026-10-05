#include "moq/interop/requirements/lineage_policy.h"

namespace moq::interop::requirements {

std::vector<Equivalence> draft22_equivalences() {
    return {
        {"D22-8-4-MAY-263", {"D21-8-4-MAY-245"},
         "text identical to D21-8-4-MAY-245; the carry tool missed it across a page break (delta note)"},
        {"D22-9-MUST-294", {"D21-9-MUST-282", "D21-9-MUST-283"},
         "text identical to D21-9-MUST-282 and -283; the carry tool pulled Table 5 into the sentence (delta note)"},
    };
}

// Draft 21 wrote LOCATION_FILTER (0x21) as `21 <Length> <fields>`; draft 22 writes `21 <Type> <fields>`.
// Every scenario below builds its filters through scenarios::filter_param / filter_param_value, or reads
// received ones through the shared wire-aware parameter walk, so it emits and parses the run's wire draft.
// (a) = the scenario builds a LOCATION_FILTER, (b) = it reads one in received bytes. Field lists map onto
// draft 22 Types field for field: 2 fields Absolute (0x02), 3 AbsoluteBounded (0x03), 4 AbsoluteRange
// (0x04), none None (0x00); no scenario here sends the draft 21 {0,0} Next Object form.
std::set<std::string> draft21_location_filter_scenarios() {
    return {
        // draft21_gap_a.cpp location_filter() via subscribe() (a): OpenFromObject {7,9} (session_probe,
        // failing_update_probe).
        "d21-publisher-request-response-before-fin",
        "d21-established-subscription-publisher-fin",
        "d21-request-stream-terminal-message-order",  // also the FETCH built by fetch() (a)
        "d21-control-stream-lifetime",
        "d21-native-quic-datagram-support",
        "d21-webtransport-h3-datagram-support",
        "d21-native-quic-without-datagram-negotiation",
        "d21-webtransport-h3-without-datagram-negotiation",
        // draft21_gap_a.cpp Filter::WholeGroup {7,0,0} (a)
        "d21-original-publisher-opens-new-subgroup",
        "d21-publish-track-with-mandatory-property",
        "d21-subscribe-single-subgroup",
        // draft21_gap_a.cpp Filter::BoundedObject {7,9,0,9} (a)
        "d21-subscribe-bounded-location-range",
        // draft21_gap_a.cpp raise_start_update() (a), after a WholeGroup subscribe
        "d21-subgroup-restart-after-reset",
        // draft21_gap_a.cpp bounded_update() (a)
        "d21-update-subscription-location-range",
        // draft21_close.cpp {u64max,0,1}, top level and nested in FILL_PARAMETERS (a); under wire draft 22
        // the probe list omits both because the draft 22 form cannot carry the overflow
        "d21-location-filter-end-group-overflow",
        "d21-fill-location-filter-end-group-overflow",
        // fetch_first_object.cpp FETCH {7,9,0,9} (a)
        "d21-fetch-first-object-flags",
        // fetch_probe.cpp encode_fetch {0,0,u64max} (a), shared by both fetch probe definitions
        "d21-cancel-fetch-with-open-request-and-data-streams",
        "d21-failed-fetch-update-data-reset",
        // fetch_response.cpp FETCH {0,0,u64max} (a)
        "d21-fetch-accepted",
        "d21-fetch-rejected",
        // fetch_group_order.cpp FETCH {7,0,2,9} (a)
        "d21-fetch-ascending-groups",
        "d21-fetch-descending-groups",
        "d21-fetch-default-group-order",
        // immutable_repeat.cpp FETCH {7,9,0,9} (a)
        "d21-immutable-property-repeat",
        // object_repeat.cpp SUBSCRIBE FORWARD then a delta to 0x21 with {7,9} (a); the singleton scenario
        // reuses the first-object FETCH (a)
        "d21-repeat-object-retrieval",
        "d21-object-immutable-property-singleton",
        // draft21_contribution_objects.cpp start_filter() {7,9} / target_range_filter() {7,9,0,9} (a)
        "d21-fetch-parameters-preserve-payload",
        "d21-prior-group-gap-repeat",
        "d21-prior-group-gap-singleton",
        "d21-prior-object-gap-repeat",
        "d21-prior-object-gap-singleton",
        "d21-subscription-forwarding-preference",  // preference_build()
        "d21-fetch-datagram-preference",
        "d21-subgroup-start-location-fin",
        // draft21_contribution_d21b.cpp SUBSCRIBE {1000000,0} (a)
        "d21-publish-done-without-data-streams",
        // draft21_contribution_d21b.cpp parse_block() reads received PUBLISH_STATE_NOTIFY / SUBSCRIBE_OK /
        // REQUEST_OK parameter blocks through walk_message_parameters (b)
        "d21-publish-state-notify-known-largest-object",
        "d21-publish-state-notify-before-first-object",
        "d21-publish-state-notify-preserves-subscriber-control",
        "d21-publish-state-notify-requested-forward-change",
        // draft21_contribution_session.cpp FETCH {1<<62,0} (a)
        "d21-fetch-start-beyond-largest-object",
        // draft21_contribution_session.cpp walk_parameters() reads received PUBLISH and
        // PUBLISH_STATE_NOTIFY parameter blocks through walk_message_parameters (b)
        "d21-publisher-parameter-serialization",
        "d21-publisher-parameter-negotiation",
        "d21-publisher-parameter-multiplicity",
        // draft21_contribution_residual_subscription.cpp location_range() / bounded_filter(), 4 fields (a)
        "d21-overlapping-subscriptions-shared-alias",
        "d21-overlapping-subscriptions-distinct-aliases",
        "d21-forward-location-and-range-filter-conjunction",
        // draft21_contribution_residual_subscription.cpp fill_whole_track() nests an empty LOCATION_FILTER
        // in FILL_PARAMETERS (a): byte 00 in both drafts (draft 21 Length 0, draft 22 Type None), which
        // both drafts read as the entire track up to Largest Object (draft 22 section 3.4, "zero-length")
        "d21-fill-fails-before-first-object",
        "d21-cancel-subscription-with-concurrent-fill-streams",
        // draft21_contribution_residual_token.cpp whole_group {0,0,0} (a)
        "d21-subgroup-completion-withheld-acknowledgments",
    };
}

// The residual: draft 21 scenarios whose expectation is a draft 21 filter fact or which cannot run under
// wire draft 22. Each has a draft 22 replacement written as an own scenario.
std::set<std::string> draft22_filter_building_scenarios() {
    return {
        // Sends {u64max,0,1}: StartGroup + EndGroupDelta overflows. Draft 22 cannot encode it, so the probe
        // list omits it under wire draft 22 (draft21_close.cpp); replaced by a raw draft 22 probe for
        // D22-9-20-9-MUST-424 (also own by that row).
        "d21-location-filter-end-group-overflow",
        // The same overflow nested in FILL_PARAMETERS; omitted under wire draft 22 for the same reason;
        // replaced alongside it for D22-9-20-9-MUST-424 (also own by that row).
        "d21-fill-location-filter-end-group-overflow",
    };
}

}  // namespace moq::interop::requirements
