#include "moq/interop/app/lineage_run.h"
#include "moq/interop/app/publisher_capabilities.h"

#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/lineage_translate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace moq::interop::app {
namespace {

RunConfig config(DraftVersion draft, std::vector<std::string> ids) {
    return {draft, TransportKind::NativeQuic, RunMode::Observed, std::move(ids),
            std::chrono::milliseconds(1000), std::nullopt, {}};
}

TEST(LineageRun, Drafts18And21AreUnchanged) {
    const auto c18 = config(DraftVersion::Draft18, {"anything"});
    const auto run18 = lineage_run(c18);
    ASSERT_TRUE(run18.has_value());
    EXPECT_EQ(run18->execution.draft, DraftVersion::Draft18);
    EXPECT_EQ(run18->wire_draft, DraftVersion::Draft18);
    EXPECT_EQ(run18->execution.scenario_ids, c18.scenario_ids);
    const auto run21 = lineage_run(config(DraftVersion::Draft21, {"d21-setup-unknown-options"}));
    ASSERT_TRUE(run21.has_value());
    EXPECT_EQ(run21->wire_draft, DraftVersion::Draft21);
}

TEST(LineageRun, Draft22SharedScenariosRunAsTheDraft21FamilyOnTheDraft22Wire) {
    const auto executable = executable_scenarios(22);
    ASSERT_FALSE(executable.empty());
    const std::string first(executable.front());
    const auto run = lineage_run(config(DraftVersion::Draft22, {first}));
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->wire_draft, DraftVersion::Draft22);
    EXPECT_EQ(run->execution.draft, DraftVersion::Draft21);
    ASSERT_EQ(run->execution.scenario_ids.size(), 1u);
    EXPECT_EQ(run->execution.scenario_ids.front(), std::string(*implementation_scenario_id(first)));
    // Everything that is not a scenario selection is carried over.
    EXPECT_EQ(run->execution.transport, TransportKind::NativeQuic);
    EXPECT_EQ(run->execution.timeout, std::chrono::milliseconds(1000));
}

TEST(LineageRun, Draft22OwnUnknownAndMixedSelectionsAreRefused) {
    EXPECT_FALSE(lineage_run(config(DraftVersion::Draft22, {"d22-location-filter-unknown-type"})).has_value());
    EXPECT_FALSE(lineage_run(config(DraftVersion::Draft22, {"d21-setup-unknown-options"})).has_value());
    const std::string executable(executable_scenarios(22).front());
    EXPECT_FALSE(lineage_run(config(DraftVersion::Draft22, {executable, "no-such-scenario"})).has_value());
    // A shared scenario whose draft 21 implementation does not exist yet is refused too.
    for (const auto d22 : shared_scenario_ids_22()) {
        if (!executable_scenario(22, d22)) {
            EXPECT_FALSE(lineage_run(config(DraftVersion::Draft22, {std::string(d22)})).has_value()) << d22;
            break;
        }
    }
}

// The draft 22 outcome set and score of a lineage run (what the run manager stores).

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / "requirements" / ("draft" + std::to_string(draft) + ".json"),
        requirements::CatalogLoadMode::AllowIncomplete));
}

// What the draft 21 evaluators hand over when nothing was observed: one outcome per row.
std::vector<requirements::Outcome> unobserved(const requirements::RequirementCatalog& catalog) {
    std::vector<requirements::Outcome> outcomes;
    for (const auto& row : catalog.requirements) {
        auto state = requirements::OutcomeState::NotRun;
        if (row.applicability != requirements::Applicability::Applicable)
            state = requirements::OutcomeState::NotApplicable;
        else if (row.testability == requirements::Testability::NotTestable)
            state = requirements::OutcomeState::NotTestable;
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

TEST(LineageRunOutcomes, OneDraft22OutcomePerCatalogRowInCatalogOrder) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    auto source = unobserved(*d21);
    // Mark every row of one shared pair as passed, to see the translated state arrive.
    const auto& pair = requirements::lineage_data::kSharedRows.front();
    for (auto& outcome : source)
        if (outcome.requirement_id == pair.d21) outcome.state = requirements::OutcomeState::Pass;
    const auto outcomes = lineage_outcomes(*d22, source);
    ASSERT_EQ(outcomes.size(), d22->requirements.size());
    const auto translated = requirements::translate_shared_outcomes(source);
    std::map<std::string, requirements::OutcomeState> shared;
    for (const auto& outcome : translated) shared.emplace(outcome.requirement_id, outcome.state);
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        const auto& row = d22->requirements[index];
        SCOPED_TRACE(row.id);
        EXPECT_EQ(outcomes[index].requirement_id, row.id);
        EXPECT_EQ(outcomes[index].requirement_id.rfind("D22-", 0), 0u);
        const auto found = shared.find(row.id);
        if (found != shared.end()) {
            EXPECT_EQ(outcomes[index].state, found->second);
        } else if (row.applicability != requirements::Applicability::Applicable) {
            EXPECT_EQ(outcomes[index].state, requirements::OutcomeState::NotApplicable);
        } else if (row.testability == requirements::Testability::NotTestable) {
            EXPECT_EQ(outcomes[index].state, requirements::OutcomeState::NotTestable);
        } else {
            // An own row has no evaluator yet (sub-project D2): it was not run.
            EXPECT_EQ(outcomes[index].state, requirements::OutcomeState::NotRun);
        }
    }
    const auto passed = std::find_if(outcomes.begin(), outcomes.end(),
                                     [&](const auto& outcome) { return outcome.requirement_id == pair.d22; });
    ASSERT_NE(passed, outcomes.end());
    EXPECT_EQ(passed->state, requirements::OutcomeState::Pass);
}

