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
    expect_error(score(testable,
                       std::vector<Outcome>{{"row", OutcomeState::NotApplicable}}));

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

}  // namespace
}  // namespace moq::interop::requirements
