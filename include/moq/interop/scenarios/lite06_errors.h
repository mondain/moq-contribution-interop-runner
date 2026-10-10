#pragma once

// The moq-lite-06 error-handling scenarios and the client-path scenario (L1d Task 7). The publisher under test is the
// CLIENT; the runner is the server and the subscriber. Every verdict rule, allowance and NotRun condition comes from
// the catalog rows cited per evaluator (requirements/moq-lite-06.json).
//
// Deadline and windows: every probe observes its whole stated allowance through an UNGATED trailing Wait labelled
// "allowance" (lite06::allowance_step) and never ends early on a condition, so a late violation is still seen. Each
// builder throws std::invalid_argument when an allowance is not positive, when the deadline does not exceed the time
// the probe needs (listed per builder) or, for the track scenarios, when a fixture value is empty. Pass a deadline of
// at least that plus a margin (a second or more live).
//
// l06-errors-unknown-reset-code and l06-errors-reserved-reset-code need the track fixture (requires_track): they
// cancel one of TWO live subscriptions and the other proves the session stays usable. l06-errors-unknown-stream-type,
// l06-errors-code-space and l06-setup-client-path need no fixture.

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

inline constexpr std::string_view kL06ErrorsUnknownStreamType = "l06-errors-unknown-stream-type";
inline constexpr std::string_view kL06ErrorsUnknownResetCode = "l06-errors-unknown-reset-code";
inline constexpr std::string_view kL06ErrorsReservedResetCode = "l06-errors-reserved-reset-code";
// kL06ErrorsCodeSpace ("l06-errors-code-space") is declared in lite06_common.h with its shared evaluator.
inline constexpr std::string_view kL06SetupClientPath = "l06-setup-client-path";

// The unregistered STREAM_TYPE of row 108's probe (not 0x0-0x6; recorded in the step bytes).
inline constexpr std::uint64_t kL06UnregisteredStreamType = 0x3f;
// The unregistered application code (64 or above) of row 030/032's cancel.
inline constexpr std::uint64_t kL06UnknownErrorCode = 0x4d1;
// The reserved code (32 through 47) of row 033's cancel.
inline constexpr std::uint64_t kL06ReservedErrorCode = 0x2a;
// The session code UNAUTHORIZED (draft 4.4.1), the only close row 032 fails.
inline constexpr std::uint64_t kL06Unauthorized = 0x2;

// The broadcast and track the code-space probe subscribes to (a broadcast a publisher does not serve).
inline constexpr std::string_view kL06UnservedBroadcast = "l1d-unserved/code-space";
inline constexpr std::string_view kL06UnservedTrack = "l1d-unserved-track";
// The bytes the code-space probe's ANNOUNCE_REQUEST carries after its prefix, inside its Message Length.
inline constexpr std::string_view kL06LengthTrailing = "l1d";

// Step labels.
inline constexpr std::string_view kL06UnknownStreamLabel = "unknown-stream";
inline constexpr std::string_view kL06ErrAnnounceLabel = "announce-request";
inline constexpr std::string_view kL06CancelledLabel = "subscribe-cancelled";  // subscription A (cancelled)
inline constexpr std::string_view kL06KeptLabel = "subscribe-kept";            // subscription B (kept)
inline constexpr std::string_view kL06LiveLabel = "live";                      // Wait gated on A and B answered
inline constexpr std::string_view kL06StopGroupLabel = "stop-group";           // STOP_SENDING on a Group stream of A
inline constexpr std::string_view kL06CancelResetLabel = "cancel-reset";       // RESET_STREAM on A
inline constexpr std::string_view kL06CancelStopLabel = "cancel-stop";         // STOP_SENDING on A
inline constexpr std::string_view kL06LaterLabel = "subscribe-later";          // subscription C (after the cancel)
inline constexpr std::string_view kL06UnservedLabel = "subscribe-unserved";
inline constexpr std::string_view kL06RefusedLabel = "refused";                // Wait gated on the refusal
inline constexpr std::string_view kL06LengthLabel = "announce-length";
inline constexpr std::string_view kL06ErrorsAllowanceLabel = lite06::kAllowanceLabel;

