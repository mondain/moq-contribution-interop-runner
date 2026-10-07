// Catalog-wide invariants over requirements/moq-lite-06.json, kept green by every hand
// classification task (L1c Tasks 4-6). The per-section-group reviewed counts are pinned and
// updated by each task as it classifies its rows.
#include "moq/interop/requirements/catalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::requirements {
namespace {

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

DraftSource lite_source() {
    return load_draft_source(106, kRoot / "docs", kRoot / "requirements/draft-digests.json");
}

RequirementCatalog lite_catalog(const DraftSource& source) {
    return RequirementCatalog::load(source, kRoot / "requirements/moq-lite-06.json",
                                    CatalogLoadMode::AllowIncomplete);
}

// True when `section` is `parent` or nested under it ("7.3" covers "7.3" and "7.3.2", not "7.30").
bool within(std::string_view section, std::string_view parent) {
    return section == parent ||
           (section.size() > parent.size() && section.starts_with(parent) && section[parent.size()] == '.');
}

// The L1 scope of the plan: sections 3, 4 (with 4.4), 5.1.1 (Announce), 5.1.2 (Subscribe), 6.3
// (Setup and Group streams), 7.1-7.10 (with the Setup parameters 7.3.x), 7.13-7.15, 7.19 and 7.20.
bool in_l1_scope(std::string_view section) {
    for (const std::string_view parent : {"3", "4", "5.1.1", "5.1.2", "6.3", "7.1", "7.2", "7.3", "7.4", "7.5",
                                          "7.6", "7.7", "7.8", "7.9", "7.10", "7.13", "7.14", "7.15", "7.19",
                                          "7.20"}) {
        if (within(section, parent)) return true;
    }
    return false;
}

// Outside the L1 scope only vocabulary, changelog and security text may be classified, and only as
// Informative or NotApplicable (never as a publisher obligation).
bool may_hold_non_scope_review(std::string_view section) {
    return within(section, "1") || within(section, "8") || within(section, "10");
}

std::string section_group(std::string_view section) {
    return std::string(section.substr(0, section.find('.')));
}

TEST(LiteCatalog, LoadsAsAnIncompleteDraft106Catalog) {
    const auto source = lite_source();
    const auto catalog = lite_catalog(source);
    EXPECT_EQ(catalog.draft, 106u);
    EXPECT_FALSE(catalog.complete);
    EXPECT_EQ(catalog.source_sha256, source.sha256);
    EXPECT_EQ(catalog.requirements.size(), scan_normative_occurrences(source).size());
}

TEST(LiteCatalog, StagedNormativeAuditIsOk) {
    const auto source = lite_source();
    const auto catalog = lite_catalog(source);
    const auto audit = audit_normative_occurrences_staged(source, catalog);
    EXPECT_TRUE(audit.ok());
    EXPECT_TRUE(audit.missing.empty());
    EXPECT_TRUE(audit.multiply_classified.empty());
    EXPECT_TRUE(audit.errors.empty());
}

TEST(LiteCatalog, ReviewedRowsObeyTheSchemaConditionals) {
    const auto catalog = lite_catalog(lite_source());
    for (const auto& row : catalog.requirements) {
        if (!row.reviewed) continue;
        EXPECT_FALSE(row.actor.empty()) << row.id;
        EXPECT_FALSE(row.summary.empty()) << row.id;
        EXPECT_FALSE(row.rationale.empty()) << row.id;
        EXPECT_FALSE(row.rationale.starts_with("Unreviewed:")) << row.id;
        if (row.applicability == Applicability::Applicable) {
            EXPECT_NE(row.testability, Testability::NotApplicable) << row.id;
            if (row.testability == Testability::Testable) {
                EXPECT_FALSE(row.scenarios.empty()) << row.id;
                EXPECT_FALSE(row.evaluators.empty()) << row.id;
            } else {
                EXPECT_TRUE(row.scenarios.empty()) << row.id;
                EXPECT_TRUE(row.evaluators.empty()) << row.id;
            }
        } else {
            EXPECT_EQ(row.testability, Testability::NotApplicable) << row.id;
            EXPECT_TRUE(row.scenarios.empty()) << row.id;
            EXPECT_TRUE(row.evaluators.empty()) << row.id;
        }
    }
}

TEST(LiteCatalog, PlannedIdsFollowTheGrammarAndAreUniquePerRow) {
    const auto catalog = lite_catalog(lite_source());
    const std::regex grammar(R"(^l06-(session|setup|announce|subscribe|group|frame|errors)-[a-z0-9-]+$)");
    for (const auto& row : catalog.requirements) {
        std::set<std::string> scenarios;
        for (const auto& id : row.scenarios) {
            EXPECT_TRUE(std::regex_match(id, grammar)) << row.id << ": " << id;
            EXPECT_TRUE(scenarios.insert(id).second) << row.id << ": duplicate " << id;
        }
        std::set<std::string> evaluators;
        for (const auto& id : row.evaluators) {
            EXPECT_TRUE(std::regex_match(id, grammar)) << row.id << ": " << id;
            EXPECT_TRUE(evaluators.insert(id).second) << row.id << ": duplicate " << id;
        }
    }
}

// A planned evaluator judges one fixed set of scenarios: every row naming an evaluator names the
// same scenario list with it (a scenario may still feed several evaluators).
TEST(LiteCatalog, EachEvaluatorPairsWithOneScenarioSet) {
    const auto catalog = lite_catalog(lite_source());
    std::map<std::string, std::vector<std::string>> scenarios_of;
    for (const auto& row : catalog.requirements) {
        for (const auto& evaluator : row.evaluators) {
            const auto [it, inserted] = scenarios_of.emplace(evaluator, row.scenarios);
            if (!inserted) EXPECT_EQ(it->second, row.scenarios) << row.id << ": " << evaluator;
        }
    }
}

// The plan lets front-matter rows be classified Informative/NotApplicable, but the moq-lite-06
// text has no normative keyword before section 1: every row carries a numbered section. If a
// later draft text adds front-matter rows, extend may_hold_non_scope_review deliberately.
TEST(LiteCatalog, NoFrontMatterRows) {
    const auto catalog = lite_catalog(lite_source());
    const std::regex numbered(R"(^[0-9]+(\.[0-9]+)*$)");
    for (const auto& row : catalog.requirements) {
        EXPECT_TRUE(std::regex_match(row.source.section, numbered)) << row.id << ": " << row.source.section;
    }
}

TEST(LiteCatalog, UnreviewedRowsKeepThePlaceholderShape) {
    const auto catalog = lite_catalog(lite_source());
    for (const auto& row : catalog.requirements) {
        if (row.reviewed) continue;
        EXPECT_TRUE(row.scenarios.empty()) << row.id;
        EXPECT_TRUE(row.evaluators.empty()) << row.id;
        EXPECT_EQ(row.applicability, Applicability::Applicable) << row.id;
        EXPECT_EQ(row.testability, Testability::NotTestable) << row.id;
        EXPECT_TRUE(row.rationale.starts_with("Unreviewed:")) << row.id;
    }
}

TEST(LiteCatalog, RowsOutsideTheL1ScopeStayUnreviewed) {
    const auto catalog = lite_catalog(lite_source());
    for (const auto& row : catalog.requirements) {
        if (!row.reviewed || in_l1_scope(row.source.section)) continue;
        ASSERT_TRUE(may_hold_non_scope_review(row.source.section))
            << row.id << " (section " << row.source.section << ") is outside the L1 scope but reviewed";
        EXPECT_NE(row.applicability, Applicability::Applicable)
            << row.id << " outside the L1 scope may only be Informative or NotApplicable";
    }
}

TEST(LiteCatalog, ScopeHelperSeparatesNeighbouringSections) {
    EXPECT_TRUE(in_l1_scope("7.1"));
    EXPECT_TRUE(in_l1_scope("7.10"));
    EXPECT_FALSE(in_l1_scope("7.11"));
    EXPECT_FALSE(in_l1_scope("7.12"));
    EXPECT_TRUE(in_l1_scope("7.3.5"));
    EXPECT_TRUE(in_l1_scope("5.1.1.2"));
    EXPECT_FALSE(in_l1_scope("5.1.3"));
    EXPECT_TRUE(in_l1_scope("6.3.2"));
    EXPECT_FALSE(in_l1_scope("6.4"));
    EXPECT_TRUE(in_l1_scope("4.4.2"));
    EXPECT_FALSE(in_l1_scope("1"));
    EXPECT_FALSE(in_l1_scope("10.1"));
    EXPECT_TRUE(may_hold_non_scope_review("10.1"));
    EXPECT_FALSE(may_hold_non_scope_review("9.1"));
}

// Sections whose hand classification is finished: every row in them is reviewed (Task 4: 1, 3, 4;
// Task 5: 5.1.1 with 5.1.1.1 and 5.1.1.2, 5.1.2, 6.3 with 6.3.1 and 6.3.2). Later tasks extend the list.
TEST(LiteCatalog, ClassifiedSectionsHaveNoUnreviewedRow) {
    const auto catalog = lite_catalog(lite_source());
    for (const auto& row : catalog.requirements) {
        bool classified = false;
        for (const std::string_view parent : {"1", "3", "4", "5.1.1", "5.1.2", "6.3"}) {
            if (within(row.source.section, parent)) classified = true;
        }
        if (classified) EXPECT_TRUE(row.reviewed) << row.id << " (section " << row.source.section << ")";
    }
}

// Pinned per task: Task 4 classified sections 1, 3 and 4; Task 5 sections 5.1.1 (with 5.1.1.1 and
// 5.1.1.2), 5.1.2 and 6.3 (with 6.3.1 and 6.3.2).
TEST(LiteCatalog, ReviewedRowCountPerSectionGroupIsPinned) {
    const auto catalog = lite_catalog(lite_source());
    std::map<std::string, std::size_t> reviewed;
    std::size_t total = 0;
    std::size_t testable = 0;
    std::set<std::string> scenarios;
    std::set<std::string> evaluators;
    for (const auto& row : catalog.requirements) {
        if (!row.reviewed) continue;
        ++reviewed[section_group(row.source.section)];
        ++total;
        if (row.testability == Testability::Testable) ++testable;
        scenarios.insert(row.scenarios.begin(), row.scenarios.end());
        evaluators.insert(row.evaluators.begin(), row.evaluators.end());
    }
    EXPECT_EQ(testable, 12u);
    EXPECT_EQ(scenarios.size(), 10u);
    EXPECT_EQ(evaluators.size(), 12u);
    const std::map<std::string, std::size_t> expected{{"1", 11}, {"3", 12}, {"4", 13}, {"5", 29}, {"6", 9}};
    EXPECT_EQ(reviewed, expected);
    EXPECT_EQ(total, 74u);
    EXPECT_EQ(catalog.requirements.size(), 212u);
}

}  // namespace
}  // namespace moq::interop::requirements