TEST(LineageRunOutcomes, ATranslatedNotApplicableNeverDropsAnApplicableTestableRow) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    const requirements::Requirement* target = nullptr;
    std::string d21_id;
    for (const auto& pair : requirements::lineage_data::kSharedRows) {
        const auto row = std::find_if(d22->requirements.begin(), d22->requirements.end(),
                                      [&](const auto& r) { return r.id == pair.d22; });
        if (row != d22->requirements.end() && row->applicability == requirements::Applicability::Applicable &&
            row->testability != requirements::Testability::NotTestable) {
            target = &*row;
            d21_id = pair.d21;
            break;
        }
    }
    ASSERT_NE(target, nullptr);
    auto source = unobserved(*d21);
    for (auto& outcome : source)
        if (outcome.requirement_id == d21_id) outcome.state = requirements::OutcomeState::NotApplicable;
    const auto outcomes = lineage_outcomes(*d22, source);
    const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                    [&](const auto& o) { return o.requirement_id == target->id; });
    ASSERT_NE(found, outcomes.end());
    EXPECT_EQ(found->state, requirements::OutcomeState::NotRun);
}

// The outcomes of a run in which every scored row of `catalog` passed: Pass for each applicable testable row,
// the row's own class otherwise.
std::vector<requirements::Outcome> all_passed(const requirements::RequirementCatalog& catalog) {
    auto outcomes = unobserved(catalog);
    for (auto& outcome : outcomes)
        if (outcome.state == requirements::OutcomeState::NotRun) outcome.state = requirements::OutcomeState::Pass;
    return outcomes;
}

bool is_required_scored(const requirements::Requirement& row) {
    return row.applicability == requirements::Applicability::Applicable &&
           row.testability == requirements::Testability::Testable &&
           (row.strength == requirements::Strength::Must || row.strength == requirements::Strength::MustNot);
}

// requirements/draft22.json is complete (D3), so a draft 22 run is scored by requirements::score like drafts 18
// and 21: nothing observed is Incomplete with the full denominators.
TEST(LineageRunOutcomes, ScoresAgainstTheCompleteDraft22Catalog) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    ASSERT_TRUE(d22->complete);
    const auto outcomes = lineage_outcomes(*d22, unobserved(*d21));
    const auto summary = requirements::score(*d22, outcomes);
    EXPECT_EQ(summary.verdict, requirements::RunVerdict::Incomplete);
    std::uint64_t required = 0;
    std::uint64_t weighted = 0;
    for (const auto& row : d22->requirements) {
        if (row.applicability != requirements::Applicability::Applicable ||
            row.testability != requirements::Testability::Testable) continue;
        weighted += requirements::score_weight(row.strength);
        if (row.strength == requirements::Strength::Must || row.strength == requirements::Strength::MustNot)
            required += requirements::score_weight(row.strength);
    }
    EXPECT_EQ(summary.coverage.possible, weighted);
    EXPECT_EQ(summary.weighted.possible, weighted);
    EXPECT_EQ(summary.required.possible, required);
    EXPECT_EQ(summary.coverage.earned, 0u);

    // Without the complete flag score() refuses the catalog outright (Error, no denominators): the flag, not a
    // lineage-specific scorer, is what lets a draft 22 run be scored.
    requirements::RequirementCatalog small{22, "test", false, {}};
    small.requirements.push_back({"D22-1-MUST-001", requirements::Strength::Must, {}, "", "",
                                  requirements::Applicability::Applicable, requirements::Testability::Testable,
                                  {"d22-x"}, {"d22-y"}, ""});
    const std::vector<requirements::Outcome> all_pass{{"D22-1-MUST-001", requirements::OutcomeState::Pass}};
    EXPECT_EQ(requirements::score(small, all_pass).verdict, requirements::RunVerdict::Error);
    EXPECT_EQ(requirements::score(small, all_pass).required.possible, 0u);
    small.complete = true;
    EXPECT_EQ(requirements::score(small, all_pass).verdict, requirements::RunVerdict::Pass);
    EXPECT_EQ(requirements::score(small, all_pass).required.earned, 10u);
}

