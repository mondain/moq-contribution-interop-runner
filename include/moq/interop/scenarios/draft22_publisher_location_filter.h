#pragma once

// Draft 22 own scenario for row D22-9-20-9-MAY-422 (draft-ietf-moq-transport-22 Section 9.20.9): "The
// LOCATION_FILTER parameter (Parameter Type 0x21) MAY appear in a FETCH, SUBSCRIBE, PUBLISH,
// REQUEST_UPDATE (for a subscription) or PUBLISH_STATE_NOTIFY message." The publisher's side of that
// permission is PUBLISH, a REQUEST_UPDATE it sends for its own PUBLISH (Section 9.5: "The sender of a
// request ... can later send a REQUEST_UPDATE on the same bidi stream"), and PUBLISH_STATE_NOTIFY (Section
// 9.10), which "reports the Location Filter now in effect at the publisher". Draft 22 writes the filter as
// an explicit Location Filter Type (0x00 to 0x05) and the fields it selects; "Any other Location Filter
// Type is a PROTOCOL_VIOLATION". The draft 21 counterpart (d21-publisher-location-filter-parameter) has no
// implementation; the shape follows the draft 21 PUBLISH_STATE_NOTIFY probes
// (d21-publish-state-notify-preserves-subscriber-control in src/scenarios/draft21_contribution_d21b.cpp).
//
// Probes are built on the draft 22 wire only; building one on another wire throws std::logic_error. The
// evaluator judges nothing on another wire.

#include "moq/interop/scenarios/raw_probe.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {

inline constexpr std::string_view kDraft22PublisherLocationFilter = "d22-publisher-location-filter-parameter";
inline constexpr std::string_view kDraft22PublisherLocationFilterEvaluator = "d22-publisher-location-filter-capability";

// One session: SUBSCRIBE (Request ID 1) for the track with FORWARD=1 and LOCATION_FILTER Type 0x02
// (Absolute Start) {7, 0}, then the window. PUBLISH requests the publisher opens are accepted with
// REQUEST_OK and its REQUEST_UPDATEs on them answered with REQUEST_OK (the courtesy responder), so a
// publisher that publishes or updates proactively can go on doing so.
RawProbeDefinition draft22_publisher_location_filter_probe(std::chrono::milliseconds deadline,
                                                           std::vector<std::vector<std::byte>> track_namespace,
                                                           std::vector<std::byte> track_name);

// Verdict of d22-publisher-location-filter-capability on one transcript. The publisher messages judged are
// PUBLISH_STATE_NOTIFY on the probe's subscription (after its SUBSCRIBE_OK), and PUBLISH, REQUEST_UPDATE and
// PUBLISH_STATE_NOTIFY on request streams the publisher opened with PUBLISH.
//   false as soon as one carries a LOCATION_FILTER that does not decode (an undefined Type, missing fields,
//   or StartGroup + EndGroupDelta past 2^64 - 1), or a PUBLISH_STATE_NOTIFY on the probe's subscription
//   reports an absolute filter (Types 0x00, 0x02 to 0x04) other than the one requested (Section 9.10: a
//   publisher "MUST NOT use PUBLISH_STATE_NOTIFY to change the value of a subscriber controlled
//   subscription parameter unless the subscriber requested the change", and the probe requests none);
//   true when the window ended and at least one LOCATION_FILTER was judged, every one decoded, and every one
//   on the probe's subscription is the requested filter;
//   no value otherwise: another scenario, an unproven stimulus, no LOCATION_FILTER sent (the permission was
//   not exercised, as the draft 21 PUBLISH_STATE_NOTIFY probes leave a row unscored without a
//   notification), a relative filter (Types 0x01, 0x05) on the probe's subscription, whose range needs a
//   Largest Object to compare, or a judged message whose parameters could not all be read.
std::optional<bool> evaluate_draft22_publisher_location_filter(const RawProbeTranscript& transcript);

}  // namespace moq::interop::scenarios
