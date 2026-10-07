#pragma once

// Helpers shared by the moq-lite-06 scenario modules (lite06_*.cpp) and the shared evaluator
// `l06-errors-code-space` (row L06-4-4-MUST-027), which several scenarios bind.
//
// Every helper reads the PEER's side of a transcript only: decoded messages through session::peer_messages(),
// issues through session::peer_issues()/peer_protocol_issues(), streams the peer opened through
// lite06::peer_streams() below, the peer's stream terminations (reset_code/stop_sending_code, which are the
// peer's direction by definition) and the peer's StreamDataEvents (transport events carry only received bytes).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/moqlite06/setup.h"

namespace moq::interop::scenarios {

// The scenario of the shared evaluator (Task 7 owns the scenario; the evaluator is shared).
inline constexpr std::string_view kL06ErrorsCodeSpace = "l06-errors-code-space";

// Row L06-4-4-MUST-027. A session close by the peer is judged for its code space only:
//   Pass:    an application close whose code is registered in the session table (draft 4.4.1, Table 2), values
//            also in the stream table included (they cannot show the space);
//   Fail:    an application close whose code is registered ONLY in the stream table (draft 4.4.2, Table 3), or a
//            RESET_STREAM / STOP_SENDING from the peer whose code is registered ONLY in the session table;
//   NotRun:  a scenario the row does not bind (only l06-errors-code-space, l06-setup-duplicate-parameter,
//            l06-setup-duplicate-stream, l06-setup-server-path and l06-setup-server-role are judged, on either
//            binding), no judgeable transcript, the runner's Setup stream not delivered, no peer close (or a QUIC
//            transport-space close, which carries no moq-lite code), an unregistered close code. On the
//            l06-errors-code-space scenario itself both halves (a stream code and a session close code) must
//            be observed for a Pass; on the probes the session half alone suffices.
std::optional<bool> evaluate_l06_errors_code_space(const LiteTranscript& transcript);

namespace lite06 {

// Draft 4.4.1 Table 2 and 4.4.2 Table 3 (pinned against the draft text in tests/golden/moqlite06_wire_audit_test.cpp).
inline constexpr std::uint64_t kSessionErrorCodes[] = {0x0, 0x1, 0x2, 0x3, 0x6, 0x10, 0x11, 0x15};
inline constexpr std::uint64_t kStreamErrorCodes[] = {0x0,  0x1,  0x2,  0x3,  0x4,  0x5,  0x12, 0x30, 0x31,
                                                      0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39};
inline constexpr std::uint64_t kProtocolViolation = 0x3;  // session code PROTOCOL_VIOLATION

bool is_session_code(std::uint64_t code);
bool is_stream_code(std::uint64_t code);

// The label the probe engine gives the runner's Setup stream record.
inline constexpr std::string_view kRunnerSetupLabel = "runner-setup";
// The label of a probe's allowance step (a Wait that executes only once the allowance elapsed).
inline constexpr std::string_view kAllowanceLabel = "allowance";

// The probe skeleton every lite06 scenario shares: id, deadline and a zero observation window (the probe ends as
// soon as its trailing allowance step executed). Throws std::invalid_argument when `allowance` is not positive or
// `deadline` does not exceed it (the allowance must always end the probe, never the deadline).
LiteProbeDefinition allowance_probe(std::string_view id, std::chrono::milliseconds deadline,
                                    std::chrono::milliseconds allowance);
// The trailing allowance step: an UNGATED Wait of `allowance`, labelled kAllowanceLabel.
LiteStep allowance_step(std::chrono::milliseconds allowance);
// The first step labelled `label`; nullptr when absent.
const LiteStepRecord* step_labelled(const LiteTranscript& transcript, std::string_view label);
// True when the allowance step executed (the whole allowance elapsed with the session open).
bool allowance_elapsed(const LiteTranscript& transcript);

// The step labelled `label` ("runner-setup" names the runner's Setup stream) when it executed and the transport
// accepted all its bytes (and its FIN when it had one): the stimulus reached the peer. nullptr otherwise.
const LiteStepRecord* proved_stimulus(const LiteTranscript& transcript, std::string_view label);
// As above, and its bytes are exactly `expected` (the stimulus rebuilt by the scenario).
const LiteStepRecord* proved_stimulus(const LiteTranscript& transcript, std::string_view label,
                                      std::span<const std::byte> expected);

// Streams the peer opened, in first-seen order (session::peer_streams for a transcript).
std::vector<const session::LiteStreamRecord*> peer_streams(const LiteTranscript& transcript);
// The record of a stream, by id (any origin); nullptr when unseen.
const session::LiteStreamRecord* find_stream(const LiteTranscript& transcript, std::uint64_t stream_id);

// The publisher's Setup stream(s) and its SETUP, read leniently: the framing (STREAM_TYPE, Message Length,
// Parameter Count, each Parameter ID / Length / Value, nothing left in the body) must be intact, but a repeated
// Parameter ID is kept rather than refused (decode_setup refuses it; rows 014 and 111 must tell the two apart).
struct PeerSetup {
    std::size_t setup_streams{0};                         // peer unidirectional streams with STREAM_TYPE 0x1
    const session::LiteStreamRecord* stream{nullptr};     // the first of them
    std::vector<std::byte> bytes;                         // its bytes after STREAM_TYPE, as received
    bool fin{false};
    bool reset{false};
    std::optional<wire::moqlite06::SetupMessage> message;  // a complete SETUP with intact framing
    bool malformed{false};  // the framing is broken (not merely a repeated id)
    bool trailing{false};   // bytes after the SETUP
};
// nullopt when the peer opened no Setup stream.
std::optional<PeerSetup> peer_setup_message(const LiteTranscript& transcript);

// The peer's session close code: an application-space close only (a QUIC transport-space close carries no
// moq-lite code). nullopt without a close.
std::optional<std::uint64_t> session_close_code(const LiteTranscript& transcript);
// The peer's RESET_STREAM code on `stream_id`; nullopt when not reset or reset without a code.
std::optional<std::uint64_t> stream_reset_code(const LiteTranscript& transcript, std::uint64_t stream_id);

// A Setup stream's bytes: STREAM_TYPE 0x1 then a SETUP built raw from `parameters` with write_framed_message
// and the varint writers, so repeated ids are possible (encode_setup refuses them by design).
std::vector<std::byte> raw_setup_stream(const std::vector<wire::moqlite06::SetupParameter>& parameters);

// The verdict shared by the four named close probes (rows 092, 112, 126, 131): the stimulus proven delivered,
// then Pass on an application close with PROTOCOL_VIOLATION (0x3), Fail on any other close (other code or
// transport space) or on a session still open when the probe's allowance step (kAllowanceLabel) executed
// (time-bounded; a stream-level reaction alone included), NotRun when the transcript is for another scenario, is
// unjudgeable (judgeable(): the close is the observation), the stimulus was not delivered, or the probe ended
// without a close before the allowance step ran.
std::optional<bool> judge_close_probe(const LiteTranscript& transcript, std::string_view scenario_id,
                                      const LiteStepRecord* stimulus);

}  // namespace lite06
}  // namespace moq::interop::scenarios
