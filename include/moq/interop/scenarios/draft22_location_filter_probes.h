#pragma once

// Draft 22 own scenarios for row D22-9-20-9-MUST-424 (draft-ietf-moq-transport-22 Section 9.20.9): "If
// StartGroup + EndGroupDelta exceeds 2^64 - 1, the endpoint MUST close the session with a
// PROTOCOL_VIOLATION." Only Location Filter Types 0x03 and 0x04 carry EndGroupDelta. The relative Type 0x01
// is clamped instead ("if greater than 2^64 - 1, it is set to 2^64 - 1"), so it is not this error.
//
// The draft 21 counterparts (d21-location-filter-end-group-overflow and
// d21-fill-location-filter-end-group-overflow, src/scenarios/draft21_close.cpp) send a length-prefixed
// field list that draft 22 cannot express, and are left out of the list on the draft 22 wire. These
// replacements write the draft 22 bytes by hand: wire::draft22::encode_location_filter refuses an overflowing
// filter by design. One scenario sends Type 0x03 at the top level of a SUBSCRIBE, the other Type 0x04 nested
// in FILL_PARAMETERS (Section 9.20.15: a sequence of Parameters with no count, evaluated with the rules of
// Section 9.20.9), so the row is passed only when both Types are rejected.
//
// Unscored probes (no catalog row names them; app::kUnscoredProbeTraits22): their verdicts are recorded as
// run events and never scored.
// - d22-location-filter-unknown-type: Section 9.20.9 "Any other Location Filter Type is a
//   PROTOCOL_VIOLATION" (no BCP 14 keyword, hence no row).
//
// Probes are built on the draft 22 wire only; building one on another wire throws std::logic_error. The
// evaluators judge nothing on another wire.

#include "moq/interop/scenarios/raw_probe.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {

inline constexpr std::string_view kDraft22LocationFilterOverflow = "d22-location-filter-end-group-overflow";
inline constexpr std::string_view kDraft22FillLocationFilterOverflow = "d22-fill-location-filter-end-group-overflow";
inline constexpr std::string_view kDraft22LocationFilterOverflowEvaluator =
    "d22-location-filter-overflow-protocol-violation";
inline constexpr std::string_view kDraft22LocationFilterUnknownType = "d22-location-filter-unknown-type";
inline constexpr std::string_view kDraft22LocationFilterUnknownTypeEvaluator =
    "d22-location-filter-unknown-type-protocol-violation";

// The LOCATION_FILTER value an overflow scenario sends (the bytes after the parameter's type delta): Type
// 0x03 {StartGroup 2^64 - 1, StartObject 0, EndGroupDelta 1} at the top level, Type 0x04 {2^64 - 1, 0, 1,
// EndObject 0} inside FILL_PARAMETERS. Empty for any other id.
std::vector<std::byte> draft22_overflow_filter_value(std::string_view scenario_id);

// One session: a SUBSCRIBE (Request ID 1) for the track whose only parameter is the overflowing filter
// (LOCATION_FILTER Type 0x03), with the draft's liveness follow-up (a fresh SUBSCRIBE for the same track,
// sent after the stimulus had time to act; see src/scenarios/raw_probe_liveness.cpp).
RawProbeDefinition draft22_location_filter_overflow_probe(std::chrono::milliseconds deadline,
                                                          std::vector<std::vector<std::byte>> track_namespace,
                                                          std::vector<std::byte> track_name);

// The same with FILL_PARAMETERS (0x23) as the only parameter: Length, then LOCATION_FILTER (0x21) Type
// 0x04 as the only nested parameter. The subscription itself has no Location Filter and is not paused, so
// the publisher has to evaluate the fill range (Sections 3.4 and 3.4.1).
RawProbeDefinition draft22_fill_location_filter_overflow_probe(std::chrono::milliseconds deadline,
                                                               std::vector<std::vector<std::byte>> track_namespace,
                                                               std::vector<std::byte> track_name);

// Verdict of d22-location-filter-overflow-protocol-violation on a transcript of either overflow scenario,
// with the draft 21 counterpart's rules (scenarios::evaluate_raw_probe_close with PROTOCOL_VIOLATION 0x3):
// true when the publisher closed the session with PROTOCOL_VIOLATION after the stimulus was delivered and
// within the reaction window; false when it closed with any other code there (NO_ERROR included), or when
// it never closed and answered the liveness follow-up with SUBSCRIBE_OK (it kept serving after input that
// required it to close). No value otherwise: another scenario or wire, an unproven stimulus, a
// transport-level, early or late close, no close and no liveness proof (a REQUEST_ERROR alone is not the
// required reaction and proves nothing either way).
std::optional<bool> evaluate_draft22_location_filter_overflow(const RawProbeTranscript& transcript);

// Unscored. One session: a SUBSCRIBE (Request ID 1) for the track whose only parameter is LOCATION_FILTER
// with the undefined Type 0x06 (no fields). No liveness follow-up (see the source).
RawProbeDefinition draft22_location_filter_unknown_type_probe(std::chrono::milliseconds deadline,
                                                              std::vector<std::vector<std::byte>> track_namespace,
                                                              std::vector<std::byte> track_name);

// Verdict of d22-location-filter-unknown-type-protocol-violation (unscored): true for a close with
// PROTOCOL_VIOLATION within the reaction window, false for one with any other code there, no value otherwise
// (in particular a publisher that never closes: without the follow-up nothing proves it kept serving).
std::optional<bool> evaluate_draft22_location_filter_unknown_type(const RawProbeTranscript& transcript);

}  // namespace moq::interop::scenarios
