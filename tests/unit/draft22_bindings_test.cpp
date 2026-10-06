#include "moq/interop/requirements/draft22_evaluators.h"

#include "moq/interop/app/own_scenarios_22.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_lineage_data.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/lineage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace moq::interop::requirements {
namespace {

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

using Key = std::tuple<std::string, std::string, std::string>;

Key key(const ExecutableBinding& binding) {
    return {binding.requirement_id, binding.scenario_id, binding.evaluator_id};
}

// Independent of the implementation: linear scans over the generated lineage tables.
std::vector<std::string> rows22(std::string_view d21) {
    std::vector<std::string> out;
    for (const auto& pair : lineage_data::kSharedRows)
        if (pair.d21 == d21) out.emplace_back(pair.d22);
    return out;
}
template <typename Table>
std::vector<std::string> names22(const Table& table, std::string_view d21) {
    std::vector<std::string> out;
    for (const auto& pair : table)
        if (pair.d21 == d21) out.emplace_back(pair.d22);
    return out;
}

// Every draft 22 binding a draft 21 binding translates to (empty when it does not translate).
std::vector<ExecutableBinding> translations(const ExecutableBinding& d21) {
    std::vector<ExecutableBinding> out;
    for (const auto& row : rows22(d21.requirement_id))
        for (const auto& scenario : names22(lineage_data::kSharedScenarios, d21.scenario_id))
            for (const auto& evaluator : names22(lineage_data::kSharedEvaluators, d21.evaluator_id))
                out.push_back({22, row, scenario, evaluator, d21.evidence_kinds});
    return out;
}

bool same(const ExecutableBinding& left, const ExecutableBinding& right) {
    return left.draft == right.draft && key(left) == key(right) && left.evidence_kinds == right.evidence_kinds;
}

// (a) No draft 22 binding is fabricated: each one is the translation of a draft 21 binding.
TEST(Draft22Bindings, EverySharedBindingIsTheTranslationOfADraft21Binding) {
    const auto draft21 = draft21_executable_bindings();
    const auto shared = draft22_shared_bindings();
    ASSERT_FALSE(shared.empty());
    for (const auto& binding : shared) {
        EXPECT_EQ(binding.draft, 22u);
        EXPECT_TRUE(binding.requirement_id.starts_with("D22-")) << binding.requirement_id;
        EXPECT_TRUE(binding.scenario_id.starts_with("d22-")) << binding.scenario_id;
        EXPECT_TRUE(binding.evaluator_id.starts_with("d22-")) << binding.evaluator_id;
        const bool sourced = std::any_of(draft21.begin(), draft21.end(), [&](const auto& source) {
            const auto candidates = translations(source);
            return std::any_of(candidates.begin(), candidates.end(),
                               [&](const auto& candidate) { return same(candidate, binding); });
        });
        EXPECT_TRUE(sourced) << binding.requirement_id << " " << binding.scenario_id << " " << binding.evaluator_id;
    }
}

// Measured: 250 draft 21 bindings = 241 translated + 9 untranslated (4 own_row: D21-3-3-1-MUST-NOT-057 x2 and
// D21-9-20-10-MUST-432 x2; 5 dropped_row: D21-4-1-MUST-083 x2, D21-4-2-MUST-089, D21-9-15-MUST-383 and
// D21-9-18-MUST-391). No draft 21 id maps to several
// draft 22 ids (fan-out 0) and no draft 22 binding has two draft 21 sources (collapsed 0: the only one-to-many row,
// D22-9-MUST-294, has a binding source in D21-9-MUST-282 only; D21-9-MUST-283 is NotApplicable in draft 21).
constexpr std::size_t kPinnedDraft21 = 250;
constexpr std::size_t kPinnedTranslated = 241;
constexpr std::size_t kPinnedExpansions = 241;
constexpr std::size_t kPinnedFanOut = 0;
constexpr std::size_t kPinnedShared = 241;
constexpr std::size_t kPinnedCollapsed = 0;
constexpr std::size_t kPinnedUntranslated = 9;
constexpr std::size_t kPinnedOwnRow = 4;
constexpr std::size_t kPinnedDroppedRow = 5;
constexpr std::size_t kPinnedOwnScenario = 0;
constexpr std::size_t kPinnedOwnEvaluator = 0;

// (b) Count-conserving: every draft 21 binding is either translated (all its translations are present) or
// untranslated with a reason, never both and never neither. Sizes pinned so a change is a visible event.
TEST(Draft22Bindings, EveryDraft21BindingIsTranslatedOrUntranslatedExactlyOnce) {
    const auto draft21 = draft21_executable_bindings();
    const auto shared = draft22_shared_bindings();
    const auto untranslated = draft22_untranslated_bindings();
    std::set<Key> shared_keys;
    for (const auto& binding : shared) shared_keys.insert(key(binding));

    std::size_t translated = 0;
    std::size_t expansions = 0;
    std::size_t fan_out = 0;  // draft 21 bindings producing more than one draft 22 binding
    for (const auto& binding : draft21) {
        const auto candidates = translations(binding);
        const auto matching = std::count_if(untranslated.begin(), untranslated.end(), [&](const auto& entry) {
            return entry.draft21.requirement_id == binding.requirement_id &&
                   entry.draft21.scenario_id == binding.scenario_id &&
                   entry.draft21.evaluator_id == binding.evaluator_id;
        });
        if (candidates.empty()) {
            EXPECT_EQ(matching, 1) << binding.requirement_id << " " << binding.scenario_id;
            continue;
        }
        EXPECT_EQ(matching, 0) << binding.requirement_id << " " << binding.scenario_id;
        ++translated;
        expansions += candidates.size();
        if (candidates.size() > 1) ++fan_out;
        for (const auto& candidate : candidates)
            EXPECT_TRUE(shared_keys.contains(key(candidate))) << candidate.requirement_id << " " << candidate.scenario_id;
    }
    EXPECT_EQ(translated + untranslated.size(), draft21.size());

    std::map<std::string, std::size_t> reasons;
    for (const auto& entry : untranslated) ++reasons[entry.reason];
    for (const auto& [reason, count] : reasons) {
        EXPECT_TRUE(reason == "own_row" || reason == "dropped_row" || reason == "own_scenario" ||
                    reason == "own_evaluator") << reason;
    }

    // Pinned measurements (update only with a reviewed binding or lineage change).
    EXPECT_EQ(draft21.size(), kPinnedDraft21);
    EXPECT_EQ(translated, kPinnedTranslated);
    EXPECT_EQ(expansions, kPinnedExpansions);
    EXPECT_EQ(fan_out, kPinnedFanOut);
    EXPECT_EQ(shared.size(), kPinnedShared);
    EXPECT_EQ(expansions - shared.size(), kPinnedCollapsed);  // removed by deduplication of one-to-many rows
    EXPECT_EQ(untranslated.size(), kPinnedUntranslated);
    EXPECT_EQ(reasons["own_row"], kPinnedOwnRow);
    EXPECT_EQ(reasons["dropped_row"], kPinnedDroppedRow);
    EXPECT_EQ(reasons["own_scenario"], kPinnedOwnScenario);
    EXPECT_EQ(reasons["own_evaluator"], kPinnedOwnEvaluator);
}

// Every untranslated draft 21 binding pinned individually as (row, scenario, reason), so a reclassification (for
// example D21-4-2-MUST-089 moving from dropped_row to own_row) fails by name, not only by a total.
TEST(Draft22Bindings, EachUntranslatedBindingKeepsItsReason) {
    using Entry = std::tuple<std::string, std::string, std::string>;
    std::vector<Entry> actual;
    for (const auto& entry : draft22_untranslated_bindings())
        actual.emplace_back(entry.draft21.requirement_id, entry.draft21.scenario_id, entry.reason);
    std::sort(actual.begin(), actual.end());
    const std::vector<Entry> expected{
        {"D21-3-3-1-MUST-NOT-057", "d21-subscribe-bounded-location-range", "own_row"},
        {"D21-3-3-1-MUST-NOT-057", "d21-update-subscription-location-range", "own_row"},
        {"D21-4-1-MUST-083", "d21-subscribe-tracks-accepted", "dropped_row"},
        {"D21-4-1-MUST-083", "d21-subscribe-tracks-rejected", "dropped_row"},
        {"D21-4-2-MUST-089", "d21-discover-original-publisher-namespaces", "dropped_row"},
        {"D21-9-15-MUST-383", "d21-subscribe-namespace-prefix-too-many-fields", "dropped_row"},
        {"D21-9-18-MUST-391", "d21-subscribe-tracks-prefix-too-many-fields", "dropped_row"},
        {"D21-9-20-10-MUST-432", "d21-fill-location-filter-end-group-overflow", "own_row"},
        {"D21-9-20-10-MUST-432", "d21-location-filter-end-group-overflow", "own_row"},
    };
    EXPECT_EQ(actual, expected);
}

// (c) No (row, scenario) is bound twice, in the shared half and in the whole draft 22 table.
TEST(Draft22Bindings, NoRowAndScenarioIsBoundTwice) {
    for (const auto& bindings : {draft22_shared_bindings(), draft22_executable_bindings()}) {
        std::set<std::pair<std::string, std::string>> seen;
        for (const auto& binding : bindings)
            EXPECT_TRUE(seen.insert({binding.requirement_id, binding.scenario_id}).second)
                << binding.requirement_id << " " << binding.scenario_id;
    }
}

// (d) D22-9-MUST-294 has two draft 21 source rows (D21-9-MUST-282 and -283) and exactly one binding per scenario.
// Only -282 carries a draft 21 binding today (-283 is NotApplicable there), so the collapse is also exercised on a
// synthetic list that binds both sources.
TEST(Draft22Bindings, OneToManyRowCollapsesToOneBindingPerScenario) {
    EXPECT_EQ(rows22("D21-9-MUST-282"), std::vector<std::string>{"D22-9-MUST-294"});
    EXPECT_EQ(rows22("D21-9-MUST-283"), std::vector<std::string>{"D22-9-MUST-294"});
    std::map<std::string, int> per_scenario;
    for (const auto& binding : draft22_shared_bindings())
        if (binding.requirement_id == "D22-9-MUST-294") ++per_scenario[binding.scenario_id];
    EXPECT_EQ(per_scenario, (std::map<std::string, int>{{"d22-publisher-request-stream-placement", 1}}));

    const std::vector<ExecutableBinding> synthetic{
        {21, "D21-9-MUST-282", "d21-publisher-request-stream-placement", "d21-publisher-first-message-placement",
         {"publish_observed", "response_delivered"}},
        {21, "D21-9-MUST-283", "d21-publisher-request-stream-placement", "d21-publisher-first-message-placement",
         {"publish_observed", "response_delivered"}},
    };
    const auto translation = translate_draft21_bindings(synthetic);
    EXPECT_TRUE(translation.untranslated.empty());
    ASSERT_EQ(translation.shared.size(), 1u);
    EXPECT_TRUE(same(translation.shared[0], {22, "D22-9-MUST-294", "d22-publisher-request-stream-placement",
                                             "d22-publisher-first-message-placement",
                                             {"publish_observed", "response_delivered"}}));
}

// Two sources with DIFFERENT evidence kinds collapse onto one binding whose kinds are their sorted, deduplicated
// union, whichever source comes first.
TEST(Draft22Bindings, CollapsedBindingCarriesTheSortedUnionOfItsSourcesEvidenceKinds) {
    const ExecutableBinding first{21, "D21-9-MUST-282", "d21-publisher-request-stream-placement",
                                  "d21-publisher-first-message-placement",
                                  {"response_delivered", "publish_observed"}};
    const ExecutableBinding second{21, "D21-9-MUST-283", "d21-publisher-request-stream-placement",
                                   "d21-publisher-first-message-placement",
                                   {"peer_setup_received", "publish_observed"}};
    const ExecutableBinding expected{22, "D22-9-MUST-294", "d22-publisher-request-stream-placement",
                                     "d22-publisher-first-message-placement",
                                     {"peer_setup_received", "publish_observed", "response_delivered"}};
    for (const auto& synthetic : {std::vector<ExecutableBinding>{first, second},
                                  std::vector<ExecutableBinding>{second, first}}) {
        const auto translation = translate_draft21_bindings(synthetic);
        EXPECT_TRUE(translation.untranslated.empty());
        ASSERT_EQ(translation.shared.size(), 1u);
        EXPECT_TRUE(same(translation.shared[0], expected));
    }
}

// The collapse key is (row, scenario): two sources onto the same draft 22 (row, scenario) naming different
// evaluators cannot be merged into one binding, and the translation refuses them instead of keeping either.
TEST(Draft22Bindings, CollapseOntoOneRowAndScenarioWithDifferentEvaluatorsIsRefused) {
    const std::vector<ExecutableBinding> synthetic{
        {21, "D21-9-MUST-282", "d21-publisher-request-stream-placement", "d21-publisher-first-message-placement",
         {"publish_observed"}},
        {21, "D21-9-MUST-283", "d21-publisher-request-stream-placement", "d21-request-stream-first-message-allowed",
         {"publish_observed"}},
    };
    ASSERT_FALSE(names22(lineage_data::kSharedEvaluators, "d21-request-stream-first-message-allowed").empty());
    EXPECT_THROW(translate_draft21_bindings(synthetic), std::logic_error);
}

// Reason priority own_row > dropped_row > own_scenario > own_evaluator on synthetic bindings.
TEST(Draft22Bindings, UntranslatedReasonsFollowTheirPriority) {
    const std::vector<ExecutableBinding> synthetic{
        {21, "D21-9-20-10-MUST-432", "d21-unknown", "d21-unknown", {"peer_close"}},
        {21, "D21-4-2-MUST-089", "d21-unknown", "d21-unknown", {"peer_close"}},
        {21, "D21-9-MUST-282", "d21-unknown", "d21-unknown", {"peer_close"}},
        {21, "D21-9-MUST-282", "d21-publisher-request-stream-placement", "d21-unknown", {"peer_close"}},
    };
    const auto translation = translate_draft21_bindings(synthetic);
    EXPECT_TRUE(translation.shared.empty());
    std::vector<std::string> reasons;
    for (const auto& entry : translation.untranslated) reasons.push_back(entry.reason);
    EXPECT_EQ(reasons, (std::vector<std::string>{"own_row", "dropped_row", "own_scenario", "own_evaluator"}));
}

// (e) A draft 21 binding on the ancestor of an own row is untranslated with reason own_row.
TEST(Draft22Bindings, BindingOnAnOwnRowAncestorIsUntranslatedAsOwnRow) {
    const auto untranslated = draft22_untranslated_bindings();
    const auto found = std::find_if(untranslated.begin(), untranslated.end(), [](const auto& entry) {
        return entry.draft21.requirement_id == "D21-9-20-10-MUST-432";
    });
    ASSERT_NE(found, untranslated.end());
    EXPECT_EQ(found->reason, "own_row");
    for (const auto& binding : draft22_shared_bindings())
        EXPECT_NE(binding.requirement_id, "D22-9-20-9-MUST-424");
}

// The own-row ancestor table is exactly what the delta file says.
TEST(Draft22Bindings, OwnRowAncestorTableMatchesTheDeltaFile) {
    std::set<std::string> own(lineage_data::kOwnRows22.begin(), lineage_data::kOwnRows22.end());
    std::set<std::string> expected;
    for (const auto& entry : load_delta_entries(kRoot / "requirements/draft21-to-22-delta.json"))
        if (!entry.d21.empty() && own.contains(entry.d22)) expected.insert(entry.d21);
    const std::set<std::string> actual(kDraft21AncestorsOfOwnRows22.begin(), kDraft21AncestorsOfOwnRows22.end());
    EXPECT_EQ(actual, expected);
    EXPECT_TRUE(std::is_sorted(kDraft21AncestorsOfOwnRows22.begin(), kDraft21AncestorsOfOwnRows22.end()));
}

bool contains(std::span<const std::string_view> ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

RequirementCatalog draft22_catalog() {
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    return RequirementCatalog::load(source, kRoot / "requirements/draft22.json", CatalogLoadMode::AllowIncomplete);
}

// The whole table is the shared half followed by the own half, sorted by (row, scenario, evaluator); the two
// halves are disjoint (no own binding names a shared row, and no shared binding names an own row).
TEST(Draft22Bindings, ExecutableBindingsAreTheSharedHalfPlusTheOwnHalf) {
    const auto shared = draft22_shared_bindings();
    const auto own = draft22_own_bindings();
    const auto all = draft22_executable_bindings();
    ASSERT_EQ(all.size(), shared.size() + own.size());
    EXPECT_TRUE(std::is_sorted(all.begin(), all.end(),
                               [](const auto& left, const auto& right) { return key(left) < key(right); }));
    for (const auto& binding : shared) {
        EXPECT_FALSE(contains(lineage_data::kOwnRows22, binding.requirement_id)) << binding.requirement_id;
        EXPECT_EQ(std::count_if(all.begin(), all.end(), [&](const auto& entry) { return same(entry, binding); }), 1);
    }
    for (const auto& binding : own)
        EXPECT_EQ(std::count_if(all.begin(), all.end(), [&](const auto& entry) { return same(entry, binding); }), 1);
}

// Pinned own half: one binding per (row, scenario) with the evaluator the row pairs it with, and the evidence
// kinds the evaluator reads (src/scenarios/draft22_*.cpp): the rebuilt stimulus and the transport events for the
// window probes, the stimulus and the peer close for the overflow close probes (as their draft 21 counterparts).
TEST(Draft22OwnBindings, BindEveryImplementedOwnRowWithTheEvidenceItsEvaluatorReads) {
    using Strings = std::vector<std::string>;
    const Strings window{"raw_probe_stimulus", "raw_probe_transport_event"};
    const Strings close{"raw_probe_stimulus", "peer_close"};
    const std::vector<ExecutableBinding> expected{
        {22, "D22-3-3-1-MUST-NOT-069", "d22-fetch-bounded-location-range",
         "d22-fetch-objects-within-requested-location-range", window},
        {22, "D22-3-3-1-MUST-NOT-069", "d22-subscribe-bounded-location-range",
         "d22-subscription-objects-within-effective-location-range", window},
        {22, "D22-3-3-1-MUST-NOT-069", "d22-update-subscription-location-range",
         "d22-subscription-objects-within-effective-location-range", window},
        {22, "D22-4-2-MUST-110", "d22-discover-original-publisher-namespaces",
         "d22-original-publisher-matching-namespace-notification", window},
        {22, "D22-6-3-MAY-159", "d22-request-stream-before-peer-setup", "d22-pre-setup-request-stream-reset", window},
        {22, "D22-9-20-9-MAY-422", "d22-publisher-location-filter-parameter",
         "d22-publisher-location-filter-capability", window},
        {22, "D22-9-20-9-MUST-424", "d22-fill-location-filter-end-group-overflow",
         "d22-location-filter-overflow-protocol-violation", close},
        {22, "D22-9-20-9-MUST-424", "d22-location-filter-end-group-overflow",
         "d22-location-filter-overflow-protocol-violation", close},
    };
    const auto own = draft22_own_bindings();
    ASSERT_EQ(own.size(), expected.size());
    for (std::size_t i = 0; i < own.size(); ++i)
        EXPECT_TRUE(same(own[i], expected[i])) << own[i].requirement_id << " " << own[i].scenario_id;
    for (const auto& binding : own) {
        EXPECT_TRUE(contains(lineage_data::kOwnRows22, binding.requirement_id)) << binding.requirement_id;
        EXPECT_TRUE(contains(lineage_data::kOwnScenarios22, binding.scenario_id)) << binding.scenario_id;
        EXPECT_TRUE(contains(lineage_data::kOwnEvaluators22, binding.evaluator_id)) << binding.evaluator_id;
    }
}

// (b) Coupling: every own scenario and every own evaluator that ANY applicable catalog row names (required or
// optional) is bound on that row, every own scenario and evaluator of the lineage is bound somewhere, and every
// implemented own scenario (app::kOwnScenarioTraits22) is bound. A new own scenario or evaluator cannot be
// registered or named by the catalog without a binding.
TEST(Draft22OwnBindings, EveryOwnScenarioAndEvaluatorNamedByAnApplicableRowIsBound) {
    const auto catalog = draft22_catalog();
    const auto own = draft22_own_bindings();
    const auto bound = [&](const std::string& row, auto&& match) {
        return std::any_of(own.begin(), own.end(),
                           [&](const auto& binding) { return binding.requirement_id == row && match(binding); });
    };
    std::set<std::string> rows_naming_own;
    for (const auto& row : catalog.requirements) {
        if (row.applicability != Applicability::Applicable) continue;
        for (const auto& scenario : row.scenarios) {
            if (!contains(lineage_data::kOwnScenarios22, scenario)) continue;
            rows_naming_own.insert(row.id);
            EXPECT_TRUE(bound(row.id, [&](const auto& binding) { return binding.scenario_id == scenario; }))
                << row.id << " " << scenario;
        }
        for (const auto& evaluator : row.evaluators) {
            if (!contains(lineage_data::kOwnEvaluators22, evaluator)) continue;
            rows_naming_own.insert(row.id);
            EXPECT_TRUE(bound(row.id, [&](const auto& binding) { return binding.evaluator_id == evaluator; }))
                << row.id << " " << evaluator;
        }
    }
    EXPECT_EQ(rows_naming_own, (std::set<std::string>{"D22-3-3-1-MUST-NOT-069", "D22-4-2-MUST-110",
                                                      "D22-6-3-MAY-159", "D22-9-20-9-MAY-422",
                                                      "D22-9-20-9-MUST-424"}));
    for (const auto scenario : lineage_data::kOwnScenarios22)
        EXPECT_TRUE(std::any_of(own.begin(), own.end(), [&](const auto& b) { return b.scenario_id == scenario; }))
            << scenario;
    for (const auto evaluator : lineage_data::kOwnEvaluators22)
        EXPECT_TRUE(std::any_of(own.begin(), own.end(), [&](const auto& b) { return b.evaluator_id == evaluator; }))
            << evaluator;
    for (const auto& traits : app::kOwnScenarioTraits22)
        EXPECT_TRUE(std::any_of(own.begin(), own.end(), [&](const auto& b) { return b.scenario_id == traits.id; }))
            << traits.id;
}

// (c) The evidence kinds of each own binding are known to the gate, and the own half alone covers every own row it
// binds with no blocking binding finding.
TEST(Draft22OwnBindings, OwnBindingsUseOnlyKnownEvidenceKindsAndCoverTheirRows) {
    const auto catalog = draft22_catalog();
    const auto own = draft22_own_bindings();
    ASSERT_FALSE(own.empty());
    const auto report = audit_completeness(catalog, own, app::executable_scenarios(22));
    std::set<std::string> bound_rows;
    for (const auto& binding : own) bound_rows.insert(binding.requirement_id);
    for (const auto& finding : report.findings) {
        EXPECT_NE(finding.code, "missing_evidence_schema") << finding.requirement_id;
        EXPECT_NE(finding.code, "nonexecutable_scenario") << finding.requirement_id;
        EXPECT_NE(finding.code, "mismatched_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "duplicate_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "orphan_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "wrong_draft_binding") << finding.requirement_id;
        EXPECT_FALSE(bound_rows.contains(finding.requirement_id)) << finding.code << " " << finding.requirement_id;
    }
}

// (a) The draft 22 gate on the whole table: every required row covered (170 of 170), the two own MAY rows added to
// the optional coverage inherited from draft 21, and no blocking finding: requirements/draft22.json is complete
// (D3 Task 4), so the incomplete_catalog finding that stood until then is gone and the gate passes.
TEST(Draft22Gate, CoversEveryRequiredRowAndPasses) {
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, kRoot / "requirements/draft22.json",
                                                  CatalogLoadMode::RequireComplete);
    const auto report = audit_completeness(catalog, draft22_executable_bindings(), app::executable_scenarios(22));
    EXPECT_EQ(report.draft, 22u);
    EXPECT_EQ(report.required_total, 170u);
    EXPECT_EQ(report.required_covered, 170u);
    EXPECT_EQ(report.optional_total, 97u);
    EXPECT_EQ(report.optional_covered, 3u);  // D21-9-20-SHOULD-401's successor, D22-6-3-MAY-159, D22-9-20-9-MAY-422
    std::vector<std::string> blocking;
    for (const auto& finding : report.findings)
        if (finding.blocking) blocking.push_back(finding.code + " " + finding.requirement_id);
    EXPECT_TRUE(blocking.empty());
    EXPECT_TRUE(catalog.complete);
    EXPECT_TRUE(report.complete());
    // The full-corpus keyword audit passes too: every normative occurrence is classified exactly once, every
    // citation anchors and the catalog is complete.
    const auto audit = audit_normative_occurrences(source, catalog);
    EXPECT_TRUE(audit.missing.empty());
    EXPECT_TRUE(audit.multiply_classified.empty());
    EXPECT_TRUE(audit.errors.empty());
    EXPECT_TRUE(audit.ok());
}

// Gap audit: runs the draft 22 gate on the whole table and prints every finding grouped by cause, then pins the
// counts per code: no binding defect of any kind, no uncovered required row, and only advisory optional gaps.
TEST(Draft22GapAudit, ReportsEveryUncoveredRequiredRow) {
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, kRoot / "requirements/draft22.json",
                                                  CatalogLoadMode::AllowIncomplete);
    const auto bindings = draft22_executable_bindings();
    const auto executable = app::executable_scenarios(22);
    const auto report = audit_completeness(catalog, bindings, executable);

    std::map<std::string, const Requirement*> rows;
    for (const auto& row : catalog.requirements) rows.emplace(row.id, &row);
    std::vector<std::string_view> shared_scenarios;
    for (const auto& pair : lineage_data::kSharedScenarios) shared_scenarios.push_back(pair.d22);
    std::vector<std::string_view> shared_evaluators;
    for (const auto& pair : lineage_data::kSharedEvaluators) shared_evaluators.push_back(pair.d22);
    const std::span<const std::string_view> own_rows(lineage_data::kOwnRows22);

    // 1 own row; 2 own/unshared or non-executable name; 3 translation miss; 4 unknown evidence kind; 0 other.
    std::map<int, std::vector<std::string>> groups;
    std::map<std::string, std::size_t> codes;
    for (const auto& finding : report.findings) {
        ++codes[finding.code];
        int cause = 0;
        if (finding.code == "missing_evidence_schema") {
            cause = 4;
        } else if (finding.code == "nonexecutable_scenario") {
            cause = 2;
        } else if (finding.code == "missing_required_evaluator") {
            if (contains(own_rows, finding.requirement_id)) {
                cause = 1;
            } else {
                const auto& row = *rows.at(finding.requirement_id);
                const bool unshared_or_nonexecutable =
                    std::any_of(row.scenarios.begin(), row.scenarios.end(), [&](const auto& id) {
                        return !contains(shared_scenarios, id) || !contains(executable, id);
                    }) ||
                    std::any_of(row.evaluators.begin(), row.evaluators.end(),
                                [&](const auto& id) { return !contains(shared_evaluators, id); });
                cause = unshared_or_nonexecutable ? 2 : 3;
            }
        }
        if (finding.code == "missing_optional_evaluator") continue;  // advisory, listed by count only
        groups[cause].push_back(finding.code + " " + finding.requirement_id + " :: " + finding.detail);
    }

    std::ostringstream out;
    out << "DRAFT22 GAP AUDIT bindings=" << bindings.size() << " required_total=" << report.required_total
        << " required_covered=" << report.required_covered << " optional_total=" << report.optional_total
        << " optional_covered=" << report.optional_covered << " findings=" << report.findings.size() << "\n";
    for (const auto& [code, count] : codes) out << "  code " << code << " = " << count << "\n";
    const char* names[] = {"0 other", "1 own rows awaiting own bindings",
                           "2 unshared or non-executable scenario/evaluator", "3 translation miss",
                           "4 unknown evidence kind"};
    for (const auto& [cause, lines] : groups) {
        out << "CAUSE " << names[cause] << " (" << lines.size() << ")\n";
        for (const auto& line : lines) out << "  " << line << "\n";
    }
    out << "UNTRANSLATED draft 21 bindings\n";
    for (const auto& entry : draft22_untranslated_bindings())
        out << "  " << entry.reason << " " << entry.draft21.requirement_id << " " << entry.draft21.scenario_id << " "
            << entry.draft21.evaluator_id << "\n";
    std::cout << out.str();

    EXPECT_EQ(report.draft, 22u);
    EXPECT_EQ(report.required_total, 170u);
    EXPECT_EQ(report.required_covered, 170u);
    EXPECT_EQ(codes["orphan_binding"], 0u);
    EXPECT_EQ(codes["duplicate_binding"], 0u);
    EXPECT_EQ(codes["mismatched_binding"], 0u);
    EXPECT_EQ(codes["nonexecutable_scenario"], 0u);
    EXPECT_EQ(codes["missing_evidence_schema"], 0u);
    EXPECT_EQ(codes["wrong_draft_binding"], 0u);
    EXPECT_EQ(codes["missing_required_evaluator"], 0u);
    EXPECT_EQ(codes["incomplete_catalog"], 0u);  // was 1 while requirements/draft22.json was complete:false
    EXPECT_EQ(codes["missing_optional_evaluator"], 94u);
}

}  // namespace
}  // namespace moq::interop::requirements
