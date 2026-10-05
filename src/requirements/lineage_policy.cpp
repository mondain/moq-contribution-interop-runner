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
// (a) = the scenario builds a LOCATION_FILTER, (b) = it reads one in received bytes.
std::set<std::string> draft22_filter_building_scenarios() {
    return {
        // draft21_gap_a.cpp:84-89 location_filter() via subscribe():104-106 (a): OpenFromObject
        // (session_probe, line 783) and failing_update_probe (line 786).
        "d21-publisher-request-response-before-fin",
        "d21-established-subscription-publisher-fin",
        "d21-request-stream-terminal-message-order",  // also fetch() at lines 143-152 (a)
        "d21-control-stream-lifetime",
        "d21-native-quic-datagram-support",
        "d21-webtransport-h3-datagram-support",
        "d21-native-quic-without-datagram-negotiation",
        "d21-webtransport-h3-without-datagram-negotiation",
        // draft21_gap_a.cpp:106 Filter::WholeGroup (a)
        "d21-original-publisher-opens-new-subgroup",
        "d21-publish-track-with-mandatory-property",
        "d21-subscribe-single-subgroup",
        // draft21_gap_a.cpp:105 Filter::BoundedObject (a)
        "d21-subscribe-bounded-location-range",
        // draft21_gap_a.cpp:128-136 raise_start_update() (a), after a WholeGroup subscribe
        "d21-subgroup-restart-after-reset",
        // draft21_gap_a.cpp:116-124 bounded_update() (a)
        "d21-update-subscription-location-range",
        // draft21_close.cpp:336-338 builds `0x21 <length> ...` (a); lines 418-420 nest it in FILL_PARAMETERS (a)
        "d21-location-filter-end-group-overflow",
        "d21-fill-location-filter-end-group-overflow",
        // fetch_first_object.cpp:76-81 FETCH with a length-prefixed 0x21 (a)
        "d21-fetch-first-object-flags",
        // fetch_probe.cpp:62-66 encode_fetch (a), shared by both fetch probe definitions
        "d21-cancel-fetch-with-open-request-and-data-streams",
        "d21-failed-fetch-update-data-reset",
        // fetch_response.cpp:59-63 (a)
        "d21-fetch-accepted",
        "d21-fetch-rejected",
        // fetch_group_order.cpp:67-72 (a)
        "d21-fetch-ascending-groups",
        "d21-fetch-descending-groups",
        "d21-fetch-default-group-order",
        // immutable_repeat.cpp:57-63 FETCH with 0x21 (a)
        "d21-immutable-property-repeat",
        // object_repeat.cpp:55-56 SUBSCRIBE parameters {0x10, 1, 0x11 (delta to 0x21), 2, 7, 9} (a); the
        // singleton scenario reuses the first-object FETCH (object_repeat.cpp:57-67, a)
        "d21-repeat-object-retrieval",
        "d21-object-immutable-property-singleton",
        // draft21_contribution_objects.cpp:225-232 start_filter()/target_range_filter() (a)
        "d21-fetch-parameters-preserve-payload",   // lines 338, 340
        "d21-prior-group-gap-repeat",              // lines 401-402 (gap_specs)
        "d21-prior-group-gap-singleton",           // line 431
        "d21-prior-object-gap-repeat",
        "d21-prior-object-gap-singleton",
        "d21-subscription-forwarding-preference",  // preference_build(), lines 466-468
        "d21-fetch-datagram-preference",
        "d21-subgroup-start-location-fin",         // line 557
        // draft21_contribution_d21b.cpp:371-375 param_lp(0x21, ...) (a)
        "d21-publish-done-without-data-streams",
        // draft21_contribution_d21b.cpp:88,114-123,442 parse_block() reads a received
        // PUBLISH_STATE_NOTIFY (which may carry LOCATION_FILTER, draft 22 section 9.20.9) with the
        // length-prefixed form (b)
        "d21-publish-state-notify-known-largest-object",
        "d21-publish-state-notify-before-first-object",
        "d21-publish-state-notify-preserves-subscriber-control",
        "d21-publish-state-notify-requested-forward-change",
        // draft21_contribution_session.cpp:354-357 FETCH with 0x21 (a)
        "d21-fetch-start-beyond-largest-object",
        // draft21_contribution_session.cpp:466-470 walk_parameters() reads received PUBLISH and
        // PUBLISH_STATE_NOTIFY parameter blocks, 0x21 as length-prefixed (b)
        "d21-publisher-parameter-serialization",
        "d21-publisher-parameter-negotiation",
        "d21-publisher-parameter-multiplicity",
        // draft21_contribution_residual_subscription.cpp:30-40 location_range()/bounded_filter() (a)
        "d21-overlapping-subscriptions-shared-alias",         // lines 68, 70
        "d21-overlapping-subscriptions-distinct-aliases",
        "d21-forward-location-and-range-filter-conjunction",  // lines 130, 135
        // draft21_contribution_residual_subscription.cpp:175-177 fill_whole_track() nests an empty
        // LOCATION_FILTER inside FILL_PARAMETERS (a)
        "d21-fill-fails-before-first-object",                    // line 276
        "d21-cancel-subscription-with-concurrent-fill-streams",  // lines 295, 297
        // draft21_contribution_residual_token.cpp:172-176 param_lp(0x21, whole_group) (a)
        "d21-subgroup-completion-withheld-acknowledgments",
    };
}

}  // namespace moq::interop::requirements
