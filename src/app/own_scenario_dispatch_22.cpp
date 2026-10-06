#include "moq/interop/app/own_scenario_dispatch_22.h"

#include "moq/interop/scenarios/draft22_location_filter_probes.h"
#include "moq/interop/scenarios/draft22_location_range.h"
#include "moq/interop/scenarios/draft22_namespace_discovery.h"
#include "moq/interop/scenarios/draft22_pre_setup_request.h"
#include "moq/interop/scenarios/draft22_publisher_location_filter.h"

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
        {scenarios::kDraft22PublisherLocationFilter, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_publisher_location_filter_probe(
                 execution.timeout, std::move(track.name_space), std::move(track.name));
         }},
        {scenarios::kDraft22RequestBeforeSetup, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_pre_setup_request_probe(execution.timeout, std::move(track.name_space),
                                                               std::move(track.name));
         }},
        {scenarios::kDraft22LocationFilterOverflow, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_location_filter_overflow_probe(
                 execution.timeout, std::move(track.name_space), std::move(track.name));
         }},
        {scenarios::kDraft22FillLocationFilterOverflow, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_fill_location_filter_overflow_probe(
                 execution.timeout, std::move(track.name_space), std::move(track.name));
         }},
        // Unscored probes (kUnscoredProbeTraits22).
        {scenarios::kDraft22LocationFilterUnknownType, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_location_filter_unknown_type_probe(
                 execution.timeout, std::move(track.name_space), std::move(track.name));
         }},
        {scenarios::kDraft22LocationFilterAbsoluteOrigin, [](const RunConfig& execution) {
             auto track = track_of(execution);
             return scenarios::draft22_location_filter_absolute_origin_probe(
                 execution.timeout, std::move(track.name_space), std::move(track.name));
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
        {scenarios::kDraft22PublisherLocationFilterEvaluator, scenarios::evaluate_draft22_publisher_location_filter},
        {scenarios::kDraft22PreSetupResetEvaluator, scenarios::evaluate_draft22_pre_setup_request},
        {scenarios::kDraft22LocationFilterOverflowEvaluator, scenarios::evaluate_draft22_location_filter_overflow},
    };
    return table;
}

// The evaluator of each unscored probe (kUnscoredProbeTraits22), keyed by the probe's id.
// With it, the reason recorded for each verdict (src/scenarios/draft22_location_filter_probes.cpp states the rules).
struct UnscoredEvaluator22 {
    std::string_view probe;
    OwnEvaluator22 evaluator;
    std::string_view pass_reason;
    std::string_view fail_reason;
    std::string_view no_verdict_reason;
};
const std::vector<UnscoredEvaluator22>& unscored_evaluators() {
    static const std::vector<UnscoredEvaluator22> table{
        {scenarios::kDraft22LocationFilterUnknownType,
         {scenarios::kDraft22LocationFilterUnknownTypeEvaluator, scenarios::evaluate_draft22_location_filter_unknown_type},
         "the publisher closed the session with PROTOCOL_VIOLATION after the undefined LOCATION_FILTER Type",
         "the publisher closed the session with an error code other than PROTOCOL_VIOLATION after the undefined "
         "LOCATION_FILTER Type",
         "no session close in the reaction window (or the stimulus was not proven sent), so nothing shows how the "
         "publisher treated the undefined Type"},
        {scenarios::kDraft22LocationFilterAbsoluteOrigin,
         {scenarios::kDraft22LocationFilterAbsoluteOriginEvaluator,
          scenarios::evaluate_draft22_location_filter_absolute_origin},
         "the publisher answered SUBSCRIBE_OK and delivered an Object under the absolute {0, 0} LOCATION_FILTER",
         "the publisher closed the session with PROTOCOL_VIOLATION, reading the valid {0, 0} filter as malformed",
         "no SUBSCRIBE_OK with a delivered Object and no PROTOCOL_VIOLATION close in the window (an error reply, "
         "silence, another close or an unproven stimulus)"},
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

std::optional<UnscoredVerdict22> evaluate_unscored_probe_22(const scenarios::RawProbeTranscript& transcript) {
    for (const auto& entry : unscored_evaluators()) {
        if (entry.probe != transcript.scenario_id) continue;
        // Evidence cut at a recording limit is never judged (as for rows).
        if (transcript.event_limit_reached)
            return UnscoredVerdict22{entry.evaluator.id, {}, "evidence cut at a recording limit; the probe is not judged"};
        if (!entry.evaluator.evaluate) return UnscoredVerdict22{entry.evaluator.id, {}, "the probe has no evaluator"};
        const auto verdict = entry.evaluator.evaluate(transcript);
        return UnscoredVerdict22{entry.evaluator.id, verdict,
                                 !verdict ? entry.no_verdict_reason : *verdict ? entry.pass_reason : entry.fail_reason};
    }
    return std::nullopt;
}

std::vector<std::pair<std::string_view, std::string_view>> unscored_probe_evaluators_22() {
    std::vector<std::pair<std::string_view, std::string_view>> ids;
    for (const auto& entry : unscored_evaluators()) ids.emplace_back(entry.probe, entry.evaluator.id);
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
