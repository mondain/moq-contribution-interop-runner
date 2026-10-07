#pragma once

#include "moq/interop/scenarios/request_probe.h"

#include <vector>

namespace moq::interop::scenarios {

// On wire draft 22 (current_wire_draft()) the profiles whose SUBSCRIBE names a track only because the
// message needs one (namespace (), track "x" on wire 21) name `request_namespace`/`request_name` instead
// when `request_name` is nonempty; both are ignored on wire 21. The reserved-namespace requests keep their
// names. Names too large for a probe frame throw std::invalid_argument.
std::vector<RequestProbeProfile> draft21_request_profiles(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> request_namespace = {}, std::vector<std::byte> request_name = {});

// evaluate_raw_probe_request_error for a draft 21 profile. On wire draft 22 the SUBSCRIBE named the run's
// track: the definition is rebuilt from the names the delivered request carries (only an exact rebuild is
// accepted) and the stimulus is proved against it. On wire 21 it is evaluate_raw_probe_request_error.
std::optional<bool> evaluate_draft21_request_profile(
    const RawProbeTranscript& transcript, const RequestProbeProfile& profile);

}  // namespace moq::interop::scenarios
