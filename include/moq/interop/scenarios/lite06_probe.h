#pragma once

// The moq-lite-06 Probe scenario (L2a Task B3). The publisher under test is the CLIENT; the runner is the server and
// the subscriber that opens the Probe Stream. Verdict rules, allowances and NotRun conditions come from the catalog
// rows cited per evaluator (requirements/moq-lite-06.json).
//
// l06-probe-report needs no track fixture. The runner opens a Probe Stream with a target (kL06ProbeFirstTarget),
// then, once the publisher reported, reset or ended the stream, or kLiteResponseAllowance passed, a second target
// (kL06ProbeSecondTarget) on the same stream unless the publisher already ended it, and observes `allowance`. The
// continuation is dynamic so that a publisher that resets the stream (a level None publisher, row 075) is never sent
// a write the reset refuses: the stimulus of such a run stays delivered in full and judgeable. Both evaluators read
// the publisher's Probe level from its decoded SETUP (draft 7.3.1: absent equals 0 None) and are mutually exclusive
// by that level.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/wire/moqlite06/probe.h"

namespace moq::interop::scenarios {

inline constexpr std::string_view kL06ProbeReport = "l06-probe-report";

// Step labels.
inline constexpr std::string_view kL06ProbeOpenLabel = "probe-open";      // the Probe Stream and its first target
inline constexpr std::string_view kL06ProbeSecondLabel = "probe-second";  // the second target, on the same stream

// The targets in bits per second (the RTT field is 0: unknown, as a subscriber sends it).
inline constexpr std::uint64_t kL06ProbeFirstTarget = 4'000'000;
inline constexpr std::uint64_t kL06ProbeSecondTarget = 8'000'000;

// STREAM_TYPE 0x4 then the PROBE (encode_probe; empty when it refuses the message).
std::vector<std::byte> l06_probe_open_bytes(std::uint64_t target_bps);
// A second target on an open stream: the PROBE message alone (encode_probe; empty when it refuses).
std::vector<std::byte> l06_probe_message_bytes(std::uint64_t target_bps);

// The publisher's Probe level from its decoded SETUP: 0 None (also when the parameter is absent), 1 Report, 2
// Increase; nullopt without a decoded SETUP or with a malformed Probe parameter (the level is then unknown).
std::optional<std::uint64_t> l06_publisher_probe_level(const LiteTranscript& transcript);

// L2c. The l06-probe-none-reset evaluator (row 075) has nothing to judge when the publisher advertised a Probe
// capability: that path is row 072's. Not a defect of the peer.
bool l06_probe_none_inapplicable(const LiteTranscript& transcript);

// l06-probe-report: the Probe Stream and first target, the continuation above, then the `allowance`. Needs
// deadline > answer_allowance + allowance.
LiteProbeDefinition l06_probe_report_probe(std::chrono::milliseconds deadline,
                                           std::chrono::milliseconds allowance = kLiteResponseAllowance,
                                           std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// L06-5-1-5-MUST-072 (scenario l06-probe-report).
std::optional<bool> evaluate_l06_probe_target_continues(const LiteTranscript& transcript);
// L06-5-1-5-MUST-075 (scenario l06-probe-report).
std::optional<bool> evaluate_l06_probe_none_reset(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
