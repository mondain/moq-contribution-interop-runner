#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <cstdint>

namespace moq::interop::scenarios {

struct Draft21PeerCloseProbe {
    std::string requirement_id;
    std::string evaluator_id;
    std::uint64_t expected_close;
    RawProbeDefinition definition;
};

std::vector<Draft21PeerCloseProbe> draft21_peer_close_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000});

}  // namespace moq::interop::scenarios
