#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <string_view>

namespace moq::interop::scenarios {

// Which scenarios may carry a liveness follow-up. The argument for each family
// is in src/scenarios/raw_probe_liveness.cpp; anything not listed there is not
// sound and never gets one.
bool liveness_follow_up_sound(unsigned draft, std::string_view scenario_id);

// Marks `definition` for a follow-up when its scenario is listed and its
// writes keep the argument valid. Changes no stimulus byte.
void apply_liveness_policy(RawProbeDefinition& definition, unsigned draft);

// Supplies the follow-up request (a SUBSCRIBE for the configured track) to a
// definition that apply_liveness_policy marked. Does nothing for any other.
void bind_liveness_track(RawProbeDefinition& definition,
                         const std::vector<std::vector<std::byte>>& track_namespace,
                         const std::vector<std::byte>& track_name);

// Whether `request` is a complete, parameter-free SUBSCRIBE with the policy's
// Request ID for `policy.draft`.
bool liveness_request_valid(const RawProbeLiveness& policy);

// Whether the definition's writes are ones a follow-up can be argued about:
// reliable streams only, no publisher-opened streams, no transport steps, no
// replacement session.
bool liveness_definition_eligible(const RawProbeDefinition& definition);

enum class LivenessAnswer {
    None,     // nothing decisive yet
    Serving,  // a well-formed SUBSCRIBE_OK on the follow-up stream
    Other,    // anything else (an error, a reset, a FIN, a malformed reply)
};
LivenessAnswer liveness_answer(const RawProbeTranscript& transcript, unsigned draft);

}  // namespace moq::interop::scenarios
