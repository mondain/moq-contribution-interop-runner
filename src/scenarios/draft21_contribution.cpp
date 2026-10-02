#include "moq/interop/scenarios/draft21_contribution.h"

#include "draft21_contribution_support.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {
using namespace d21c;

std::vector<Spec> all_specs() {
    auto result = session_specs();
    auto objects = object_specs();
    result.insert(result.end(), std::make_move_iterator(objects.begin()),
                  std::make_move_iterator(objects.end()));
    auto slice_b = d21b_specs();
    result.insert(result.end(), std::make_move_iterator(slice_b.begin()),
                  std::make_move_iterator(slice_b.end()));
    d21b_attach_rows(result);
    return result;
}

const Judge& judge_for(const Spec& spec, const RowBinding& row) {
    return row.judge ? row.judge : spec.judge;
}

std::optional<Fixture> transcript_fixture(const RawProbeTranscript& transcript) {
    for (const auto& write : transcript.writes) {
        auto fixture = recover_fixture(write.write.bytes);
        if (fixture) return fixture;
    }
    return std::nullopt;
}
}  // namespace

std::vector<Draft21ContributionProbe> draft21_contribution_probes(
    std::chrono::milliseconds deadline, std::vector<std::vector<std::byte>> track_namespace,
    std::vector<std::byte> track_name, std::string denied_token) {
    const Fixture fixture{std::move(track_namespace), std::move(track_name), std::move(denied_token)};
    if (deadline.count() <= 0 || !fixture_valid(fixture))
        throw std::invalid_argument("invalid draft-21 contribution probe configuration");
    std::vector<Draft21ContributionProbe> result;
    for (const auto& spec : all_specs()) {
        for (const auto& row : spec.rows) {
            auto definition = spec.build(fixture);
            definition.id = spec.scenario;
            definition.deadline = deadline;
            const Judge judge = judge_for(spec, row);
            definition.response_ready = [judge](const RawProbeTranscript& transcript) {
                const View view(transcript);
                return view.valid() && judge(view).ready;
            };
            result.push_back({row.requirement, row.evaluator, 21, std::move(definition)});
        }
    }
    return result;
}

bool draft21_contribution_scenarios_are_alternatives(const std::string& requirement_id) {
    return requirement_id == "D21-9-9-MUST-365";
}

std::optional<bool> evaluate_draft21_contribution_probe(
    const RawProbeTranscript& transcript, const Draft21ContributionProbe& probe) {
    if (probe.draft != 21 || probe.definition.deadline.count() <= 0) return std::nullopt;
    auto fixture = transcript_fixture(transcript);
    if (!fixture) return std::nullopt;
    fixture->denied_token = transcript.denied_authorization_token.value_or(std::string{});
    std::vector<Draft21ContributionProbe> candidates;
    try {
        candidates = draft21_contribution_probes(probe.definition.deadline, fixture->track_namespace,
                                                 fixture->track_name, fixture->denied_token);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    const auto expected = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return candidate.requirement_id == probe.requirement_id &&
               candidate.evaluator_id == probe.evaluator_id &&
               candidate.definition.id == probe.definition.id;
    });
    if (expected == candidates.end() || transcript.scenario_id != expected->definition.id) return std::nullopt;
    auto definition = expected->definition;
    if (definition.offer_replacement_session) {
        // The GOAWAY names the runner's second listener, which only the run knew.
        if (!transcript.replacement_uri || !definition.bind_alternate_uri) return std::nullopt;
        definition.bind_alternate_uri(definition, *transcript.replacement_uri);
    }
    if (!raw_probe_stimulus_valid(transcript, definition)) return std::nullopt;
    const View view(transcript);
    if (!view.valid()) return std::nullopt;
    for (const auto& spec : all_specs()) {
        if (spec.scenario != probe.definition.id) continue;
        for (const auto& row : spec.rows) {
            if (probe.requirement_id == row.requirement && probe.evaluator_id == row.evaluator)
                return judge_for(spec, row)(view).result;
        }
    }
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
