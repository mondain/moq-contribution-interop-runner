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
    auto residual = residual_specs();
    result.insert(result.end(), std::make_move_iterator(residual.begin()),
                  std::make_move_iterator(residual.end()));
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
    std::vector<std::byte> track_name) {
    const Fixture fixture{std::move(track_namespace), std::move(track_name)};
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

namespace {
bool terminal_event(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
           std::holds_alternative<transport::LocalCloseEvent>(event) ||
           std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
           std::holds_alternative<transport::TransportErrorEvent>(event) ||
           std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
const Spec* spec_of(const std::vector<Spec>& specs, const std::string& scenario) {
    for (const auto& spec : specs)
        if (spec.scenario == scenario) return &spec;
    return nullptr;
}
}  // namespace

std::optional<bool> evaluate_draft21_contribution_probe(
    const RawProbeTranscript& transcript, const Draft21ContributionProbe& probe) {
    if (probe.draft != 21 || probe.definition.deadline.count() <= 0) return std::nullopt;
    const auto fixture = transcript_fixture(transcript);
    if (!fixture) return std::nullopt;
    std::vector<Draft21ContributionProbe> candidates;
    try {
        candidates = draft21_contribution_probes(probe.definition.deadline,
                                                 fixture->track_namespace, fixture->track_name);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    const auto expected = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return candidate.requirement_id == probe.requirement_id &&
               candidate.evaluator_id == probe.evaluator_id &&
               candidate.definition.id == probe.definition.id;
    });
    if (expected == candidates.end() || transcript.scenario_id != expected->definition.id) return std::nullopt;
    const auto specs = all_specs();
    const auto* spec = spec_of(specs, probe.definition.id);
    if (!spec) return std::nullopt;
    // A window scenario judges the evidence collected until its window ended:
    // the deadline passed or the peer closed. Everything else must complete.
    RawProbeTranscript prefix = transcript;
    bool window_ended = false;
    if (spec->window && !transcript.harness_failed && transcript.stimulus_delivered) {
        const auto end = std::find_if(transcript.events.begin(), transcript.events.end(), terminal_event);
        const bool closed = end != transcript.events.end();
        window_ended = transcript.timed_out || closed;
        if (window_ended) {
            prefix.events.assign(transcript.events.begin(), end);
            prefix.complete = true;
            prefix.timed_out = false;
        }
    }
    if (!raw_probe_stimulus_valid(prefix, expected->definition)) return std::nullopt;
    View view(prefix);
    if (!view.valid()) return std::nullopt;
    view.set_window_ended(window_ended);
    for (const auto& row : spec->rows) {
        if (probe.requirement_id == row.requirement && probe.evaluator_id == row.evaluator)
            return judge_for(*spec, row)(view).result;
    }
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