// With the catalog complete: every scored row passing is a Pass, one required Fail fails the run, and one
// required row left unexecuted (NotRun) keeps it Incomplete, never a Pass.
TEST(LineageRunOutcomes, AFullPassPassesAFailFailsAndAnUnexecutedRequiredRowIsIncomplete) {
    const auto d22 = catalog(22);
    ASSERT_TRUE(d22->complete);
    const auto passed = all_passed(*d22);
    const auto pass = requirements::score(*d22, passed);
    EXPECT_EQ(pass.verdict, requirements::RunVerdict::Pass);
    EXPECT_GT(pass.required.possible, 0u);
    EXPECT_EQ(pass.required.earned, pass.required.possible);
    EXPECT_EQ(pass.weighted.earned, pass.weighted.possible);
    EXPECT_EQ(pass.coverage.earned, pass.coverage.possible);

    const auto required = std::find_if(d22->requirements.begin(), d22->requirements.end(), is_required_scored);
    ASSERT_NE(required, d22->requirements.end());
    const auto with_state = [&](requirements::OutcomeState state) {
        auto outcomes = passed;
        for (auto& outcome : outcomes)
            if (outcome.requirement_id == required->id) outcome.state = state;
        return requirements::score(*d22, outcomes);
    };
    const auto failed = with_state(requirements::OutcomeState::Fail);
    EXPECT_EQ(failed.verdict, requirements::RunVerdict::Fail);
    EXPECT_EQ(failed.required.earned, pass.required.possible - 10u);
    EXPECT_EQ(failed.coverage.earned, pass.coverage.possible) << "a Fail was evaluated, so it counts as covered";
    const auto unexecuted = with_state(requirements::OutcomeState::NotRun);
    EXPECT_EQ(unexecuted.verdict, requirements::RunVerdict::Incomplete);
    EXPECT_EQ(unexecuted.required.possible, pass.required.possible) << "a NotRun row stays in the denominator";
    EXPECT_EQ(unexecuted.required.earned, pass.required.possible - 10u);
    EXPECT_EQ(unexecuted.coverage.earned, pass.coverage.possible - 10u);
}

// The realistic lineage case: every draft 21 evaluator passed but the own draft 22 evaluators produced nothing.
// The own required rows (069, 110, 424) are NotRun, so the run is Incomplete although the catalog is complete;
// once the own rows pass too, every scored draft 22 row has passed and the run is a Pass.
TEST(LineageRunOutcomes, AFullDraft21PassWithoutOwnOutcomesIsIncompleteNotAPass) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    ASSERT_TRUE(d22->complete);
    const auto source = all_passed(*d21);
    const auto shared_only = lineage_outcomes(*d22, source);
    ASSERT_EQ(shared_only.size(), d22->requirements.size());
    std::set<std::string> not_run_required;
    for (std::size_t index = 0; index < shared_only.size(); ++index)
        if (shared_only[index].state == requirements::OutcomeState::NotRun && is_required_scored(d22->requirements[index]))
            not_run_required.insert(shared_only[index].requirement_id);
    // Exactly the own rows that are required and scored (the other own MUST rows are classified not testable):
    // every shared required row is reached by the translation and passes.
    EXPECT_EQ(not_run_required, (std::set<std::string>{"D22-3-3-1-MUST-NOT-069", "D22-4-2-MUST-110",
                                                        "D22-9-20-9-MUST-424"}));
    EXPECT_EQ(requirements::score(*d22, shared_only).verdict, requirements::RunVerdict::Incomplete);

    std::vector<requirements::Outcome> own;
    for (const auto id : requirements::lineage_data::kOwnRows22) {
        const auto row = std::find_if(d22->requirements.begin(), d22->requirements.end(),
                                      [&](const auto& r) { return r.id == id; });
        ASSERT_NE(row, d22->requirements.end()) << id;
        if (row->applicability == requirements::Applicability::Applicable &&
            row->testability == requirements::Testability::Testable)
            own.push_back({std::string(id), requirements::OutcomeState::Pass});
    }
    const auto full = lineage_outcomes(*d22, source, own);
    const auto summary = requirements::score(*d22, full);
    EXPECT_EQ(summary.verdict, requirements::RunVerdict::Pass);
    EXPECT_EQ(summary.required.earned, summary.required.possible);
}

