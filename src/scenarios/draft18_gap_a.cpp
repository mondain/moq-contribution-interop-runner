#include "moq/interop/scenarios/draft18_gap_a.h"

#include "draft18_gap_a_common.h"

#include <algorithm>
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
using namespace gap_a;

const std::vector<Entry>& entries() {
    static const std::vector<Entry> all = [] {
        std::vector<Entry> result;
        for (auto builder : {setup_entries, discovery_entries, object_entries}) {
            auto part = builder();
            result.insert(result.end(), std::make_move_iterator(part.begin()),
                          std::make_move_iterator(part.end()));
        }
        return result;
    }();
    return all;
}

RawProbeDefinition build(const Entry& entry, const Fixture& fixture,
                         std::chrono::milliseconds deadline) {
    auto definition = entry.build(fixture, deadline);
    const auto observe = entry.observe;
    definition.response_ready = [observe, fixture](const RawProbeTranscript& transcript) {
        return observe(transcript, fixture).ready;
    };
    return definition;
}
}  // namespace

std::vector<Draft18GapAProbe> draft18_gap_a_probes(
    std::chrono::milliseconds deadline, std::vector<std::vector<std::byte>> track_namespace,
    std::vector<std::byte> track_name) {
    if (deadline.count() <= 0) throw std::invalid_argument("invalid gap-A probe deadline");
    const auto fixture = canonical_fixture(std::move(track_namespace), std::move(track_name));
    if (!fixture_valid(fixture)) throw std::invalid_argument("invalid gap-A probe track fixture");
    std::vector<Draft18GapAProbe> result;
    for (const auto& entry : entries())
        result.push_back({entry.requirement_id, entry.evaluator_id, entry.native_only,
                          build(entry, fixture, deadline)});
    return result;
}

bool draft18_gap_a_scenario(std::string_view scenario_id) {
    return std::any_of(entries().begin(), entries().end(),
        [scenario_id](const auto& entry) { return entry.scenario_id == scenario_id; });
}

bool draft18_gap_a_requires_track(std::string_view scenario_id) {
    return std::any_of(entries().begin(), entries().end(), [scenario_id](const auto& entry) {
        return entry.scenario_id == scenario_id && entry.requires_track;
    });
}

bool draft18_gap_a_native_only(std::string_view scenario_id) {
    return std::any_of(entries().begin(), entries().end(), [scenario_id](const auto& entry) {
        return entry.scenario_id == scenario_id && entry.native_only;
    });
}

std::optional<bool> evaluate_draft18_gap_a_probe(const RawProbeTranscript& transcript,
                                                 const Draft18GapAProbe& profile, bool webtransport) {
    if (profile.definition.deadline.count() <= 0) return std::nullopt;
    const auto found = std::find_if(entries().begin(), entries().end(), [&](const auto& entry) {
        return entry.scenario_id == profile.definition.id &&
            entry.requirement_id == profile.requirement_id &&
            entry.evaluator_id == profile.evaluator_id;
    });
    if (found == entries().end() || transcript.scenario_id != found->scenario_id) return std::nullopt;
    if (found->native_only && webtransport) return std::nullopt;
    Fixture fixture = canonical_fixture({}, {std::byte{'x'}});
    if (found->recover) {
        if (transcript.writes.empty()) return std::nullopt;
        auto recovered = recover_fixture(*found->recover, transcript.writes.front().write.bytes);
        if (!recovered) return std::nullopt;
        fixture = std::move(*recovered);
    }
    const auto expected = build(*found, fixture, profile.definition.deadline);
    if (!raw_probe_stimulus_valid(transcript, expected)) return std::nullopt;
    return found->observe(transcript, fixture).result;
}

}  // namespace moq::interop::scenarios
