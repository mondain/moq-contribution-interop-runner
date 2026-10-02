#pragma once
#include "moq/interop/scenarios/raw_probe.h"
namespace moq::interop::scenarios {
enum class FetchFirstObjectField { Group, Object };
struct FetchFirstObjectProbe {
  std::string requirement_id;
  std::string evaluator_id;
  unsigned draft;
  FetchFirstObjectField field;
  RawProbeDefinition definition;
};
bool fetch_first_object_fixture_valid(
    const std::vector<std::vector<std::byte>> &track_namespace,
    const std::vector<std::byte> &track_name);
std::vector<FetchFirstObjectProbe> draft18_fetch_first_object_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::vector<FetchFirstObjectProbe> draft21_fetch_first_object_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});
std::optional<bool>
evaluate_fetch_first_object_probe(const RawProbeTranscript &transcript,
                                  const FetchFirstObjectProbe &profile);
} // namespace moq::interop::scenarios
