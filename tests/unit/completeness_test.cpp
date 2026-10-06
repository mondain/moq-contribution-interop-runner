#include "moq/interop/requirements/completeness.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_evaluators.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace moq::interop::requirements {
namespace {

constexpr std::array<std::string_view, 1> kSyntheticScenarios{"scenario"};

Requirement row(std::string id, Strength strength,
                Testability testability = Testability::Testable) {
    return {std::move(id), strength, {"1", 1, 1, 1, 1}, "publisher",
            "Synthetic observable behavior", Applicability::Applicable,
            testability, {"scenario"}, {"evaluator"}, "Synthetic reason"};
}

ExecutableBinding binding(std::string id) {
    return {18, std::move(id), "scenario", "evaluator", {"request_observed"}};
}

TEST(CompletenessTest, DistinguishesRequiredCoverageFromAdvisoryCoverage) {
    RequirementCatalog catalog{18, "source", true,
        {row("required-covered", Strength::Must),
         row("required-missing", Strength::MustNot),
         row("optional-missing", Strength::Should),
         row("untestable", Strength::Must, Testability::NotTestable)}};
    const std::vector bindings{binding("required-covered")};
    const auto report = audit_completeness(catalog, bindings, kSyntheticScenarios);
    EXPECT_FALSE(report.complete());
    EXPECT_EQ(report.required_total, 2u);
    EXPECT_EQ(report.required_covered, 1u);
    EXPECT_EQ(report.optional_total, 1u);
    EXPECT_EQ(report.optional_covered, 0u);
    ASSERT_EQ(report.findings.size(), 2u);
    EXPECT_EQ(report.findings[0].requirement_id, "optional-missing");
    EXPECT_FALSE(report.findings[0].blocking);
    EXPECT_EQ(report.findings[1].requirement_id, "required-missing");
    EXPECT_TRUE(report.findings[1].blocking);
}

TEST(CompletenessTest, RejectsOrphanMismatchDuplicateAndMissingEvidence) {
    RequirementCatalog catalog{18, "source", true,
        {row("covered", Strength::Must), row("missing", Strength::Must)}};
    auto mismatched = binding("missing");
    mismatched.evaluator_id = "deleted-evaluator";
    auto evidence_free = binding("missing");
    evidence_free.evidence_kinds.clear();
    const std::vector bindings{binding("covered"), binding("covered"),
        binding("orphan"), mismatched, evidence_free};
    const auto report = audit_completeness(catalog, bindings, kSyntheticScenarios);
    EXPECT_FALSE(report.complete());
    EXPECT_EQ(report.required_covered, 1u);
    const auto has = [&](std::string_view code) {
        return std::any_of(report.findings.begin(), report.findings.end(),
            [&](const auto& finding) { return finding.code == code; });
    };
    EXPECT_TRUE(has("duplicate_binding"));
    EXPECT_TRUE(has("orphan_binding"));
    EXPECT_TRUE(has("mismatched_binding"));
    EXPECT_TRUE(has("missing_evidence_schema"));
    EXPECT_TRUE(has("missing_required_evaluator"));
}

TEST(CompletenessTest, CompleteSyntheticCatalogPasses) {
    RequirementCatalog catalog{21, "source", true,
        {row("required", Strength::Must), row("optional", Strength::May)}};
    auto required = binding("required");
    required.draft = 21;
    auto optional = binding("optional");
    optional.draft = 21;
    const std::vector bindings{required, optional};
    const auto report = audit_completeness(catalog, bindings, kSyntheticScenarios);
    EXPECT_TRUE(report.complete());
    EXPECT_TRUE(report.findings.empty());
}
TEST(CompletenessTest, PartialScenarioOrEvaluatorBindingsKeepRequiredRowUncovered) {
    auto required = row("required",Strength::Must);
    required.scenarios.push_back("second-scenario");
    required.evaluators.push_back("second-evaluator");
    const RequirementCatalog catalog{18,"source",true,{required}};
    const std::array<std::string_view,2> scenarios{"scenario","second-scenario"};
    std::vector bindings{binding("required")};
    auto report = audit_completeness(catalog,bindings,scenarios);
    EXPECT_EQ(report.required_covered,0u);
    EXPECT_FALSE(report.complete());
    EXPECT_TRUE(std::any_of(report.findings.begin(),report.findings.end(),[](const auto& finding) {
        return finding.code == "missing_required_evaluator" && finding.requirement_id == "required" &&
            finding.detail.find("missing_scenario=second-scenario") != std::string::npos &&
            finding.detail.find("missing_evaluator=second-evaluator") != std::string::npos;
    }));
    auto second_scenario = binding("required");
    second_scenario.scenario_id = "second-scenario";
    bindings.push_back(second_scenario);
    report = audit_completeness(catalog,bindings,scenarios);
    EXPECT_EQ(report.required_covered,0u);
    EXPECT_FALSE(report.complete());
    ASSERT_EQ(report.findings.size(),1u);
    EXPECT_NE(report.findings.front().detail.find("missing_evaluator=second-evaluator"),std::string::npos);
    EXPECT_EQ(report.findings.front().detail.find("missing_scenario="),std::string::npos);
    bindings.back().evaluator_id = "second-evaluator";
    report = audit_completeness(catalog,bindings,scenarios);
    EXPECT_EQ(report.required_covered,1u);
    EXPECT_TRUE(report.complete());
    EXPECT_TRUE(report.findings.empty());
}

TEST(CompletenessTest, ReportsRequiredRowCompletionForAllDrafts) {
    const auto root = std::filesystem::path{MOQ_INTEROP_PROJECT_SOURCE_DIR};
    for (const unsigned draft : {18u, 21u, 22u}) {
        const auto source = load_draft_source(
            draft, root / "docs", root / "requirements" / "draft-digests.json");
        const auto catalog = RequirementCatalog::load(
            source, root / "requirements" /
                ("draft" + std::to_string(draft) + ".json"),
            draft == 22 ? CatalogLoadMode::AllowIncomplete : CatalogLoadMode::RequireComplete);
        const auto bindings = draft == 18 ? draft18_executable_bindings()
            : draft == 21 ? draft21_executable_bindings()
                          : draft22_executable_bindings();
        const auto report = audit_completeness(
            catalog, bindings, app::executable_scenarios(draft));
        // The required set is the testable MUST and MUST NOT rows. It started at 175 rows per
        // draft and shrinks only when a row is reclassified as not testable in the catalog.
        const auto expected_required = static_cast<std::size_t>(std::count_if(
            catalog.requirements.begin(), catalog.requirements.end(), [](const auto& row) {
                return row.applicability == Applicability::Applicable &&
                       row.testability == Testability::Testable &&
                       (row.strength == Strength::Must || row.strength == Strength::MustNot);
            }));
        EXPECT_EQ(report.required_total, expected_required);
        // Every draft reaches required-row completion: every required row has a binding, so
        // losing a binding (or a scenario registration) fails here. Draft 22 has 170 required
        // rows against 173 for drafts 18 and 21: its catalog has three fewer testable MUST or
        // MUST NOT rows than draft 21's.
        EXPECT_EQ(report.required_total, draft == 22 ? 170u : 173u);
        EXPECT_EQ(report.required_covered, report.required_total);
        const auto blocking = static_cast<std::size_t>(std::count_if(
            report.findings.begin(), report.findings.end(),
            [](const auto& finding) {
                return finding.blocking &&
                       finding.code == "missing_required_evaluator";
            }));
        EXPECT_EQ(report.required_total - report.required_covered, blocking);
        const auto audit = audit_normative_occurrences(source, catalog);
        if (draft == 22) {
            // requirements/draft22.json is still complete:false, so the only blocking finding
            // is incomplete_catalog and the only audit error is the incomplete-catalog one.
            // TODO(D3 Task 4): when the catalog flips to complete:true, replace this branch
            // with EXPECT_TRUE(report.complete()) and EXPECT_TRUE(audit.ok()) like 18 and 21.
            for (const auto& finding : report.findings) {
                if (finding.blocking)
                    EXPECT_EQ(finding.code, "incomplete_catalog") << finding.requirement_id;
            }
            EXPECT_TRUE(audit.missing.empty());
            EXPECT_TRUE(audit.multiply_classified.empty());
        } else {
            EXPECT_TRUE(report.complete());
            EXPECT_TRUE(audit.ok());
        }
    }
}

TEST(CompletenessTest, ExecutableScenarioRegistryHasNoEmptyOrDuplicateIds) {
    for (const unsigned draft : {18u, 21u, 22u}) {
        const auto ids = app::executable_scenarios(draft);
        EXPECT_FALSE(ids.empty());
        std::set<std::string_view> seen;
        for (const auto id : ids) {
            EXPECT_FALSE(id.empty()) << "draft " << draft;
            EXPECT_TRUE(seen.insert(id).second) << "draft " << draft << " duplicate " << id;
        }
    }
}

TEST(CompletenessTest, StaleScenarioRegistrationDoesNotCountAsCoverage) {
    RequirementCatalog catalog{18, "source", true,
        {row("required", Strength::Must)}};
    const std::vector bindings{binding("required")};
    const std::array<std::string_view, 1> scenarios{"renamed-scenario"};
    const auto report = audit_completeness(catalog, bindings, scenarios);
    EXPECT_EQ(report.required_covered, 0u);
    EXPECT_FALSE(report.complete());
    EXPECT_TRUE(std::any_of(report.findings.begin(), report.findings.end(),
        [](const auto& finding) {
            return finding.code == "nonexecutable_scenario";
        }));
}

TEST(CompletenessTest, UnknownEvidenceKindCannotClaimCoverage) {
    RequirementCatalog catalog{18, "source", true,
        {row("required", Strength::Must)}};
    auto invalid = binding("required");
    invalid.evidence_kinds = {"not_a_real_evidence_kind"};
    const std::vector bindings{invalid};
    const auto report = audit_completeness(catalog, bindings, kSyntheticScenarios);
    EXPECT_FALSE(report.complete());
    EXPECT_EQ(report.required_covered, 0u);
    EXPECT_TRUE(std::any_of(report.findings.begin(), report.findings.end(),
        [](const auto& finding) {
            return finding.code == "missing_evidence_schema";
        }));
}

}  // namespace
}  // namespace moq::interop::requirements
