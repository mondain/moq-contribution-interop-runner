#pragma once

// The moq-lite-06 Track Stream scenario (L2a Task B2). The publisher under test is the CLIENT; the runner is the
// server and the subscriber. Verdict rules, allowances and NotRun conditions come from the catalog rows cited per
// evaluator (requirements/moq-lite-06.json).
//
// l06-track-info needs the track fixture (plan decision (d)): the builder copies the broadcast path and track name
// into the definition (requires_track = true) and the transcript carries them, so the evaluators rebuild the exact
// stimulus bytes. Every probe observes its whole stated window through an UNGATED trailing Wait labelled
// "allowance" (lite06::allowance_step) and never ends early on a condition. The builder throws
// std::invalid_argument when the deadline does not exceed the time the probe needs or a fixture value is empty.
//
// The Track Stream lookup helpers below are shared with the fetch scenarios (lite06_fetch.h): a subscriber must
// already have the track's TRACK_INFO before it parses FETCH frames (draft 5.1.3, 7.16), so those probes begin
// with the same lookup.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/moqlite06/track.h"

namespace moq::interop::scenarios {

inline constexpr std::string_view kL06TrackInfo = "l06-track-info";

// Step labels.
inline constexpr std::string_view kL06TrackFirstLabel = "track-first";        // the lookup every track/fetch probe opens
inline constexpr std::string_view kL06TrackAnsweredLabel = "track-answered";  // Wait gated on the lookup's answer
inline constexpr std::string_view kL06TrackPauseLabel = "track-pause";        // l06-track-info: between the lookups
inline constexpr std::string_view kL06TrackSecondLabel = "track-second";      // l06-track-info: the second lookup

// The pause between the two Track Streams of l06-track-info.
inline constexpr std::chrono::milliseconds kL06TrackPause{500};

// STREAM_TYPE 0x6 then the TRACK message (encode_track_request; empty when it refuses the message).
std::vector<std::byte> l06_track_bytes(std::string_view broadcast_path, std::string_view track_name);

// The Track Stream lookup of a probe: a Track Stream for the fixture (kL06TrackFirstLabel, left open) and a Wait
// (kL06TrackAnsweredLabel) gated on its answer, TRACK_INFO or the stream ended, for at most `answer_allowance`.
void l06_add_track_lookup(LiteProbeDefinition& definition, std::string_view broadcast_path,
                          std::string_view track_name, std::chrono::milliseconds answer_allowance);

// The first lookup's stream when its exact TRACK bytes reached the publisher; nullptr otherwise.
const session::LiteStreamRecord* l06_track_lookup_stream(const LiteTranscript& transcript);
// The TRACK_INFO the first lookup received: nullopt without a reply (a reset included) or with an unreadable one.
std::optional<wire::moqlite06::TrackInfo> l06_track_lookup_info(const LiteTranscript& transcript);

// l06-track-info: announce exchange (ANNOUNCE_REQUEST "" and a Wait gated on its answer, at most `answer_allowance`),
// a Track Stream for the fixture, kL06TrackPause, a second Track Stream for the same track, then the response
// `allowance`. Needs deadline > answer_allowance + kL06TrackPause + allowance.
LiteProbeDefinition l06_track_info_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                         std::string_view track_name,
                                         std::chrono::milliseconds allowance = kLiteResponseAllowance,
                                         std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// L06-7-12-MUST-NOT-163 (scenario l06-track-info).
std::optional<bool> evaluate_l06_track_info_immutable(const LiteTranscript& transcript);
// L06-7-12-MUST-170 (scenario l06-track-info).
std::optional<bool> evaluate_l06_track_info_timescale_nonzero(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