// ---- Own draft 22 scenarios and outcomes (Task 8) ------------------------------------------------------

TEST(LineageRunOwn, ARegisteredOwnScenarioStillHasNoLineageRun) {
    const ScopedOwnScenario22 stub({{"d22-request-stream-before-peer-setup", false}, {}});
    ASSERT_TRUE(executable_scenario(22, "d22-request-stream-before-peer-setup"));
    // The run manager dispatches own ids natively; the family path keeps refusing them.
    EXPECT_FALSE(lineage_run(config(DraftVersion::Draft22, {"d22-request-stream-before-peer-setup"})).has_value());
}

TEST(LineageRunOwn, OwnOutcomesFillTheirDraft22RowsAndNothingElse) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    const auto source = unobserved(*d21);
    const std::vector<requirements::Outcome> own{{"D22-6-3-MAY-159", requirements::OutcomeState::Pass},
                                                 {"D22-3-3-1-MUST-NOT-069", requirements::OutcomeState::Fail}};
    const auto with = lineage_outcomes(*d22, source, own);
    const auto without = lineage_outcomes(*d22, source);
    ASSERT_EQ(with.size(), d22->requirements.size());
    ASSERT_EQ(without.size(), with.size());
    for (std::size_t index = 0; index < with.size(); ++index) {
        SCOPED_TRACE(with[index].requirement_id);
        EXPECT_EQ(with[index].requirement_id, d22->requirements[index].id);
        if (with[index].requirement_id == "D22-6-3-MAY-159")
            EXPECT_EQ(with[index].state, requirements::OutcomeState::Pass);
        else if (with[index].requirement_id == "D22-3-3-1-MUST-NOT-069")
            EXPECT_EQ(with[index].state, requirements::OutcomeState::Fail);
        else
            EXPECT_EQ(with[index].state, without[index].state);
    }
}

TEST(LineageRunOwn, AnOwnOutcomeForARowTheCatalogLacksIsKeptAtTheEnd) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    const std::vector<requirements::Outcome> own{{"D22-NO-SUCH-ROW", requirements::OutcomeState::Pass}};
    const auto outcomes = lineage_outcomes(*d22, unobserved(*d21), own);
    ASSERT_EQ(outcomes.size(), d22->requirements.size() + 1);
    EXPECT_EQ(outcomes.back().requirement_id, "D22-NO-SUCH-ROW");
}

TEST(LineageRunOwn, AnOwnOutcomeThatDuplicatesAnotherOutcomeThrows) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    const auto source = unobserved(*d21);
    // A row the translation reached.
    const auto& shared_row = requirements::lineage_data::kSharedRows.front();
    const std::vector<requirements::Outcome> translated{{std::string(shared_row.d22), requirements::OutcomeState::Pass}};
    EXPECT_THROW(lineage_outcomes(*d22, source, translated), std::logic_error);
    // Two own outcomes for one row.
    const std::vector<requirements::Outcome> twice{{"D22-6-3-MAY-159", requirements::OutcomeState::Pass},
                                                   {"D22-6-3-MAY-159", requirements::OutcomeState::Fail}};
    EXPECT_THROW(lineage_outcomes(*d22, source, twice), std::logic_error);
}

// The data guard behind lineage_outcomes and evaluate_own_draft22: a row that names an own scenario or an own
// evaluator is an own row and names nothing shared, so it never receives a translated outcome as well, and
// every scenario it names carries a draft 22 id in the transcripts.
TEST(LineageRunOwn, RowsNamingOwnIdsAreOwnRowsAndNameOnlyOwnIds) {
    const auto d22 = catalog(22);
    const auto contains = [](const auto& list, std::string_view id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    };
    const auto& own_scenarios = requirements::lineage_data::kOwnScenarios22;
    const auto& own_evaluators = requirements::lineage_data::kOwnEvaluators22;
    std::size_t naming_own = 0;
    for (const auto& row : d22->requirements) {
        const bool names_own =
            std::any_of(row.scenarios.begin(), row.scenarios.end(),
                        [&](const auto& id) { return contains(own_scenarios, id); }) ||
            std::any_of(row.evaluators.begin(), row.evaluators.end(),
                        [&](const auto& id) { return contains(own_evaluators, id); });
        if (!names_own) continue;
        ++naming_own;
        SCOPED_TRACE(row.id);
        EXPECT_TRUE(contains(requirements::lineage_data::kOwnRows22, row.id));
        for (const auto& id : row.scenarios) EXPECT_TRUE(contains(own_scenarios, id)) << id;
        for (const auto& id : row.evaluators) EXPECT_TRUE(contains(own_evaluators, id)) << id;
    }
    EXPECT_GT(naming_own, 0u) << "guards against a vacuous walk";
}

