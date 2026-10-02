#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <cstdint>
#include <optional>
#include <string>

namespace moq::interop::scenarios {

struct RequestProbeProfile {
    unsigned draft;
    std::string requirement_id;
    std::string evaluator_id;
    std::uint64_t expected_error;
    bool namespace_scoped;
    RawProbeDefinition definition;
    // The drafts name this error but omit its REQUEST_ERROR code assignment.
    bool compatibility_error{false};
};

// A response must follow complete acceptance of the corresponding request.
bool request_probe_response_ready(const RawProbeTranscript& transcript);
std::optional<bool> evaluate_raw_probe_request_error(
    const RawProbeTranscript& transcript, const RequestProbeProfile& profile);

}  // namespace moq::interop::scenarios
