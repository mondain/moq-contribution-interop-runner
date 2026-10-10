#pragma once

// The moq-lite-06 Goaway scenarios (L2a Task B3). The publisher under test is the CLIENT; the runner is the server
// and sends the GOAWAY (a server may send a non-empty New Session URI, draft 7.18; the publisher client never sends
// one here, so rows about the sender are not exercised). Verdict rules, allowances and NotRun conditions come from
// the catalog rows cited per evaluator (requirements/moq-lite-06.json).
//
// l06-goaway-single needs the track fixture: it subscribes (the announce exchange, then the default SUBSCRIBE) and
// sends the GOAWAY only after the publisher has opened kL06GoawayMinGroups Group streams of its own accord, which is
// the proof that it opens streams on a cadence. l06-goaway-duplicate and l06-goaway-oversize need no fixture and are
// close probes: the peer's session close IS the observation, so their evaluators gate on judgeable() alone through
// lite06::judge_close_probe.
//
// GOAWAY URIs: l06-goaway-single and l06-goaway-duplicate use a non-empty URI on a reserved ".invalid" host, so a
// peer that follows it dials nothing (draft 7.18: an empty URI means "simply close the session", which would hide
// the behaviour under test).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"

namespace moq::interop::scenarios {

inline constexpr std::string_view kL06GoawaySingle = "l06-goaway-single";
inline constexpr std::string_view kL06GoawayDuplicate = "l06-goaway-duplicate";
inline constexpr std::string_view kL06GoawayOversize = "l06-goaway-oversize";

// Step labels.
inline constexpr std::string_view kL06GoawayLabel = "goaway";                // l06-goaway-single
inline constexpr std::string_view kL06GoawayFirstLabel = "goaway-first";     // l06-goaway-duplicate
inline constexpr std::string_view kL06GoawaySettleLabel = "goaway-settle";    // the pause between the two
inline constexpr std::string_view kL06GoawaySecondLabel = "goaway-second";
inline constexpr std::string_view kL06GoawayOversizeLabel = "goaway-oversize";
inline constexpr std::string_view kL06GoawayLearnLabel = "goaway-subscribe";  // l06-goaway-single's SUBSCRIBE

// The URI the runner's valid GOAWAYs carry.
inline constexpr std::string_view kL06GoawayUri = "https://goaway-target.invalid/moq";
// The Subscribe ID of l06-goaway-single's SUBSCRIBE.
inline constexpr std::uint64_t kL06GoawaySubscribeId = 0;
// Group streams the publisher must have opened before the GOAWAY (the cadence proof). Three, because the first is
// usually the group already in progress when the SUBSCRIBE lands: only the gaps from the second on are whole GOPs.
inline constexpr std::size_t kL06GoawayMinGroups = 3;
// The URI length l06-goaway-oversize claims: one above the draft 7.18 limit of 8,192 bytes.
inline constexpr std::size_t kL06GoawayOversizeLength = 8193;
// The flight time allowed to a stream the publisher opened before it saw the GOAWAY, in the draft's silence.
inline constexpr std::chrono::milliseconds kL06GoawayNewStreamAllowance{2000};
// The pause between the two GOAWAYs of l06-goaway-duplicate.
inline constexpr std::chrono::milliseconds kL06GoawaySettle{500};
// The wait for the cadence proof of l06-goaway-single.
inline constexpr std::chrono::milliseconds kL06GoawayCadenceAllowance{10000};

// STREAM_TYPE 0x5 then the GOAWAY (encode_goaway; empty when it refuses the message).
std::vector<std::byte> l06_goaway_bytes(std::string_view new_session_uri);
// STREAM_TYPE 0x5 then a GOAWAY whose URI is kL06GoawayOversizeLength bytes, built raw (encode_goaway refuses it): the
// named deliberate violation of l06-goaway-oversize.
std::vector<std::byte> l06_goaway_oversize_bytes();

// l06-goaway-single: announce exchange, the default SUBSCRIBE, the continuation that sends the GOAWAY once
// kL06GoawayMinGroups Group streams were opened (no cadence within `cadence_allowance`, the subscription ended or
// the session closed: no GOAWAY, NotRun) and observes `window`. Needs
// deadline > answer_allowance + cadence_allowance + window.
LiteProbeDefinition l06_goaway_single_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                            std::string_view track_name,
                                            std::chrono::milliseconds window = kLiteObservationWindow,
                                            std::chrono::milliseconds cadence_allowance = kL06GoawayCadenceAllowance,
                                            std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);
// l06-goaway-duplicate: a Goaway Stream with a valid GOAWAY, kL06GoawaySettle, a second Goaway Stream with another,
// then the close `allowance`. Needs deadline > kL06GoawaySettle + allowance.
LiteProbeDefinition l06_goaway_duplicate_probe(std::chrono::milliseconds deadline,
                                               std::chrono::milliseconds allowance = kLiteCloseAllowance);
// l06-goaway-oversize: one Goaway Stream with the oversize GOAWAY, then the close `allowance`. Needs
// deadline > allowance.
LiteProbeDefinition l06_goaway_oversize_probe(std::chrono::milliseconds deadline,
                                              std::chrono::milliseconds allowance = kLiteCloseAllowance);

// L06-5-1-6-MUST-NOT-077 (scenario l06-goaway-single).
std::optional<bool> evaluate_l06_goaway_no_new_streams(const LiteTranscript& transcript);
// L06-7-18-MUST-186 (scenario l06-goaway-duplicate).
std::optional<bool> evaluate_l06_goaway_second_closes(const LiteTranscript& transcript);
// L06-7-18-MUST-179 (scenario l06-goaway-oversize).
std::optional<bool> evaluate_l06_goaway_oversize_violation(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
