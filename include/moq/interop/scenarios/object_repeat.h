#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

enum class ObjectRepeatAspect { Payload, ImmutableCount };

struct ObjectRepeatProbe {
    std::string requirement_id;
    std::string evaluator_id;
    unsigned draft;
    ObjectRepeatAspect aspect;
    RawProbeDefinition definition;
};

std::vector<ObjectRepeatProbe> draft18_object_repeat_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::vector<ObjectRepeatProbe> draft21_object_repeat_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool> evaluate_object_repeat_probe(
    const RawProbeTranscript& transcript, const ObjectRepeatProbe& profile);

}  // namespace moq::interop::scenarios
