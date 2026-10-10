#pragma once

// The moq-lite-06 datagram scenario (L2b Task B2a). The publisher under test is the CLIENT; the runner is the server
// and the subscriber. The rule (catalog row L06-6-4-MUST-NOT-105): a publisher must not send a datagram whose body
// exceeds 1200 bytes (draft 6.4). Datagrams are a permission (row 100), so a publisher that sends none, the moq CLI
// included, is NotRun, never a Pass.
//
// The scenario needs the track fixture (plan decision (d)): the builder copies the broadcast path and track name into
// the definition (requires_track = true) and the transcript carries them, so the evaluator rebuilds the exact stimulus
// bytes. It observes its whole window through an UNGATED trailing Wait labelled "allowance" and never ends early.

#include <chrono>
#include <optional>
#include <string_view>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"

namespace moq::interop::scenarios {

inline constexpr std::string_view kL06DatagramSize = "l06-datagram-size";

// Step label of the default SUBSCRIBE (Subscribe ID 0, as l06-subscribe-latest sends it).
inline constexpr std::string_view kL06DatagramSubscribeLabel = "datagram-subscribe";

// l06-datagram-size: announce exchange (ANNOUNCE_REQUEST "" and a Wait gated on its answer, at most
// `answer_allowance`), the default SUBSCRIBE, then the observation `window`. Needs deadline > answer_allowance + window.
LiteProbeDefinition l06_datagram_size_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                            std::string_view track_name,
                                            std::chrono::milliseconds window = kLiteObservationWindow,
                                            std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// L06-6-4-MUST-NOT-105 (scenario l06-datagram-size). Fail: a datagram whose body exceeds 1200 bytes. Pass: at least one
// datagram within the limit and none above it. NotRun: no datagram that decoded or exceeded the limit, an
// unjudgeable transcript, the stimuli not delivered as built.
std::optional<bool> evaluate_l06_datagram_size_limit(const LiteTranscript& transcript);

// L2c. The publisher used no datagram, with the stimuli delivered as built: the rule is vacuous (datagrams are a
// permission, row 100). False for an unproved transcript, and for any run in which a datagram decoded or exceeded
// the limit.
bool l06_datagram_size_inapplicable(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
