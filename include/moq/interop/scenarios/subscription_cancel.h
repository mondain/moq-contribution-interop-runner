#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

struct SubscriptionCancelProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    RawProbeDefinition definition;
};

std::vector<SubscriptionCancelProbe> draft18_subscription_cancel_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::vector<SubscriptionCancelProbe> draft21_subscription_cancel_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool> evaluate_subscription_cancel_probe(
    const RawProbeTranscript& transcript, const SubscriptionCancelProbe& profile);

}  // namespace moq::interop::scenarios
