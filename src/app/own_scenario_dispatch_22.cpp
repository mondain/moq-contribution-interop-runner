#include "moq/interop/app/own_scenario_dispatch_22.h"

#include "moq/interop/scenarios/draft22_location_range.h"
#include "moq/interop/scenarios/draft22_namespace_discovery.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace moq::interop::app {
namespace {

// A production own probe. Its traits live only in kOwnScenarioTraits22 (the predicates' single source);
// own_probe_tables_agree_22() checks that both tables name the same ids.
struct OwnProbe22 {
    std::string_view id;
    std::function<scenarios::RawProbeDefinition(const RunConfig&)> probe;
};

std::vector<std::byte> bytes_of(std::string_view value) {
    std::vector<std::byte> result;
    for (const char byte : value) result.push_back(static_cast<std::byte>(byte));
    return result;
}

struct Track {
    std::vector<std::vector<std::byte>> name_space;
    std::vector<std::byte> name;
};

// The track fixture of a probe that needs one.
Track track_of(const RunConfig& execution) {
    if (!execution.track_fixture) throw std::invalid_argument("track probe requires a track fixture");
    Track track;
    for (const auto& field : execution.track_fixture->namespace_fields) track.name_space.push_back(bytes_of(field));
    track.name = bytes_of(execution.track_fixture->track_name);
    return track;
}

// Production probes for the ids in kOwnScenarioTraits22 (Tasks 9-10 add them together).
const std::vector<OwnProbe22>& production_probes() {
    static const std::vector<OwnProbe22> table{
        {scenarios::kDraft22SubscribeLocationRange, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_subscribe_location_range_probe(execution.timeout, std::move(track.name_space),
                                                                      std::move(track.name));
         }},
        {scenarios::kDraft22UpdateLocationRange, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_update_location_range_probe(execution.timeout, std::move(track.name_space),
                                                                   std::move(track.name));
         }},
        {scenarios::kDraft22FetchLocationRange, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_fetch_location_range_probe(execution.timeout, std::move(track.name_space),
                                                                  std::move(track.name));
         }},
        {scenarios::kDraft22DiscoverNamespaces, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_namespace_discovery_probe(execution.timeout, std::move(track.name_space),
                                                                 std::move(track.name));
         }},
    };
    return table;
}

// Production own draft 22 evaluators (Tasks 9-10).
const std::vector<OwnEvaluator22>& production_evaluators() {
    static const std::vector<OwnEvaluator22> table{
        {scenarios::kDraft22SubscriptionRangeEvaluator, scenarios::evaluate_draft22_subscription_location_range},
        {scenarios::kDraft22FetchRangeEvaluator, scenarios::evaluate_draft22_fetch_location_range},
        {scenarios::kDraft22NamespaceDiscoveryEvaluator, scenarios::evaluate_draft22_namespace_discovery},
    };
    return table;
}

std::optional<OwnEvaluator22> own_evaluator(std::string_view id) {
    if (auto registered = OwnScenarioRegistry22::instance().registered_evaluator(id)) return registered;
    for (const auto& entry : production_evaluators())
        if (entry.id == id) return entry;
    return std::nullopt;
}

}  // namespace

std::optional<scenarios::RawProbeDefinition> own_probe_22(std::string_view id, const RunConfig& execution) {
    if (const auto scenario = OwnScenarioRegistry22::instance().registered_scenario(id)) {
        if (!scenario->probe) return std::nullopt;
        return scenario->probe(execution);
    }
    for (const auto& entry : production_probes())
        if (entry.id == id && entry.probe) return entry.probe(execution);
    return std::nullopt;
}

bool has_own_probe_22(std::string_view id) {
    if (const auto scenario = OwnScenarioRegistry22::instance().registered_scenario(id))
        return static_cast<bool>(scenario->probe);
    return std::any_of(production_probes().begin(), production_probes().end(),
                       [&](const auto& entry) { return entry.id == id && entry.probe; });
}

std::vector<std::string_view> production_own_evaluator_ids_22() {
    std::vector<std::string_view> ids;
    for (const auto& entry : production_evaluators()) ids.push_back(entry.id);
    return ids;
}

std::vector<requirements::Outcome> evaluate_own_draft22(const requirements::RequirementCatalog& draft22,
                                                        std::span<const scenarios::RawProbeTranscript> transcripts) {
    using requirements::OutcomeState;
    std::vector<requirements::Outcome> outcomes;
    for (const auto& row : draft22.requirements) {
        if (row.applicability != requirements::Applicability::Applicable ||
            row.testability != requirements::Testability::Testable)
            continue;
        std::vector<OwnEvaluator22> evaluators;
        for (const auto& id : row.evaluators)
            if (auto evaluator = own_evaluator(id)) evaluators.push_back(std::move(*evaluator));
        if (evaluators.empty()) continue;
        auto state = OutcomeState::NotRun;
        std::map<std::string, std::size_t> contexts;
        std::map<std::string, std::size_t> successful;
        std::set<std::string, std::less<>> exercised;
        for (const auto& transcript : transcripts) {
            if (std::find(row.scenarios.begin(), row.scenarios.end(), transcript.scenario_id) == row.scenarios.end())
                continue;
            ++contexts[transcript.scenario_id];
            // Evidence cut at a recording limit is never scored (as for draft 21 rows).
            if (transcript.event_limit_reached) continue;
            bool verdict = false;
            for (const auto& evaluator : evaluators) {
                if (!evaluator.evaluate) continue;
                const auto result = evaluator.evaluate(transcript);
                if (!result) continue;
                if (!*result) {
                    state = OutcomeState::Fail;
                    break;
                }
                verdict = true;
                exercised.emplace(evaluator.id);
            }
            if (state == OutcomeState::Fail) break;
            if (verdict) ++successful[transcript.scenario_id];
        }
        const bool scenarios_settled = !row.scenarios.empty() &&
            std::all_of(row.scenarios.begin(), row.scenarios.end(), [&](const auto& scenario) {
                return contexts[scenario] == 1 && successful[scenario] == 1;
            });
        if (state != OutcomeState::Fail && scenarios_settled &&
            std::all_of(row.evaluators.begin(), row.evaluators.end(),
                        [&](const auto& evaluator) { return exercised.contains(evaluator); }))
            state = OutcomeState::Pass;
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

}  // namespace moq::interop::app
