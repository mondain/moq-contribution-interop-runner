#include "moq/interop/requirements/draft22_evaluators.h"

#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_lineage_data.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace moq::interop::requirements {
namespace {

using Reverse = std::multimap<std::string_view, std::string_view>;

// d21 -> d22 for a NamePair table ({d22, d21}, sorted by d22); a d21 id may have several d22 counterparts.
template <typename Table>
Reverse reverse(const Table& table) {
    Reverse map;
    for (const auto& pair : table) map.emplace(pair.d21, pair.d22);
    return map;
}

const Reverse& scenarios() {
    static const Reverse map = reverse(lineage_data::kSharedScenarios);
    return map;
}

const Reverse& evaluators() {
    static const Reverse map = reverse(lineage_data::kSharedEvaluators);
    return map;
}

std::vector<std::string_view> targets(const Reverse& map, std::string_view d21) {
    std::vector<std::string_view> out;
    const auto [first, last] = map.equal_range(d21);
    for (auto it = first; it != last; ++it) out.push_back(it->second);
    return out;
}

std::vector<std::string_view> rows(std::string_view d21) {
    std::vector<std::string_view> out;
    const auto& pairs = lineage_data::kSharedRows;  // sorted by (d21, d22)
    auto it = std::lower_bound(pairs.begin(), pairs.end(), d21,
                               [](const lineage_data::RowPair& pair, std::string_view value) { return pair.d21 < value; });
    for (; it != pairs.end() && it->d21 == d21; ++it) out.push_back(it->d22);
    return out;
}

bool own_row_ancestor(std::string_view d21) {
    return std::binary_search(kDraft21AncestorsOfOwnRows22.begin(), kDraft21AncestorsOfOwnRows22.end(), d21);
}

// Folds another draft 21 source into the draft 22 binding it collapses onto. The sources must agree on the
// evaluator (one binding per (row, scenario) cannot carry two); the evidence kinds become their sorted,
// deduplicated union, so the result does not depend on which source came first.
void merge(ExecutableBinding& target, const ExecutableBinding& source, std::string_view evaluator) {
    if (target.evaluator_id != evaluator)
        throw std::logic_error("draft 21 binding " + source.requirement_id + " " + source.scenario_id +
                               " collapses onto " + target.requirement_id + " " + target.scenario_id +
                               " with evaluator " + std::string(evaluator) + ", another source with " +
                               target.evaluator_id);
    std::set<std::string> kinds(target.evidence_kinds.begin(), target.evidence_kinds.end());
    kinds.insert(source.evidence_kinds.begin(), source.evidence_kinds.end());
    target.evidence_kinds.assign(kinds.begin(), kinds.end());
}

}  // namespace

Draft22Translation translate_draft21_bindings(std::span<const ExecutableBinding> draft21) {
    Draft22Translation result;
    // Keyed by (row, scenario), the collapse key; with one evaluator per key this is also the
    // (row, scenario, evaluator) order.
    std::map<std::pair<std::string, std::string>, ExecutableBinding> shared;
    for (const auto& binding : draft21) {
        const auto row_targets = rows(binding.requirement_id);
        const auto scenario_targets = targets(scenarios(), binding.scenario_id);
        const auto evaluator_targets = targets(evaluators(), binding.evaluator_id);
        const char* reason = nullptr;
        if (row_targets.empty()) reason = own_row_ancestor(binding.requirement_id) ? "own_row" : "dropped_row";
        else if (scenario_targets.empty()) reason = "own_scenario";
        else if (evaluator_targets.empty()) reason = "own_evaluator";
        if (reason) {
            result.untranslated.push_back({binding, reason});
            continue;
        }
        for (const auto row : row_targets)
            for (const auto scenario : scenario_targets)
                for (const auto evaluator : evaluator_targets) {
                    ExecutableBinding translated{22, std::string(row), std::string(scenario), std::string(evaluator),
                                                 binding.evidence_kinds};
                    // A second draft 21 source of the same draft 22 (row, scenario) (a one-to-many row) is merged
                    // into the first; it is never emitted twice.
                    auto [entry, inserted] =
                        shared.try_emplace({translated.requirement_id, translated.scenario_id}, std::move(translated));
                    if (!inserted) merge(entry->second, binding, evaluator);
                }
    }
    result.shared.reserve(shared.size());
    for (auto& [key, binding] : shared) result.shared.push_back(std::move(binding));
    return result;
}

std::vector<ExecutableBinding> draft22_shared_bindings() {
    return translate_draft21_bindings(draft21_executable_bindings()).shared;
}

std::vector<UntranslatedBinding> draft22_untranslated_bindings() {
    return translate_draft21_bindings(draft21_executable_bindings()).untranslated;
}