// Row 069 with the publisher declaring no FETCH: passing SUBSCRIBE evidence alone never passes it, and it stays
// in the denominators (the FETCH scenario was skipped, so its evaluator was never exercised).
TEST(LineageRunOwn, Row069StaysNotRunAndScoredWithoutFetchEvidence) {
    const auto d21 = catalog(21);
    const auto d22 = catalog(22);
    const PublisherCapabilities no_fetch{.fetch = false};
    // What evaluate_own_draft22 yields for 069 when only the SUBSCRIBE contexts ran.
    const std::vector<requirements::Outcome> own{{"D22-3-3-1-MUST-NOT-069", requirements::OutcomeState::NotRun}};
    auto outcomes = lineage_outcomes(*d22, unobserved(*d21), own);
    apply_publisher_capabilities(22, *d22, no_fetch, outcomes);
    const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                    [](const auto& outcome) { return outcome.requirement_id == "D22-3-3-1-MUST-NOT-069"; });
    ASSERT_NE(found, outcomes.end());
    EXPECT_EQ(found->state, requirements::OutcomeState::NotRun);
    const auto with_row = requirements::score(*d22, outcomes);
    EXPECT_EQ(with_row.verdict, requirements::RunVerdict::Incomplete);
    auto dropped = outcomes;
    for (auto& outcome : dropped)
        if (outcome.requirement_id == "D22-3-3-1-MUST-NOT-069") outcome.state = requirements::OutcomeState::NotApplicable;
    EXPECT_EQ(with_row.required.possible, requirements::score(*d22, dropped).required.possible + 10u)
        << "069 stays in the required denominator";
}

TEST(LineageRunOwn, FetchExclusionOnTheDraft22Catalog) {
    const auto d22 = catalog(22);
    const PublisherCapabilities no_fetch{.fetch = false};
    std::size_t excluded = 0;
    std::size_t naming_own_fetch = 0;
    const requirements::Requirement* row069 = nullptr;
    for (const auto& row : d22->requirements) {
        if (row.id == "D22-3-3-1-MUST-NOT-069") row069 = &row;
        if (!row_not_applicable_reason(22, row, no_fetch)) continue;
        ++excluded;
        naming_own_fetch += std::count(row.scenarios.begin(), row.scenarios.end(), "d22-fetch-bounded-location-range");
    }
    // Only shared FETCH rows leave (25, the same count as before the own FETCH scenario was known to need
    // FETCH): the only row naming it (069) also names the two own SUBSCRIBE scenarios, and a mixed row
    // keeps the "every named scenario" rule.
    EXPECT_EQ(excluded, 25u);
    EXPECT_EQ(naming_own_fetch, 0u);
    ASSERT_NE(row069, nullptr);
    EXPECT_FALSE(row_not_applicable_reason(22, *row069, no_fetch).has_value());
    EXPECT_TRUE(scenario_skip_reason(22, "d22-fetch-bounded-location-range", no_fetch).has_value());
    // A row naming only the own FETCH scenario leaves the run like a shared FETCH-only row.
    auto fetch_only = *row069;
    fetch_only.id = "D22-FETCH-ONLY";
    fetch_only.scenarios = {"d22-fetch-bounded-location-range"};
    EXPECT_TRUE(row_not_applicable_reason(22, fetch_only, no_fetch).has_value());
    EXPECT_FALSE(row_not_applicable_reason(22, fetch_only, {}).has_value());
    requirements::RequirementCatalog small = *d22;
    small.requirements = {fetch_only};
    std::vector<requirements::Outcome> outcomes{{"D22-FETCH-ONLY", requirements::OutcomeState::NotRun}};
    apply_publisher_capabilities(22, small, no_fetch, outcomes);
    EXPECT_EQ(outcomes.front().state, requirements::OutcomeState::NotApplicable);
}

}  // namespace
}  // namespace moq::interop::app