// Subscribe IDs of the reset-code probes (each new within its session).
inline constexpr std::uint64_t kL06CancelledSubscribeId = 0;
inline constexpr std::uint64_t kL06KeptSubscribeId = 1;
inline constexpr std::uint64_t kL06LaterSubscribeId = 2;
// The Subscribe ID of the code-space probe's SUBSCRIBE.
inline constexpr std::uint64_t kL06UnservedSubscribeId = 0;

// STREAM_TYPE kL06UnregisteredStreamType and nothing else (row 108's stimulus).
std::vector<std::byte> l06_unknown_stream_type_bytes();
// STREAM_TYPE 0x1 then an ANNOUNCE_REQUEST whose Message Length covers the empty prefix plus kL06LengthTrailing
// (row 107's stimulus; built raw, encode_announce_request writes no trailing bytes).
std::vector<std::byte> l06_message_length_extra_bytes();
// The code-space probe's SUBSCRIBE (id kL06UnservedSubscribeId, kL06UnservedBroadcast / kL06UnservedTrack).
std::vector<std::byte> l06_unserved_subscribe_bytes();
// The publisher's expected SETUP Path value (draft 7.3.2): path-abempty, then '?' and the query when non-empty.
std::string l06_expected_client_path(std::string_view url_path, std::string_view url_query);

// l06-errors-unknown-stream-type (rows 108, 109): after the setup exchange a bidirectional stream carrying only
// STREAM_TYPE kL06UnregisteredStreamType (no FIN), then a conforming ANNOUNCE_REQUEST "" on a new Announce Stream,
// then the `allowance` (the stated window of 109 and the reaction allowance of 108). Needs deadline > allowance.
LiteProbeDefinition l06_errors_unknown_stream_type_probe(std::chrono::milliseconds deadline,
                                                         std::chrono::milliseconds allowance = kLiteResponseAllowance);

