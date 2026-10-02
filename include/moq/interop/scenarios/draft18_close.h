#pragma once
#include "moq/interop/scenarios/raw_probe.h"
#include <string_view>
namespace moq::interop::scenarios {
struct Draft18CloseProfile {
    std::string_view requirement_id;
    std::string_view scenario_id;
    std::string_view evaluator_id;
    std::optional<std::uint64_t> expected_close;
    bool webtransport_only{false};
    bool native_only{false};
};
std::span<const Draft18CloseProfile> draft18_close_profiles();
// The duplicate Request ID scenario sends a SUBSCRIBE that must itself be
// acceptable, so it names the configured track when one is supplied. Every
// other scenario ignores the track.
RawProbeDefinition draft18_close_probe(std::string_view scenario_id,
                                      std::chrono::milliseconds deadline,
                                      std::vector<std::vector<std::byte>> track_namespace = {},
                                      std::vector<std::byte> track_name = {});
// The probe a transcript was produced from: recovers the track the stimulus
// named, so evaluation compares against the bytes that were actually sent.
RawProbeDefinition draft18_close_probe_for(std::string_view scenario_id,
                                          const RawProbeTranscript& transcript,
                                          std::chrono::milliseconds deadline);
}  // namespace moq::interop::scenarios
