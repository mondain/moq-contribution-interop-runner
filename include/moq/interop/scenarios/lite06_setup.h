#pragma once

// The moq-lite-06 setup scenarios (L1d Task 4). The publisher under test is the CLIENT; the runner is the server
// and the subscriber, and sends its own Setup stream first except where a named violation probe deliberately
// changes that stream. Every verdict rule, allowance and NotRun condition comes from the catalog rows cited per
// evaluator (requirements/moq-lite-06.json).

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

inline constexpr std::string_view kL06SetupStream = "l06-setup-stream";
inline constexpr std::string_view kL06SetupUnknownParameter = "l06-setup-unknown-parameter";
inline constexpr std::string_view kL06SetupDuplicateParameter = "l06-setup-duplicate-parameter";
inline constexpr std::string_view kL06SetupDuplicateStream = "l06-setup-duplicate-stream";
inline constexpr std::string_view kL06SetupServerPath = "l06-setup-server-path";
inline constexpr std::string_view kL06SetupServerRole = "l06-setup-server-role";

// The undefined Parameter ID (not 0x1-0x5) and its short value sent by l06-setup-unknown-parameter (row 110).
inline constexpr std::uint64_t kL06UnknownParameterId = 0x7f;
inline constexpr std::string_view kL06UnknownParameterValue = "l1d";
// The step labels of the stimuli the evaluators prove.
inline constexpr std::string_view kL06AnnounceLabel = "announce-request";
inline constexpr std::string_view kL06SecondSetupLabel = "second-setup-stream";
// The last step of every probe here: a Wait that executes only once the probe's allowance has elapsed after the
// stimulus (or, for l06-setup-stream, once the publisher's Setup stream ended, its gate, or never: gate_expired
// then records that the allowance elapsed). A peer close before it leaves it unexecuted; the deadline passing
// before it sets timed_out. Its record is how an evaluator knows "still open at the end of the allowance".
inline constexpr std::string_view kL06AllowanceLabel = lite06::kAllowanceLabel;
// The Path value the server-path probe sends (row 126: a plain value such as "/").
inline constexpr std::string_view kL06ServerPathValue = "/";

// Each builder throws std::invalid_argument when `deadline` does not exceed `allowance` (the probe must end by
// its allowance, never by the deadline, so a silent publisher is judged rather than timed out). The allowance is
// a step (kL06AllowanceLabel) followed by a zero observation window, so the probe ends as soon as it executes.

// The runner's ordinary Setup stream, then the allowance step gated on the publisher's Setup stream having ended
// (FIN or reset), expiring `allowance` after the runner's Setup stream was written.
LiteProbeDefinition l06_setup_stream_probe(std::chrono::milliseconds deadline,
                                           std::chrono::milliseconds allowance = kLiteSetupAllowance);
// The runner's SETUP carries Parameter 0x7f = "l1d"; then ANNOUNCE_REQUEST with the empty prefix; ends
// `allowance` after the request was sent (or on a peer close).
LiteProbeDefinition l06_setup_unknown_parameter_probe(std::chrono::milliseconds deadline,
                                                      std::chrono::milliseconds allowance = kLiteResponseAllowance);
// The runner's SETUP carries Cost (0x4) = 1 twice; nothing else.
LiteProbeDefinition l06_setup_duplicate_parameter_probe(std::chrono::milliseconds deadline,
                                                        std::chrono::milliseconds allowance = kLiteCloseAllowance);
// The ordinary runner Setup stream, then a second Setup stream (STREAM_TYPE 0x1, empty SETUP, FIN); nothing else.
LiteProbeDefinition l06_setup_duplicate_stream_probe(std::chrono::milliseconds deadline,
                                                     std::chrono::milliseconds allowance = kLiteCloseAllowance);
// The runner's SETUP carries Path = "/" (only the client may send Path); nothing else.
LiteProbeDefinition l06_setup_server_path_probe(std::chrono::milliseconds deadline,
                                                std::chrono::milliseconds allowance = kLiteCloseAllowance);
// The runner's SETUP carries Role = 0 (Both; only the client may send Role); nothing else.
LiteProbeDefinition l06_setup_server_role_probe(std::chrono::milliseconds deadline,
                                                std::chrono::milliseconds allowance = kLiteCloseAllowance);

// The exact runner Setup stream bytes of each probe (the evaluators compare the recorded stimulus with them).
std::vector<std::byte> l06_unknown_parameter_runner_setup();
std::vector<std::byte> l06_duplicate_parameter_runner_setup();
std::vector<std::byte> l06_server_path_runner_setup();
std::vector<std::byte> l06_server_role_runner_setup();

// L06-3-1-MUST-014 (scenario l06-setup-stream).
std::optional<bool> evaluate_l06_setup_stream_single_setup(const LiteTranscript& transcript);
// L06-7-3-MUST-NOT-111 (scenario l06-setup-stream).
std::optional<bool> evaluate_l06_setup_parameters_unique(const LiteTranscript& transcript);
// L06-7-3-MUST-110 (scenario l06-setup-unknown-parameter).
std::optional<bool> evaluate_l06_setup_unknown_parameter_ignored(const LiteTranscript& transcript);
// L06-7-3-MUST-112 (scenario l06-setup-duplicate-parameter).
std::optional<bool> evaluate_l06_setup_duplicate_parameter_close(const LiteTranscript& transcript);
// L06-6-3-1-MUST-092 (scenario l06-setup-duplicate-stream).
std::optional<bool> evaluate_l06_setup_duplicate_stream_close(const LiteTranscript& transcript);
// L06-7-3-2-MUST-126 (scenario l06-setup-server-path).
std::optional<bool> evaluate_l06_setup_server_path_close(const LiteTranscript& transcript);
// L06-7-3-3-MUST-131 (scenario l06-setup-server-role).
std::optional<bool> evaluate_l06_setup_server_role_close(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
