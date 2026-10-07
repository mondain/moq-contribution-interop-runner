#pragma once

// The moq-lite-06 announce and session scenarios (L1d Task 5). The publisher under test is the CLIENT; the runner
// is the server and the subscriber. Every verdict rule, allowance and NotRun condition comes from the catalog rows
// cited per evaluator (requirements/moq-lite-06.json).
//
// l06-announce-prefix, l06-announce-lifecycle and l06-session-stream-close need the track fixture (plan decision
// (d)): the broadcast path (RunConfig::track namespace fields joined with '/') and, for the SUBSCRIBE of
// l06-session-stream-close, the track name. The builders copy them into the definition (requires_track = true) and
// the transcript carries them, so the evaluators rebuild the exact stimulus and judge route coverage.
//
// Deadline and allowance: like the setup probes, every probe ends with an UNGATED Wait labelled "allowance"
// (lite06::allowance_step) and a zero observation window, and each builder throws std::invalid_argument when the
// deadline does not exceed the time the probe needs (its allowance, plus the answer allowance for
// l06-session-stream-close) or when a required fixture value is empty. Pass a deadline of at least that plus a
// margin (a second or more live): the allowance runs from the allowance step's current_at, the deadline from
// establishment.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"

namespace moq::interop::scenarios {

inline constexpr std::string_view kL06AnnouncePrefix = "l06-announce-prefix";
inline constexpr std::string_view kL06AnnounceLifecycle = "l06-announce-lifecycle";
inline constexpr std::string_view kL06SessionStreamClose = "l06-session-stream-close";

// Step labels of l06-announce-prefix: the three ANNOUNCE_REQUESTs (the first two are opened at once).
inline constexpr std::string_view kL06AnnounceEmptyLabel = "announce-empty";
inline constexpr std::string_view kL06AnnounceBroadcastLabel = "announce-broadcast";
inline constexpr std::string_view kL06AnnounceDisjointLabel = "announce-disjoint";
// Step label of the ANNOUNCE_REQUEST of l06-announce-lifecycle and l06-session-stream-close.
inline constexpr std::string_view kL06AnnounceRequestLabel = "announce-request";
// Step labels of l06-session-stream-close.
inline constexpr std::string_view kL06SubscribeRequestLabel = "subscribe-request";
inline constexpr std::string_view kL06AnswersLabel = "answers";
inline constexpr std::string_view kL06AnnounceFinLabel = "announce-fin";
inline constexpr std::string_view kL06SubscribeFinLabel = "subscribe-fin";
// The trailing allowance step of every probe here.
inline constexpr std::string_view kL06AnnounceAllowanceLabel = lite06::kAllowanceLabel;
// The Subscribe ID of the SUBSCRIBE sent by l06-session-stream-close.
inline constexpr std::uint64_t kL06StreamCloseSubscribeId = 0;

// Path segments, split on '/', empty segments dropped (moq.dev trims and joins with a single '/').
std::vector<std::string> l06_path_segments(std::string_view path);
// The prefix of the broadcast path that l06-announce-prefix (and l06-announce-lifecycle) request: its first
// segment ("demo" for "demo/live"; the path itself when it has one segment).
std::string l06_broadcast_prefix(std::string_view broadcast_path);
// A prefix disjoint from the broadcast path in both directions: its first segment differs from the path's.
std::string l06_disjoint_prefix(std::string_view broadcast_path);
// Draft 5.1.1: a route's full prefix is the requested prefix joined with the Route Prefix Suffix; it covers a path
// when its segments are a leading run of the path's segments (matching is per segment).
bool l06_route_covers(std::string_view request_prefix, std::string_view suffix, std::string_view broadcast_path);

// The exact runner stream bytes (STREAM_TYPE then the request) the evaluators compare the stimuli with.
std::vector<std::byte> l06_announce_request_bytes(std::string_view prefix);
std::vector<std::byte> l06_stream_close_subscribe_bytes(std::string_view broadcast_path, std::string_view track_name);

// l06-announce-prefix: ANNOUNCE_REQUESTs for the empty prefix and for l06_broadcast_prefix (opened at once, so two
// overlapping requests are live together), then for l06_disjoint_prefix; then the allowance (the stated window in
// which every response message is decoded). The request streams stay open.
LiteProbeDefinition l06_announce_prefix_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                              std::chrono::milliseconds allowance = kLiteResponseAllowance);
// l06-announce-lifecycle: one ANNOUNCE_REQUEST for l06_broadcast_prefix held open for the window, while the
// adapter or driver may end and restart the broadcast (the adapter contract cannot ask for that yet).
LiteProbeDefinition l06_announce_lifecycle_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                                 std::chrono::milliseconds window = kLiteObservationWindow);
// l06-session-stream-close: an ANNOUNCE_REQUEST for the empty prefix and a SUBSCRIBE for the fixture's track; a
// Wait gated on the publisher's first answer on both (bounded by `answer_allowance`, labelled "answers"); then the
// runner FINs both streams (its send direction) and observes for `allowance`.
LiteProbeDefinition l06_session_stream_close_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                                   std::string_view track_name,
                                                   std::chrono::milliseconds allowance = kLiteCloseAllowance,
                                                   std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// L06-7-4-MUST-139 (scenario l06-announce-prefix).
std::optional<bool> evaluate_l06_announce_ok_then_starts(const LiteTranscript& transcript);
// L06-7-5-SHOULD-143 (scenario l06-announce-prefix).
std::optional<bool> evaluate_l06_announce_ok_hop_assigned(const LiteTranscript& transcript);
// L06-7-5-MUST-NOT-141 (scenario l06-announce-prefix).
std::optional<bool> evaluate_l06_announce_hop_list_excludes_own(const LiteTranscript& transcript);
// L06-7-7-MUST-NOT-152 (scenario l06-announce-lifecycle).
std::optional<bool> evaluate_l06_announce_retired_id_unused(const LiteTranscript& transcript);
// L06-4-3-MUST-025 (scenario l06-session-stream-close).
std::optional<bool> evaluate_l06_session_peer_closes_send(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
