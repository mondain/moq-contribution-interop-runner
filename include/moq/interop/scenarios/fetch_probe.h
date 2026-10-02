#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

enum class FetchProbeExpectation {
    CancelRequestReset,
    CancelDataReset,
    FailedUpdateDataReset,
};

struct FetchProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    FetchProbeExpectation expectation;
    RawProbeDefinition definition;
};

std::vector<FetchProbe> draft18_fetch_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::vector<FetchProbe> draft21_fetch_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool> evaluate_fetch_probe(const RawProbeTranscript& transcript,
                                       const FetchProbe& profile);

}  // namespace moq::interop::scenarios
