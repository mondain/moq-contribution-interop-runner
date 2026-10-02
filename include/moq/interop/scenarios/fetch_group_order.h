#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

enum class FetchGroupOrderRequest { BothExplicit, Ascending, Descending, Default };

struct FetchGroupOrderProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    FetchGroupOrderRequest order;
    RawProbeDefinition definition;
};

std::vector<FetchGroupOrderProbe> draft18_fetch_group_order_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::vector<FetchGroupOrderProbe> draft21_fetch_group_order_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool> evaluate_fetch_group_order_probe(
    const RawProbeTranscript& transcript, const FetchGroupOrderProbe& profile);

} // namespace moq::interop::scenarios
