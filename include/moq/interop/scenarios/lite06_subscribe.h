#pragma once

// The moq-lite-06 subscribe scenarios (L1d Task 6). The publisher under test is the CLIENT; the runner is the
// server and the subscriber. Every verdict rule, allowance and NotRun condition comes from the catalog rows cited
// per evaluator (requirements/moq-lite-06.json).
//
// All five scenarios need the track fixture (plan decision (d)): the broadcast path (RunConfig::track namespace
// fields joined with '/') and the track name. The builders copy both into the definition (requires_track = true)
// and the transcript carries them, so the evaluators rebuild the exact stimulus bytes.
//
// Every SUBSCRIBE the runner sends conforms (draft 3.6, 7.9: Frame Start 0 unless the abutting second subscription
// names a group the runner received, Frame End 0 whenever Group End is 0), except the deliberate invalid-bounds
// probe (row L06-3-6-MUST-023). Each states kL06LargeMaxAgeMs as Subscriber Max Age (rows 159 and 190: staleness
// resets cannot explain a gap), except the LEARNING subscription of the floor and abutting probes, which states
// Max Age 0: under draft 7.9 (2019-2023) a publisher holding history SHOULD start a large-Max-Age subscription at
// its oldest unexpired group, and only Max Age 0 starts at the latest group, which is what those probes learn. Every
// SUBSCRIBE gets a new Subscribe ID within the session (row L06-7-9-MUST-NOT-154).
//
// Deadline and windows: every probe observes its whole stated window through an UNGATED trailing Wait labelled
// "allowance" (lite06::allowance_step) and never ends early on a condition, so a late violation is still seen. Each
// builder throws std::invalid_argument when the deadline does not exceed the time the probe needs (listed per
// builder) or when a fixture value is empty. Pass a deadline of at least that plus a margin (a second or more live).
//
// l06-subscribe-group-floor and l06-subscribe-abutting-frame-start learn the latest group from a default
// subscription first, through the engine's dynamic next_steps continuation. The continuation runs only when the
// recorder changes: a learning step whose allowance passed is closed on the next change; a publisher that never
// sends anything more (a silent learning subscription) leaves the continuation open until the deadline, which sets
// timed_out, so every evaluator of the probe is NotRun. Task 9 must treat timed_out on these two probes as NotRun,
// NOT as a harness error, and give them deadlines of at least the stated sums plus a margin.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/wire/moqlite06/subscribe.h"