// ===================================================================================================================
// Own bindings: the rows of lineage_data::kOwnRows22 that have an own scenario and evaluator (implemented in D2).
// One binding per (row, scenario), carrying the evaluator the catalog row (requirements/draft22.json) pairs with
// it. Every evaluator here judges a raw probe transcript; the evidence kinds name what it reads. An own scenario or
// evaluator named by a catalog row, listed in the lineage or registered in app::kOwnScenarioTraits22 without a
// binding here fails Draft22OwnBindings.EveryOwnScenarioAndEvaluatorNamedByAnApplicableRowIsBound
// (tests/unit/draft22_bindings_test.cpp). Kept in (row, scenario, evaluator) order.
// ===================================================================================================================
std::vector<ExecutableBinding> draft22_own_bindings() {
    // Window probes: the evaluator rebuilds the stimulus from the transcript's first write and proves it was
    // delivered (raw_probe_stimulus), then judges the publisher's streams, messages and resets in the transport
    // events before the terminal event (raw_probe_transport_event).
    const std::vector<std::string> window{"raw_probe_stimulus", "raw_probe_transport_event"};
    // Close probes: the evaluator rebuilds the stimulus and judges the session close code
    // (evaluate_raw_probe_close), as the draft 21 close-probe bindings of the counterpart row did.
    const std::vector<std::string> close{"raw_probe_stimulus", "peer_close"};
    return {
        // D22-3-3-1-MUST-NOT-069, no Object outside the requested Location range:
        // src/scenarios/draft22_location_range.cpp. FETCH: the Objects on the fetch stream of each bounded
        // FETCH, judged against the LOCATION_FILTER it carried (evaluate_draft22_fetch_location_range).
        {22, "D22-3-3-1-MUST-NOT-069", "d22-fetch-bounded-location-range",
         "d22-fetch-objects-within-requested-location-range", window},
        // SUBSCRIBE, and SUBSCRIBE then REQUEST_UPDATE: the subgroup Objects of each subscription, judged against
        // the range in effect, from SUBSCRIBE_OK's Largest Object (evaluate_draft22_subscription_location_range).
        {22, "D22-3-3-1-MUST-NOT-069", "d22-subscribe-bounded-location-range",
         "d22-subscription-objects-within-effective-location-range", window},
        {22, "D22-3-3-1-MUST-NOT-069", "d22-update-subscription-location-range",
         "d22-subscription-objects-within-effective-location-range", window},
        // D22-4-2-MUST-110, NAMESPACE for each known matching namespace after SUBSCRIBE_NAMESPACE:
        // src/scenarios/draft22_namespace_discovery.cpp. The NAMESPACE messages on the response stream of each
        // prefix's SUBSCRIBE_NAMESPACE (evaluate_draft22_namespace_discovery).
        {22, "D22-4-2-MUST-110", "d22-discover-original-publisher-namespaces",
         "d22-original-publisher-matching-namespace-notification", window},
        // D22-6-3-MAY-159 (optional), an early request stream reset (or buffered) before SETUP completes:
        // src/scenarios/draft22_pre_setup_request.cpp. The early request stream's reset, STOP_SENDING or first
        // answer relative to the SETUP completion (evaluate_draft22_pre_setup_request).
        {22, "D22-6-3-MAY-159", "d22-request-stream-before-peer-setup", "d22-pre-setup-request-stream-reset",
         window},
        // D22-9-20-9-MAY-422 (optional), LOCATION_FILTER in publisher messages:
        // src/scenarios/draft22_publisher_location_filter.cpp. The LOCATION_FILTER values decoded from the
        // publisher's request-stream messages (evaluate_draft22_publisher_location_filter).
        {22, "D22-9-20-9-MAY-422", "d22-publisher-location-filter-parameter",
         "d22-publisher-location-filter-capability", window},
        // D22-9-20-9-MUST-424, PROTOCOL_VIOLATION when StartGroup + EndGroupDelta overflows:
        // src/scenarios/draft22_location_filter_probes.cpp. The session close code after the overflowing filter,
        // inside FILL_PARAMETERS or at top level (evaluate_draft22_location_filter_overflow).
        {22, "D22-9-20-9-MUST-424", "d22-fill-location-filter-end-group-overflow",
         "d22-location-filter-overflow-protocol-violation", close},
        {22, "D22-9-20-9-MUST-424", "d22-location-filter-end-group-overflow",
         "d22-location-filter-overflow-protocol-violation", close},
    };
}
// ============================================================================================ end of own bindings

std::vector<ExecutableBinding> draft22_executable_bindings() {
    auto bindings = draft22_shared_bindings();
    auto own = draft22_own_bindings();
    bindings.insert(bindings.end(), std::make_move_iterator(own.begin()), std::make_move_iterator(own.end()));
    std::sort(bindings.begin(), bindings.end(), [](const auto& left, const auto& right) {
        return std::tie(left.requirement_id, left.scenario_id, left.evaluator_id) <
               std::tie(right.requirement_id, right.scenario_id, right.evaluator_id);
    });
    return bindings;
}

}  // namespace moq::interop::requirements
