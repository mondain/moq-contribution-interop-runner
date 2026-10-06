#pragma once

// Draft 22 own scenarios for row D22-3-3-1-MUST-NOT-069 (draft-ietf-moq-transport-22 Section 3.3.1):
// "A publisher MUST NOT send objects from outside the requested range." Each probe asks for the same
// track once per explicit Location Filter Type of Section 9.20.9 (0x01 Relative Start through 0x05 Next
// Object) and judges every Object delivered against the range its request selected, inclusive at both
// ends. Draft 21 counterpart: d21-subscribe-bounded-location-range (src/scenarios/draft21_gap_a.cpp),
// whose single filter is the 0x04 request below.
//
// Fixture contract (the draft 21 gap A contract): the track named by the run is published by the driven
// publisher and retains Group 7 from its first Object, including Object 9. The ranges that are relative
// to the Largest Object (Types 0x01 and 0x05) are resolved with the LARGEST_OBJECT the publisher reports
// in its SUBSCRIBE_OK (Section 9.20.17), which Section 9.6 says lets the subscriber "determine the start
// group/object when not explicitly specified", or for a filter set by REQUEST_UPDATE in the REQUEST_OK
// (REQUEST_UPDATE_OK) that acknowledges it (Sections 9.3 and 9.5.1).
//
// Probes are built on the draft 22 wire only (scenarios::current_wire_draft() == 22); building one on
// another wire throws std::logic_error. The evaluators judge nothing on another wire.

#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {

inline constexpr std::string_view kDraft22SubscribeLocationRange = "d22-subscribe-bounded-location-range";
inline constexpr std::string_view kDraft22UpdateLocationRange = "d22-update-subscription-location-range";
inline constexpr std::string_view kDraft22SubscriptionRangeEvaluator =
    "d22-subscription-objects-within-effective-location-range";

// The filters every probe of this family requests, one request each, in this order: 0x01 (StartGroup 1:
// the Group of the Largest Object), 0x02 {7, 9}, 0x03 {7, 9, EndGroupDelta 0}, 0x04 {7, 9, 0, 9}, 0x05.
std::vector<wire::draft22::LocationFilter> draft22_location_range_filters();

// One session, five SUBSCRIBEs (Request IDs 1, 3, 5, 7, 9, FORWARD=1), each carrying one filter above.
RawProbeDefinition draft22_subscribe_location_range_probe(
    std::chrono::milliseconds deadline, std::vector<std::vector<std::byte>> track_namespace,
    std::vector<std::byte> track_name);

// One session, five SUBSCRIBEs (Request IDs 1..9, FORWARD=0, no filter), then on each request stream, once
// its SUBSCRIBE_OK arrived, a REQUEST_UPDATE (Request IDs 11..19) setting FORWARD=1 and one filter above.
// Relative ranges use the LARGEST_OBJECT of that update's REQUEST_OK. Draft 21 counterpart:
// d21-update-subscription-location-range (one subscription, the 0x04 filter).
RawProbeDefinition draft22_update_location_range_probe(
    std::chrono::milliseconds deadline, std::vector<std::vector<std::byte>> track_namespace,
    std::vector<std::byte> track_name);

// Verdict of d22-subscription-objects-within-effective-location-range on one transcript: false when an
// Object fits no range of the subscriptions it can belong to, true when the observation window ended
// (both subscription scenarios) with every subscription established (for the update scenario: its
// update acknowledged with REQUEST_OK), every range known and at least one Object judged inside, and no
// value otherwise (another scenario, a stimulus that cannot be proven, missing or insufficient evidence).
std::optional<bool> evaluate_draft22_subscription_location_range(const RawProbeTranscript& transcript);

}  // namespace moq::interop::scenarios
