#pragma once

// Draft 22 own scenario for row D22-6-3-MAY-159 (draft-ietf-moq-transport-22 Section 6.3): "Unidirectional
// streams containing Objects or bidirectional stream(s) beginning with a request message could arrive prior
// to the control streams, in which case the data SHOULD be buffered until both control streams arrive and
// setup is complete. If an implementation does not want to buffer or if the message type is not supported,
// it MAY reset such streams before the session and control streams are established." Draft 21 said "such
// bidirectional streams"; draft 22 also covers early unidirectional Object streams. The draft 21
// counterpart (d21-request-stream-before-peer-setup) has no implementation.
//
// The probe sends an early request stream only. A runner validating a publisher subscribes; an Object stream
// it sent would carry a Track Alias the publisher never assigned, so a reset of it could be the publisher's
// unknown-alias handling (Section 3.1.3.1) rather than this permission, and is not exercised.
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

inline constexpr std::string_view kDraft22RequestBeforeSetup = "d22-request-stream-before-peer-setup";
inline constexpr std::string_view kDraft22PreSetupResetEvaluator = "d22-pre-setup-request-stream-reset";
// How long the runner holds back the rest of its SETUP after the early request went out.
inline constexpr std::chrono::milliseconds kDraft22PreSetupHold{500};

// One session. The runner's control stream starts with only the first byte of its SETUP (Section 9.1: Type
// 0x2F00, two bytes, then Length), so the publisher cannot complete setup. At once, without waiting for the
// publisher's SETUP: a SUBSCRIBE (Request ID 1, FORWARD=0) for the track on a new request stream, and
// kDraft22PreSetupHold later the rest of the SETUP on the control stream.
RawProbeDefinition draft22_pre_setup_request_probe(std::chrono::milliseconds deadline,
                                                   std::vector<std::vector<std::byte>> track_namespace,
                                                   std::vector<std::byte> track_name);

// Verdict of d22-pre-setup-request-stream-reset on one transcript. Both permitted behaviours pass: true
// when the publisher reset the request stream (or sent STOP_SENDING for it) before the rest of the SETUP
// went out, i.e. before the session could be established (the MAY), or answered the request with
// SUBSCRIBE_OK or REQUEST_ERROR only after the rest of the SETUP went out (it buffered, the SHOULD). No
// behaviour of the publisher fails this permission, so the evaluator never returns false. No value
// otherwise: another scenario, an unproven stimulus, an answer that began before the SETUP was complete
// (the request was processed before setup, which is neither), a reset after it (a cancellation under
// Section 6.4.2.3, not this permission), a session close with no earlier reset (a reset before completion
// still passes when the session closes after it), or nothing by the end of the window. The hold is a limit:
// an eager answer whose flight exceeds it, or a request retransmitted after the SETUP tail, looks like
// buffering.
std::optional<bool> evaluate_draft22_pre_setup_request(const RawProbeTranscript& transcript);

}  // namespace moq::interop::scenarios