// l06-errors-unknown-reset-code (rows 030, 032): two SUBSCRIBEs for the fixture opened at once (A id 0, B id 1,
// default range, large Max Age); a Wait gated on both answered with SUBSCRIBE_OK and neither ended (at most
// `answer_allowance`); a STOP_SENDING with kL06UnknownErrorCode on an open Group stream of A (gated on one existing,
// at most `answer_allowance`; the row 098 note's non-fatal check, credited to no row); RESET_STREAM and STOP_SENDING
// of A with kL06UnknownErrorCode; a later SUBSCRIBE C (id 2) for the fixture; the `allowance`.
// Needs deadline > 2 * answer_allowance + allowance. The Group stream STOP_SENDING targets only a stream still open
// (no FIN, no reset); should a live transport still refuse it with InvalidState, the engine sets harness_failed and
// every evaluator is NotRun: Task 9 maps that to NotRun / a stored harness error, never a verdict.
LiteProbeDefinition l06_errors_unknown_reset_code_probe(
    std::chrono::milliseconds deadline, std::string_view broadcast_path, std::string_view track_name,
    std::chrono::milliseconds allowance = kLiteResponseAllowance,
    std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// l06-errors-reserved-reset-code (row 033): as above without the Group stream STOP_SENDING, with
// kL06ReservedErrorCode. Needs deadline > answer_allowance + allowance.
LiteProbeDefinition l06_errors_reserved_reset_code_probe(
    std::chrono::milliseconds deadline, std::string_view broadcast_path, std::string_view track_name,
    std::chrono::milliseconds allowance = kLiteResponseAllowance,
    std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// l06-errors-code-space (rows 027, 107): the stream half first, a SUBSCRIBE for kL06UnservedBroadcast (a broadcast
// the publisher does not serve) and a Wait gated on its refusal (the publisher resets, stops or ends that stream; at
// most `answer_allowance`); then the session half, the l06_message_length_extra_bytes ANNOUNCE_REQUEST; then the
// close `allowance`. The stream half comes first so the session close cannot discard its code. When the `refused`
// gate expired (no refusal within `answer_allowance`) a later close might still answer the SUBSCRIBE; the step
// record's gate_expired says so (Task 8: record it in the evidence of row 107).
// Needs deadline > answer_allowance + allowance.
LiteProbeDefinition l06_errors_code_space_probe(std::chrono::milliseconds deadline,
                                                std::chrono::milliseconds allowance = kLiteCloseAllowance,
                                                std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// l06-setup-client-path (rows 120, 124, 125): the default runner Setup stream and the `allowance`, observing the
// publisher's SETUP. `url_path` (path-abempty) and `url_query` (without '?') are what the adapter gave the publisher
// in its session URL; recorded in the definition (session_url_path / session_url_query; session_url_has_path set
// when the path is non-empty). Needs deadline > allowance. Task 9 must set session_url_has_path, session_url_path,
// session_url_query and binding CONSISTENTLY (the flag is redundant with a non-empty path and is not derived: a
// definition with the flag unset is NotRun whatever the strings say). Row 120 is an exact byte match of
// path + "?" + query: Task 9 passes unreserved characters only.
LiteProbeDefinition l06_setup_client_path_probe(std::chrono::milliseconds deadline, std::string_view url_path,
                                                std::string_view url_query,
                                                std::chrono::milliseconds allowance = kLiteSetupAllowance);

// L06-7-2-MUST-108 (scenario l06-errors-unknown-stream-type).
std::optional<bool> evaluate_l06_errors_unknown_stream_type_reset(const LiteTranscript& transcript);
// L06-7-2-MUST-NOT-109 (scenario l06-errors-unknown-stream-type).
std::optional<bool> evaluate_l06_errors_unknown_stream_type_not_fatal(const LiteTranscript& transcript);
// L06-4-4-MUST-030 (scenario l06-errors-unknown-reset-code).
std::optional<bool> evaluate_l06_errors_unknown_code_tolerated(const LiteTranscript& transcript);
// L06-4-4-MUST-NOT-032 (scenario l06-errors-unknown-reset-code).
std::optional<bool> evaluate_l06_errors_no_assumed_unauthorized(const LiteTranscript& transcript);
// L06-4-4-MUST-NOT-033 (scenario l06-errors-reserved-reset-code).
std::optional<bool> evaluate_l06_errors_reserved_code_tolerated(const LiteTranscript& transcript);
// L06-7-1-SHOULD-107 (scenario l06-errors-code-space). Row 027 on the same scenario is
// evaluate_l06_errors_code_space (lite06_common.h).
std::optional<bool> evaluate_l06_errors_message_length_close(const LiteTranscript& transcript);
// L06-7-3-2-SHOULD-124 (scenario l06-setup-client-path).
std::optional<bool> evaluate_l06_setup_path_sent(const LiteTranscript& transcript);
// L06-7-3-2-MUST-120 (scenario l06-setup-client-path).
std::optional<bool> evaluate_l06_setup_path_query_appended(const LiteTranscript& transcript);
// L06-7-3-2-MUST-NOT-125 (scenario l06-setup-client-path).
std::optional<bool> evaluate_l06_setup_path_absent_on_uri_binding(const LiteTranscript& transcript);

// L2c. Rows 120 and 124 are judged on native QUIC only and row 125 on WebTransport only (draft 7.3.2): on the other
// binding the rule is out of reach, not unjudged. False unless the scenario ran on a known binding in a judgeable
// transcript.
bool l06_setup_path_native_rows_inapplicable(const LiteTranscript& transcript);   // 120, 124: ran on WebTransport
bool l06_setup_path_absent_inapplicable(const LiteTranscript& transcript);         // 125: ran on native QUIC

}  // namespace moq::interop::scenarios