namespace moq::interop::scenarios {

inline constexpr std::string_view kL06SubscribeLatest = "l06-subscribe-latest";
inline constexpr std::string_view kL06SubscribeRefused = "l06-subscribe-refused";
inline constexpr std::string_view kL06SubscribeInvalidFrameBounds = "l06-subscribe-invalid-frame-bounds";
inline constexpr std::string_view kL06SubscribeGroupFloor = "l06-subscribe-group-floor";
inline constexpr std::string_view kL06SubscribeAbuttingFrameStart = "l06-subscribe-abutting-frame-start";

// The Subscriber Max Age every runner SUBSCRIBE states (one hour, in milliseconds), except the learning one.
inline constexpr std::uint64_t kL06LargeMaxAgeMs = 3'600'000;
// The Subscriber Max Age of the learning SUBSCRIBE (floor and abutting probes): 0 starts at the latest group.
inline constexpr std::uint64_t kL06LearnMaxAgeMs = 0;

// Step labels.
inline constexpr std::string_view kL06SubAnnounceLabel = "announce-request";   // ANNOUNCE_REQUEST "" (latest, refused)
inline constexpr std::string_view kL06SubAnnouncedLabel = "announced";         // Wait gated on the announce answer
inline constexpr std::string_view kL06SubscribeLatestLabel = "subscribe-latest";
inline constexpr std::string_view kL06SubscribeUncoveredLabel = "subscribe-uncovered-path";
inline constexpr std::string_view kL06SubscribeUnknownTrackLabel = "subscribe-unknown-track";
inline constexpr std::string_view kL06SubscribeInvalidLabel = "subscribe-invalid-bounds";
inline constexpr std::string_view kL06SubscribeLearnLabel = "subscribe-learn";        // floor and abutting probes
inline constexpr std::string_view kL06FloorAtLatestLabel = "subscribe-floor-latest";  // F = L
inline constexpr std::string_view kL06FloorAboveLabel = "subscribe-floor-above";      // F = L + 2
inline constexpr std::string_view kL06AbuttingFirstLabel = "subscribe-abutting-first";
inline constexpr std::string_view kL06AbuttingSecondLabel = "subscribe-abutting-second";
inline constexpr std::string_view kL06SubscribeAllowanceLabel = lite06::kAllowanceLabel;

// Subscribe IDs (each new within its session).
inline constexpr std::uint64_t kL06LatestSubscribeId = 0;
inline constexpr std::uint64_t kL06UncoveredSubscribeId = 0;
inline constexpr std::uint64_t kL06UnknownTrackSubscribeId = 1;
inline constexpr std::uint64_t kL06InvalidSubscribeId = 0;
inline constexpr std::uint64_t kL06LearnSubscribeId = 0;
inline constexpr std::uint64_t kL06FloorAtLatestSubscribeId = 1;
inline constexpr std::uint64_t kL06FloorAboveSubscribeId = 2;
inline constexpr std::uint64_t kL06AbuttingFirstSubscribeId = 1;
inline constexpr std::uint64_t kL06AbuttingSecondSubscribeId = 2;
// The abutting split N: the first subscription ends at frame N-1 of group G, the second starts at frame N.
inline constexpr std::uint64_t kL06AbuttingFrameSplit = 1;
// The Frame End (wire value, absolute frame + 1) of the invalid-bounds probe, sent with Group End 0.
inline constexpr std::uint64_t kL06InvalidFrameEnd = 1;

// The broadcast path of row 062 case (a): under l06_disjoint_prefix, so no route covering the configured broadcast
// covers it.
std::string l06_uncovered_path(std::string_view broadcast_path);
// The track name of row 062 case (b): the configured track with a suffix the broadcast does not serve.
std::string l06_unknown_track(std::string_view track_name);

// The ANNOUNCE_REQUEST "" stream bytes (STREAM_TYPE then the request) every fixture scenario that needs the
// publisher's announced track starts with.
std::vector<std::byte> l06_announce_all_bytes();
// ANNOUNCE_REQUEST "" (label kL06SubAnnounceLabel, left open) and a Wait (kL06SubAnnouncedLabel) gated on its answer,
// ANNOUNCE_OK and the initial set or the stream ended, for at most `answer_allowance`.
void l06_add_announce_exchange(LiteProbeDefinition& definition, std::chrono::milliseconds answer_allowance);

// A conforming SUBSCRIBE for the fixture: Subscriber Priority 0, kL06LargeMaxAgeMs, the given bounds.
wire::moqlite06::Subscribe l06_subscribe(std::uint64_t subscribe_id, std::string_view broadcast_path,
                                         std::string_view track_name, std::uint64_t group_start = 0,
                                         std::uint64_t group_end = 0, std::uint64_t frame_start = 0,
                                         std::uint64_t frame_end = 0);
// STREAM_TYPE 0x2 then the SUBSCRIBE (encode_subscribe; empty when it refuses the message).
std::vector<std::byte> l06_subscribe_bytes(const wire::moqlite06::Subscribe& subscribe);
// The learning SUBSCRIBE of the floor and abutting probes: id kL06LearnSubscribeId, default range, Subscriber Max
// Age kL06LearnMaxAgeMs (0), so the publisher starts it at its latest group.
wire::moqlite06::Subscribe l06_learning_subscribe(std::string_view broadcast_path, std::string_view track_name);
std::vector<std::byte> l06_learning_subscribe_bytes(std::string_view broadcast_path, std::string_view track_name);
// The invalid-bounds stimulus built raw (encode_subscribe refuses it): Group End 0, Frame End kL06InvalidFrameEnd.
std::vector<std::byte> l06_invalid_bounds_subscribe_bytes(std::string_view broadcast_path,
                                                          std::string_view track_name);
// The SUBSCRIBE a recorded Subscribe-stream stimulus carries (STREAM_TYPE 0x2 then a whole SUBSCRIBE, nothing
// after); nullopt otherwise.
std::optional<wire::moqlite06::Subscribe> l06_decode_subscribe_stimulus(std::span<const std::byte> bytes);

// l06-subscribe-latest: ANNOUNCE_REQUEST "" and a Wait gated on its answer (ANNOUNCE_OK and the initial set, or
// the stream ended; at most `answer_allowance`), then the default SUBSCRIBE (id 0: latest group, unbounded, large
// Max Age) and the observation `window`. Needs deadline > answer_allowance + window.
LiteProbeDefinition l06_subscribe_latest_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                               std::string_view track_name,
                                               std::chrono::milliseconds window = kLiteObservationWindow,
                                               std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);
