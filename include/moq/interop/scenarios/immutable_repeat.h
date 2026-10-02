#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

enum class ImmutableRepeatAspect { Content, Presence, Serialization };

struct ImmutableRepeatProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    ImmutableRepeatAspect aspect;
    RawProbeDefinition definition;
};

std::vector<ImmutableRepeatProbe> draft18_immutable_repeat_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::vector<ImmutableRepeatProbe> draft21_immutable_repeat_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool> evaluate_immutable_repeat_probe(
    const RawProbeTranscript& transcript, const ImmutableRepeatProbe& profile);

} // namespace moq::interop::scenarios
