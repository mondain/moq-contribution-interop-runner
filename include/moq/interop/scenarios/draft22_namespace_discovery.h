#pragma once

// Draft 22 own scenario for row D22-4-2-MUST-110 (draft-ietf-moq-transport-22 Section 4.2): "On success,
// the publisher MUST send a NAMESPACE message for each namespace it knows that matches the Track Namespace
// Prefix". A publisher knows a namespace when it is an Original Publisher for one or more tracks in it;
// matching is Namespace Prefix Matching (Section 2.4.2: every prefix field equals the namespace's field at
// that position). Draft 21 counterpart: d21-discover-original-publisher-namespaces
// (src/scenarios/draft21_gap_a.cpp), which asks once with the empty prefix. The SUBSCRIBE_NAMESPACE and
// NAMESPACE wire is the same in both drafts.
//
// Fixture contract (the draft 21 gap A contract): the driven publisher originally publishes the track the
// run names, so it knows the track's namespace.
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

inline constexpr std::string_view kDraft22DiscoverNamespaces = "d22-discover-original-publisher-namespaces";
inline constexpr std::string_view kDraft22NamespaceDiscoveryEvaluator =
    "d22-original-publisher-matching-namespace-notification";

// The nonmatching prefix field the probe uses for a track namespace whose first field is `first`: `first`
// without its last byte (a byte prefix of the field, which Section 2.4.2's exact field comparison does not
// match), or `first` followed by '-' when it has one byte.
std::vector<std::byte> draft22_nonmatching_prefix_field(const std::vector<std::byte>& first);

// One session (Request IDs in Section 6.4.2.1 server parity):
//   0. SUBSCRIBE (ID 1) for the track, FORWARD=0: names the track (the evaluator recovers it from here)
//      and shows whether the publisher serves it;
//   1. SUBSCRIBE_NAMESPACE (ID 3) with the empty prefix (Section 4.2: zero fields asks for all
//      namespaces);
//   2. once 1 is settled, STOP_SENDING (CANCELLED) on it, which cancels it (Section 4.2), because the empty
//      prefix shares a prefix with every other one (PREFIX_OVERLAP);
//   3. 250 ms later, SUBSCRIBE_NAMESPACE (ID 5), matching prefix: the track namespace's first field;
//   4. SUBSCRIBE_NAMESPACE (ID 7), nonmatching prefix (draft22_nonmatching_prefix_field). The matching and
//      nonmatching prefixes do not share a prefix (neither is a prefix of the other).
// A publisher that ends the empty prefix's response with FIN keeps the cancellation (and so 3 and 4) from
// going out: STOP_SENDING needs the stream open.
RawProbeDefinition draft22_namespace_discovery_probe(std::chrono::milliseconds deadline,
                                                     std::vector<std::vector<std::byte>> track_namespace,
                                                     std::vector<std::byte> track_name);

// Verdict of d22-original-publisher-matching-namespace-notification on one transcript:
//   false when the publisher, having answered the track's SUBSCRIBE with SUBSCRIBE_OK, accepted the empty
//   or the matching prefix with REQUEST_OK and then ended that response with FIN without a NAMESPACE whose
//   prefix plus suffix is the track namespace or covers it (6.4.2.2: a FIN means everything required was
//   sent); judged on the writes that went out when the probe stopped short (see above);
//   true when both the matching and the empty prefix were answered with REQUEST_OK and a NAMESPACE whose
//   prefix plus suffix is exactly the track namespace, and the nonmatching prefix was answered without a
//   NAMESPACE that shows the prefix was ignored or byte-matched (a suffix equal to the track namespace, or
//   to it without its first field);
//   no value otherwise (another scenario, an unproven stimulus, a refused or unanswered request, a stream
//   reset, only a covering namespace, a context that did not settle before its deadline).
std::optional<bool> evaluate_draft22_namespace_discovery(const RawProbeTranscript& transcript);

}  // namespace moq::interop::scenarios