// l06-subscribe-refused: the announce exchange as above, then two SUBSCRIBEs opened at once: (a) id 0 for
// l06_uncovered_path with the fixture's track, (b) id 1 for the fixture's broadcast with l06_unknown_track; then
// the response `allowance`. Needs deadline > answer_allowance + allowance.
LiteProbeDefinition l06_subscribe_refused_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                                std::string_view track_name,
                                                std::chrono::milliseconds allowance = kLiteResponseAllowance,
                                                std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);
// l06-subscribe-invalid-frame-bounds: the raw SUBSCRIBE with Group End 0 and a non-zero Frame End (the named
// deliberate violation), then the response `allowance`. Needs deadline > allowance.
LiteProbeDefinition l06_subscribe_invalid_frame_bounds_probe(
    std::chrono::milliseconds deadline, std::string_view broadcast_path, std::string_view track_name,
    std::chrono::milliseconds allowance = kLiteResponseAllowance);
// l06-subscribe-group-floor: the learning SUBSCRIBE (id 0, default range, Max Age 0); once a GROUP of it decodes,
// L is the highest Group Sequence it delivered so far, and the continuation opens two floored SUBSCRIBEs at once
// (id 1 with Group Start F = L, id 2 with F = L + 2; Frame Start 0, Group End 0, Frame End 0, large Max Age)
// followed by the `window`. No GROUP within `learn_allowance` (seen on the next change), or the learning stream
// ended or reset: no floored subscription is sent (NotRun). Needs deadline > learn_allowance + window.
LiteProbeDefinition l06_subscribe_group_floor_probe(std::chrono::milliseconds deadline,
                                                    std::string_view broadcast_path, std::string_view track_name,
                                                    std::chrono::milliseconds window = kLiteObservationWindow,
                                                    std::chrono::milliseconds learn_allowance = kLiteResponseAllowance);
// l06-subscribe-abutting-frame-start (draft 3.6): the learning SUBSCRIBE (id 0, default range, Max Age 0) gives G
// (the highest Group Sequence decoded on it when its first GROUP decodes); the first subscription (id 1: Group Start
// G, Group End G+1 on the wire, Frame End N on the wire, i.e. frames 0..N-1 of G with N = kL06AbuttingFrameSplit);
// once frames 0..N-1 of group G decoded on it, the second (id 2: Group Start G, Frame Start N, unbounded) and the
// `window`. G = 0 is a valid pattern: Group Start 0 then also reads as "no floor", but draft 3.6 lets a Frame Start
// qualify group 0 ("group 0 included"), so the second subscription still names frame N of group 0. When a stage
// fails (no GROUP within `step_allowance`, the first subscription answered with another group or a non-zero Frame
// Start, a stream ended or reset first) the continuation sends no second subscription and still observes the
// `window` (the default subscription stays judged). Needs deadline > 2 * step_allowance + window.
LiteProbeDefinition l06_subscribe_abutting_frame_start_probe(
    std::chrono::milliseconds deadline, std::string_view broadcast_path, std::string_view track_name,
    std::chrono::milliseconds window = kLiteObservationWindow,
    std::chrono::milliseconds step_allowance = kLiteResponseAllowance);

// L06-6-3-2-MUST-093 (scenario l06-subscribe-latest).
std::optional<bool> evaluate_l06_group_starts_with_group(const LiteTranscript& transcript);
// L06-6-3-2-MUST-NOT-097 (scenario l06-subscribe-latest).
std::optional<bool> evaluate_l06_group_unique_sequence(const LiteTranscript& transcript);
// L06-7-19-SHOULD-190 (scenario l06-subscribe-latest).
std::optional<bool> evaluate_l06_group_sequence_increments(const LiteTranscript& transcript);
// L06-5-1-2-MUST-062 (scenario l06-subscribe-refused).
std::optional<bool> evaluate_l06_subscribe_refused_reset(const LiteTranscript& transcript);
// L06-3-6-MUST-023 (scenario l06-subscribe-invalid-frame-bounds).
std::optional<bool> evaluate_l06_subscribe_invalid_frame_bounds_reset(const LiteTranscript& transcript);
// L06-7-9-MUST-NOT-159 (scenario l06-subscribe-group-floor).
std::optional<bool> evaluate_l06_subscribe_no_group_below_floor(const LiteTranscript& transcript);
// L06-7-13-MUST-172 (scenario l06-subscribe-group-floor).
std::optional<bool> evaluate_l06_subscribe_ok_group_at_floor(const LiteTranscript& transcript);
// L06-3-6-MUST-020 (scenario l06-subscribe-abutting-frame-start).
std::optional<bool> evaluate_l06_subscribe_resolved_start(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
