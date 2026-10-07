#include "moq/interop/requirements/scoring.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace moq::interop::requirements {
namespace {

Requirement requirement(std::string id, Strength strength,
                        Applicability applicability = Applicability::Applicable,
                        Testability testability = Testability::Testable) {
    std::vector<std::string> scenarios;
    std::vector<std::string> evaluators;
    if (testability == Testability::Testable) {
        scenarios.push_back("scenario");
        evaluators.push_back("evaluator");
    }
    return {std::move(id), strength, {"1", 1, 1, 1, 1}, "publisher", "behavior",
            applicability, testability, std::move(scenarios), std::move(evaluators), "reason"};
}

RequirementCatalog catalog(std::vector<Requirement> requirements, unsigned draft = 18) {
    return {draft, "synthetic", true, std::move(requirements)};
}

void expect_ratio(const ScoreRatio& ratio, std::uint64_t earned, std::uint64_t possible) {
    EXPECT_EQ(ratio.earned, earned);
    EXPECT_EQ(ratio.possible, possible);
}

void expect_error(const ScoreSummary& summary) {
    EXPECT_EQ(summary.verdict, RunVerdict::Error);
    expect_ratio(summary.required, 0, 0);
    expect_ratio(summary.weighted, 0, 0);
    expect_ratio(summary.coverage, 0, 0);
}

TEST(ScoringTest, AppliesExactIntegerWeightsToEveryStrength) {
    const auto input = catalog({requirement("must", Strength::Must),
                                requirement("must-not", Strength::MustNot),
                                requirement("should", Strength::Should),
                                requirement("should-not", Strength::ShouldNot),
                                requirement("may", Strength::May)});
    const std::vector<Outcome> outcomes{{"must", OutcomeState::Pass},
                                        {"must-not", OutcomeState::Fail},
                                        {"should", OutcomeState::Pass},
                                        {"should-not", OutcomeState::Fail},
                                        {"may", OutcomeState::Pass}};

    const auto summary = score(input, outcomes);

    EXPECT_EQ(summary.verdict, RunVerdict::Fail);
    expect_ratio(summary.required, 10, 20);
    expect_ratio(summary.weighted, 14, 27);
    expect_ratio(summary.coverage, 27, 27);
}

TEST(ScoringTest, PreservesZeroOverZeroForOnlyExcludedRows) {
    const auto input = catalog(
        {requirement("not-testable", Strength::Must, Applicability::Applicable,
                     Testability::NotTestable),
         requirement("not-applicable", Strength::Should, Applicability::NotApplicable,
                     Testability::NotApplicable),
         requirement("informative", Strength::May, Applicability::Informative,
                     Testability::NotApplicable)});
    const std::vector<Outcome> outcomes{{"not-testable", OutcomeState::NotTestable},
                                        {"not-applicable", OutcomeState::NotApplicable},
                                        {"informative", OutcomeState::NotApplicable}};

    const auto summary = score(input, outcomes);

    EXPECT_EQ(summary.verdict, RunVerdict::Pass);
    expect_ratio(summary.required, 0, 0);
    expect_ratio(summary.weighted, 0, 0);
    expect_ratio(summary.coverage, 0, 0);
}

TEST(ScoringTest, AdvisoryAndOptionalFailuresDoNotFailConformance) {
    const auto input = catalog({requirement("must", Strength::Must),
                                requirement("should", Strength::Should),
                                requirement("may", Strength::May)});
    const std::vector<Outcome> outcomes{{"must", OutcomeState::Pass},
                                        {"should", OutcomeState::Fail},
                                        {"may", OutcomeState::Fail}};

    const auto summary = score(input, outcomes);

    EXPECT_EQ(summary.verdict, RunVerdict::Pass);
    expect_ratio(summary.required, 10, 10);
    expect_ratio(summary.weighted, 10, 14);
    expect_ratio(summary.coverage, 14, 14);
}

TEST(ScoringTest, NotRunRequiredOrNonRequiredRowsMakeRunIncomplete) {
    for (const auto strength : {Strength::Must, Strength::Should, Strength::May}) {
        const auto summary = score(catalog({requirement("row", strength)}),
                                   std::vector<Outcome>{{"row", OutcomeState::NotRun}});
        EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
        expect_ratio(summary.weighted, 0, strength == Strength::Must ? 10 :
                                          strength == Strength::Should ? 3 : 1);
        expect_ratio(summary.coverage, 0, summary.weighted.possible);
    }
}

TEST(ScoringTest, RequiredFailureTakesPrecedenceOverNotRun) {
    const auto input = catalog({requirement("must-not", Strength::MustNot),
                                requirement("unrun", Strength::Should)});
    const std::vector<Outcome> outcomes{{"must-not", OutcomeState::Fail},
                                        {"unrun", OutcomeState::NotRun}};

    const auto summary = score(input, outcomes);

    EXPECT_EQ(summary.verdict, RunVerdict::Fail);
    expect_ratio(summary.required, 0, 10);
    expect_ratio(summary.weighted, 0, 13);
    expect_ratio(summary.coverage, 10, 13);
}

TEST(ScoringTest, ContradictoryEvidenceFailsWithoutDoubleCountingWeight) {
    const auto input = catalog({requirement("must", Strength::Must)});
    const std::vector<Outcome> outcomes{{"must", OutcomeState::Pass},
                                        {"must", OutcomeState::Pass},
                                        {"must", OutcomeState::Fail},
                                        {"must", OutcomeState::Fail}};

    const auto summary = score(input, outcomes);

    EXPECT_EQ(summary.verdict, RunVerdict::Fail);
    expect_ratio(summary.required, 0, 10);
    expect_ratio(summary.weighted, 0, 10);
    expect_ratio(summary.coverage, 10, 10);
}

TEST(ScoringTest, RejectsMissingAndUnknownRequirementIds) {
    const auto input = catalog({requirement("known", Strength::Must)});
    expect_error(score(input, std::vector<Outcome>{}));
    expect_error(score(input, std::vector<Outcome>{{"known", OutcomeState::Pass},
                                                    {"unknown", OutcomeState::Pass}}));
}

TEST(ScoringTest, RejectsTerminalClassificationMixedWithEvidence) {
    const auto input = catalog({requirement("row", Strength::Must)});
    expect_error(score(input, std::vector<Outcome>{{"row", OutcomeState::NotRun},
                                                    {"row", OutcomeState::Pass}}));
    expect_error(score(input, std::vector<Outcome>{{"row", OutcomeState::NotRun},
                                                    {"row", OutcomeState::NotRun}}));
}

TEST(ScoringTest, RejectsOutcomesInconsistentWithCatalogClassification) {
    const auto testable = catalog({requirement("row", Strength::Must)});
    expect_error(score(testable,
                       std::vector<Outcome>{{"row", OutcomeState::NotTestable}}));
    // A scored row may be reported NotApplicable, but only as its single observation.
    expect_error(score(testable, std::vector<Outcome>{{"row", OutcomeState::NotApplicable},
                                                      {"row", OutcomeState::Pass}}));
    expect_error(score(testable, std::vector<Outcome>{{"row", OutcomeState::NotApplicable},
                                                      {"row", OutcomeState::NotApplicable}}));
    expect_error(score(testable, std::vector<Outcome>{{"row", OutcomeState::NotApplicable},
                                                      {"row", OutcomeState::NotRun}}));

    const auto not_testable = catalog({requirement("row", Strength::Must,
                                                    Applicability::Applicable,
                                                    Testability::NotTestable)});
    expect_error(score(not_testable, std::vector<Outcome>{{"row", OutcomeState::Pass}}));

    const auto not_applicable = catalog({requirement("row", Strength::May,
                                                      Applicability::Informative,
                                                      Testability::NotApplicable)});
    expect_error(score(not_applicable,
                       std::vector<Outcome>{{"row", OutcomeState::NotTestable}}));
}

TEST(ScoringTest, ScoredRowReportedNotApplicableLeavesEveryDenominator) {
    const auto input = catalog({requirement("must-passed", Strength::Must),
                                requirement("must-excluded", Strength::Must),
                                requirement("should-excluded", Strength::Should),
                                requirement("may-excluded", Strength::May),
                                requirement("should-passed", Strength::Should)});
    const auto summary = score(input, std::vector<Outcome>{{"must-passed", OutcomeState::Pass},
                                                           {"must-excluded", OutcomeState::NotApplicable},
                                                           {"should-excluded", OutcomeState::NotApplicable},
                                                           {"may-excluded", OutcomeState::NotApplicable},
                                                           {"should-passed", OutcomeState::Pass}});
    EXPECT_EQ(summary.verdict, RunVerdict::Pass);
    expect_ratio(summary.required, 10, 10);
    expect_ratio(summary.weighted, 13, 13);
    expect_ratio(summary.coverage, 13, 13);
}

TEST(ScoringTest, NotApplicableRowsNeitherFailNorMakeTheRunIncomplete) {
    const auto input = catalog({requirement("kept", Strength::Must),
                                requirement("excluded", Strength::Must)});
    // The excluded row would otherwise be NotRun and make the run incomplete.
    EXPECT_EQ(score(input, std::vector<Outcome>{{"kept", OutcomeState::Pass},
                                                {"excluded", OutcomeState::NotRun}}).verdict,
              RunVerdict::Incomplete);
    EXPECT_EQ(score(input, std::vector<Outcome>{{"kept", OutcomeState::Pass},
                                                {"excluded", OutcomeState::NotApplicable}}).verdict,
              RunVerdict::Pass);
    // Rows that still ran keep deciding the verdict.
    EXPECT_EQ(score(input, std::vector<Outcome>{{"kept", OutcomeState::Fail},
                                                {"excluded", OutcomeState::NotApplicable}}).verdict,
              RunVerdict::Fail);
    EXPECT_EQ(score(input, std::vector<Outcome>{{"kept", OutcomeState::NotRun},
                                                {"excluded", OutcomeState::NotApplicable}}).verdict,
              RunVerdict::Incomplete);
    // A run in which every scored row is excluded has nothing to score and is not an error.
    const auto all = score(input, std::vector<Outcome>{{"kept", OutcomeState::NotApplicable},
                                                       {"excluded", OutcomeState::NotApplicable}});
    EXPECT_EQ(all.verdict, RunVerdict::Pass);
    expect_ratio(all.required, 0, 0);
}

TEST(ScoringTest, RejectsInvalidCatalogIntegrity) {
    auto incomplete = catalog({requirement("row", Strength::Must)});
    incomplete.complete = false;
    expect_error(score(incomplete, std::vector<Outcome>{{"row", OutcomeState::Pass}}));

    auto duplicates = catalog({requirement("row", Strength::Must),
                               requirement("row", Strength::May)});
    expect_error(score(duplicates, std::vector<Outcome>{{"row", OutcomeState::Pass}}));

    const auto inconsistent = catalog({requirement("row", Strength::Must,
                                                   Applicability::NotApplicable,
                                                   Testability::Testable)});
    expect_error(score(inconsistent, std::vector<Outcome>{{"row", OutcomeState::Pass}}));
}

TEST(ScoringTest, ScoresEachDraftInputIndependently) {
    const auto draft18 = catalog({requirement("d18-required", Strength::Must)}, 18);
    const auto draft21 = catalog({requirement("d21-optional", Strength::May)}, 21);

    const auto result18 = score(
        draft18, std::vector<Outcome>{{"d18-required", OutcomeState::Pass}});
    const auto result21 = score(
        draft21, std::vector<Outcome>{{"d21-optional", OutcomeState::Fail}});

    EXPECT_EQ(result18.verdict, RunVerdict::Pass);
    expect_ratio(result18.required, 10, 10);
    expect_ratio(result18.weighted, 10, 10);
    EXPECT_EQ(result21.verdict, RunVerdict::Pass);
    expect_ratio(result21.required, 0, 0);
    expect_ratio(result21.weighted, 0, 1);
}

// ---- score_staged -------------------------------------------------------------------------
// Placeholder row the baseline generates: Applicable/NotTestable, reviewed == false.
Requirement unreviewed(std::string id, Strength strength) {
    auto placeholder = requirement(std::move(id), strength, Applicability::Applicable,
                                   Testability::NotTestable);
    placeholder.reviewed = false;
    return placeholder;
}

RequirementCatalog staged_catalog(std::vector<Requirement> requirements) {
    auto result = catalog(std::move(requirements), 106);
    result.complete = false;
    return result;
}

// Rows: A reviewed Must scored (10), B reviewed Should scored (3), C reviewed NotTestable,
// D reviewed NotApplicable, E unreviewed Must (10, required), F unreviewed May (1).
// required.possible = A + E = 20; weighted.possible = coverage.possible = 10+3+10+1 = 24.
RequirementCatalog staged_table() {
    return staged_catalog({
        requirement("A", Strength::Must),
        requirement("B", Strength::Should),
        requirement("C", Strength::Must, Applicability::Applicable, Testability::NotTestable),
        requirement("D", Strength::Must, Applicability::NotApplicable,
                    Testability::NotApplicable),
        unreviewed("E", Strength::Must),
        unreviewed("F", Strength::May)});
}

std::vector<Outcome> staged_outcomes(OutcomeState a, OutcomeState b,
                                     OutcomeState e = OutcomeState::NotRun,
                                     OutcomeState f = OutcomeState::NotRun) {
    return {{"A", a}, {"B", b}, {"C", OutcomeState::NotTestable},
            {"D", OutcomeState::NotApplicable}, {"E", e}, {"F", f}};
}

TEST(StagedScoringTest, AllReviewedPassIsIncompleteNeverPass) {
    const auto summary = score_staged(staged_table(),
        staged_outcomes(OutcomeState::Pass, OutcomeState::Pass));
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    expect_ratio(summary.required, 10, 20);   // A earned 10; E counts in possible only
    expect_ratio(summary.weighted, 13, 24);   // A 10 + B 3 earned
    expect_ratio(summary.coverage, 13, 24);   // A and B were run: 13
}

TEST(StagedScoringTest, ReviewedRequiredFailIsFail) {
    const auto summary = score_staged(staged_table(),
        staged_outcomes(OutcomeState::Fail, OutcomeState::Pass));
    EXPECT_EQ(summary.verdict, RunVerdict::Fail);
    expect_ratio(summary.required, 0, 20);
    expect_ratio(summary.weighted, 3, 24);    // only B earned
    expect_ratio(summary.coverage, 13, 24);   // a failed row was still run
}

TEST(StagedScoringTest, ReviewedOptionalFailIsIncomplete) {
    const auto summary = score_staged(staged_table(),
        staged_outcomes(OutcomeState::Pass, OutcomeState::Fail));
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    expect_ratio(summary.required, 10, 20);
    expect_ratio(summary.weighted, 10, 24);   // B failed: only A earned
    expect_ratio(summary.coverage, 13, 24);
}

TEST(StagedScoringTest, ReviewedNotRunIsIncomplete) {
    const auto summary = score_staged(staged_table(),
        staged_outcomes(OutcomeState::Pass, OutcomeState::NotRun));
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    expect_ratio(summary.required, 10, 20);
    expect_ratio(summary.weighted, 10, 24);
    expect_ratio(summary.coverage, 10, 24);   // B not run: only A (10)
}

TEST(StagedScoringTest, ReviewedRequiredFailBeatsNotRun) {
    const auto summary = score_staged(staged_table(),
        staged_outcomes(OutcomeState::Fail, OutcomeState::NotRun));
    EXPECT_EQ(summary.verdict, RunVerdict::Fail);
}

TEST(StagedScoringTest, ScoredRowDeclaredNotApplicableLeavesDenominators) {
    // As score(): a scored row the run declares NotApplicable leaves every denominator.
    const auto summary = score_staged(staged_table(),
        staged_outcomes(OutcomeState::Pass, OutcomeState::NotApplicable));
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    expect_ratio(summary.required, 10, 20);
    expect_ratio(summary.weighted, 10, 21);   // possible 24 - 3
    expect_ratio(summary.coverage, 10, 21);
}

TEST(StagedScoringTest, EveryNonNotRunOutcomeForUnreviewedRowIsError) {
    for (const auto state : {OutcomeState::Pass, OutcomeState::Fail, OutcomeState::NotTestable,
                             OutcomeState::NotApplicable}) {
        expect_error(score_staged(staged_table(),
            staged_outcomes(OutcomeState::Pass, OutcomeState::Pass, state)));
        expect_error(score_staged(staged_table(),
            staged_outcomes(OutcomeState::Pass, OutcomeState::Pass, OutcomeState::NotRun, state)));
    }
}

TEST(StagedScoringTest, DuplicateUnknownAndMissingOutcomesAreErrors) {
    auto duplicate = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    duplicate.push_back({"E", OutcomeState::NotRun});
    expect_error(score_staged(staged_table(), duplicate));

    auto duplicate_reviewed = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    duplicate_reviewed.push_back({"A", OutcomeState::Pass});
    // Two Pass outcomes for a scored row are valid in score() (evidence accumulates).
    EXPECT_EQ(score_staged(staged_table(), duplicate_reviewed).verdict, RunVerdict::Incomplete);

    auto mixed_reviewed = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    mixed_reviewed.push_back({"A", OutcomeState::NotRun});
    expect_error(score_staged(staged_table(), mixed_reviewed));

    auto unknown = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    unknown.push_back({"nope", OutcomeState::NotRun});
    expect_error(score_staged(staged_table(), unknown));

    auto missing_unreviewed = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    missing_unreviewed.erase(missing_unreviewed.begin() + 4);  // E
    expect_error(score_staged(staged_table(), missing_unreviewed));

    auto missing_reviewed = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    missing_reviewed.erase(missing_reviewed.begin());          // A
    expect_error(score_staged(staged_table(), missing_reviewed));
}

TEST(StagedScoringTest, WrongStatesForReviewedRowKindsAreErrors) {
    auto not_testable_passed = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    not_testable_passed[2].state = OutcomeState::Pass;         // C is NotTestable
    expect_error(score_staged(staged_table(), not_testable_passed));

    auto not_applicable_passed = staged_outcomes(OutcomeState::Pass, OutcomeState::Pass);
    not_applicable_passed[3].state = OutcomeState::Pass;       // D is NotApplicable
    expect_error(score_staged(staged_table(), not_applicable_passed));

    expect_error(score_staged(staged_table(),
        staged_outcomes(OutcomeState::NotTestable, OutcomeState::Pass)));
}

TEST(StagedScoringTest, InvalidCatalogRowsAreErrors) {
    expect_error(score_staged(staged_catalog({unreviewed("E", Strength::Must),
                                              unreviewed("E", Strength::May)}),
        std::vector<Outcome>{{"E", OutcomeState::NotRun}}));
    expect_error(score_staged(staged_catalog({requirement("", Strength::Must)}),
        std::vector<Outcome>{{"", OutcomeState::Pass}}));
}

TEST(StagedScoringTest, CompleteCatalogIsErrorAndScoreStillRejectsIncomplete) {
    auto complete_catalog = staged_table();
    complete_catalog.complete = true;
    expect_error(score_staged(complete_catalog,
        staged_outcomes(OutcomeState::Pass, OutcomeState::Pass)));
    // score() is untouched: the same incomplete catalog is an Error there.
    expect_error(score(staged_table(), staged_outcomes(OutcomeState::Pass, OutcomeState::Pass)));
}

TEST(StagedScoringTest, ZeroUnreviewedRowsStillNeverPasses) {
    const auto input = staged_catalog({requirement("A", Strength::Must),
                                       requirement("B", Strength::Should)});
    const std::vector<Outcome> outcomes{{"A", OutcomeState::Pass}, {"B", OutcomeState::Pass}};
    const auto summary = score_staged(input, outcomes);
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    expect_ratio(summary.required, 10, 10);
    expect_ratio(summary.weighted, 13, 13);
    expect_ratio(summary.coverage, 13, 13);
    // The same rows in a complete catalog pass.
    EXPECT_EQ(score(catalog({requirement("A", Strength::Must),
                             requirement("B", Strength::Should)}), outcomes).verdict,
              RunVerdict::Pass);
}

TEST(StagedScoringTest, OnlyUnreviewedRows) {
    // MustNot 10 required, ShouldNot 3, May 1: required 0/10, weighted 0/14, coverage 0/14.
    const auto input = staged_catalog({unreviewed("U1", Strength::MustNot),
                                       unreviewed("U2", Strength::ShouldNot),
                                       unreviewed("U3", Strength::May)});
    const auto summary = score_staged(input, std::vector<Outcome>{
        {"U1", OutcomeState::NotRun}, {"U2", OutcomeState::NotRun}, {"U3", OutcomeState::NotRun}});
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    expect_ratio(summary.required, 0, 10);
    expect_ratio(summary.weighted, 0, 14);
    expect_ratio(summary.coverage, 0, 14);
}

TEST(StagedScoringTest, ReviewedNotTestableAndNotApplicableRowsOnly) {
    const auto input = staged_catalog({
        requirement("C", Strength::Must, Applicability::Applicable, Testability::NotTestable),
        requirement("D", Strength::Should, Applicability::Informative,
                    Testability::NotApplicable)});
    const auto summary = score_staged(input, std::vector<Outcome>{
        {"C", OutcomeState::NotTestable}, {"D", OutcomeState::NotApplicable}});
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    expect_ratio(summary.required, 0, 0);
    expect_ratio(summary.weighted, 0, 0);
    expect_ratio(summary.coverage, 0, 0);
}

TEST(StagedScoringTest, StagedCountsOverTheTable) {
    // 6 rows; reviewed A B C D; unreviewed E (Must, required) and F (May).
    const auto counts = staged_counts(staged_table());
    EXPECT_EQ(counts.rows, 6u);
    EXPECT_EQ(counts.reviewed, 4u);
    EXPECT_EQ(counts.unreviewed, 2u);
    EXPECT_EQ(counts.unreviewed_required, 1u);
    const auto none = staged_counts(staged_catalog({}));
    EXPECT_EQ(none.rows, 0u);
    EXPECT_EQ(none.unreviewed_required, 0u);
}

}  // namespace
}  // namespace moq::interop::requirements
