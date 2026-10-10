// Catalog-wide invariants over requirements/moq-lite-06.json, kept green by every hand
// classification task (L1c Tasks 4-6, L2a Task B1). The per-section-group reviewed counts are pinned and
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

std::string section_group(std::string_view section) {
    return std::string(section.substr(0, section.find('.')));
}

TEST(LiteCatalog, LoadsAsACompleteDraft106Catalog) {
    const auto source = lite_source();
    const auto catalog = lite_catalog(source);
    EXPECT_EQ(catalog.draft, 106u);
    EXPECT_TRUE(catalog.complete);  // flipped by L2c; every row is reviewed
    EXPECT_NO_THROW((void)RequirementCatalog::load(source, kRoot / "requirements/moq-lite-06.json",
                                                    CatalogLoadMode::RequireComplete));
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
    const std::regex grammar(R"(^l06-(session|setup|announce|subscribe|group|frame|errors|track|fetch|probe|goaway|datagram)-[a-z0-9-]+$)");
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

// The moq-lite-06 text has no normative keyword before section 1: every row carries a numbered section.
TEST(LiteCatalog, NoFrontMatterRows) {
    const auto catalog = lite_catalog(lite_source());
    const std::regex numbered(R"(^[0-9]+(\.[0-9]+)*$)");
    for (const auto& row : catalog.requirements) {
        EXPECT_TRUE(std::regex_match(row.source.section, numbered)) << row.id << ": " << row.source.section;
    }
}

// L2b classified the last 39 rows: nothing is unreviewed any more. (The catalog stays complete:false until L2c.)
TEST(LiteCatalog, EveryRowIsReviewed) {
    const auto catalog = lite_catalog(lite_source());
    for (const auto& row : catalog.requirements) {
        EXPECT_TRUE(row.reviewed) << row.id << " (section " << row.source.section << ")";
        EXPECT_FALSE(row.rationale.starts_with("Unreviewed:")) << row.id;
    }
}

// Pinned per task: L2b Task B1 classified the last 39 rows (6.1-6.4: 19, 8: 9, 10: 11). Task 4 classified sections 1,
// 3 and 4; Task 5 sections 5.1.1 (with 5.1.1.1 and
// 5.1.1.2), 5.1.2 and 6.3 (with 6.3.1 and 6.3.2); Task 6 the section 7 rows of the L1 scope (63 rows); L2a
// Task B1 the 36 rows of 5.1.3-5.1.6 (13), 7.12 (9), 7.16 (2) and 7.18 (12). 39 rows stay unreviewed for L2b.
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
    EXPECT_EQ(testable, 40u);
    EXPECT_EQ(scenarios.size(), 27u);
    EXPECT_EQ(evaluators.size(), 40u);
    const std::map<std::string, std::size_t> expected{{"1", 11}, {"3", 12}, {"4", 13}, {"5", 42},
                                                      {"6", 28}, {"7", 86}, {"8", 9},  {"10", 11}};
    EXPECT_EQ(reviewed, expected);
    EXPECT_EQ(total, 212u);
    EXPECT_EQ(catalog.requirements.size(), 212u);
}

// The work-list contract: the distinct planned scenario and evaluator ids of the classified catalog, sorted.
// L1d and L2a implement exactly these; a change here is a deliberate change of the work list.
const std::vector<std::string> kPlannedScenarios{
    "l06-announce-lifecycle",
    "l06-announce-prefix",
    "l06-datagram-size",
    "l06-errors-code-space",
    "l06-errors-reserved-reset-code",
    "l06-errors-unknown-reset-code",
    "l06-errors-unknown-stream-type",
    "l06-fetch-group",
    "l06-fetch-unknown-group",
    "l06-goaway-duplicate",
    "l06-goaway-oversize",
    "l06-goaway-single",
    "l06-probe-report",
    "l06-session-stream-close",
    "l06-setup-client-path",
    "l06-setup-duplicate-parameter",
    "l06-setup-duplicate-stream",
    "l06-setup-server-path",
    "l06-setup-server-role",
    "l06-setup-stream",
    "l06-setup-unknown-parameter",
    "l06-subscribe-abutting-frame-start",
    "l06-subscribe-group-floor",
    "l06-subscribe-invalid-frame-bounds",
    "l06-subscribe-latest",
    "l06-subscribe-refused",
    "l06-track-info",
};

const std::vector<std::string> kPlannedEvaluators{
    "l06-announce-hop-list-excludes-own",
    "l06-announce-ok-hop-assigned",
    "l06-announce-ok-then-starts",
    "l06-announce-retired-id-unused",
    "l06-datagram-size-limit",
    "l06-errors-code-space",
    "l06-errors-message-length-close",
    "l06-errors-no-assumed-unauthorized",
    "l06-errors-reserved-code-tolerated",
    "l06-errors-unknown-code-tolerated",
    "l06-errors-unknown-stream-type-not-fatal",
    "l06-errors-unknown-stream-type-reset",
    "l06-fetch-short-run",
    "l06-fetch-unknown-group-reset",
    "l06-goaway-no-new-streams",
    "l06-goaway-oversize-violation",
    "l06-goaway-second-closes",
    "l06-group-sequence-increments",
    "l06-group-starts-with-group",
    "l06-group-unique-sequence",
    "l06-probe-none-reset",
    "l06-probe-target-continues",
    "l06-session-peer-closes-send",
    "l06-setup-duplicate-parameter-close",
    "l06-setup-duplicate-stream-close",
    "l06-setup-parameters-unique",
    "l06-setup-path-absent-on-uri-binding",
    "l06-setup-path-query-appended",
    "l06-setup-path-sent",
    "l06-setup-server-path-close",
    "l06-setup-server-role-close",
    "l06-setup-stream-single-setup",
    "l06-setup-unknown-parameter-ignored",
    "l06-subscribe-invalid-frame-bounds-reset",
    "l06-subscribe-no-group-below-floor",
    "l06-subscribe-ok-group-at-floor",
    "l06-subscribe-refused-reset",
    "l06-subscribe-resolved-start",
    "l06-track-info-immutable",
    "l06-track-info-timescale-nonzero",
};

// The coverage target: every Applicable + Testable required (Must/MustNot) row names at
// least one planned scenario, and each of its scenarios is on the pinned work list (L1d, extended by L2a).
TEST(LiteCatalog, EveryTestableRequiredL1RowNamesAPlannedScenario) {
    const auto catalog = lite_catalog(lite_source());
    const std::set<std::string> work_list(kPlannedScenarios.begin(), kPlannedScenarios.end());
    std::size_t required_testable = 0;
    for (const auto& row : catalog.requirements) {
        if (row.applicability != Applicability::Applicable || row.testability != Testability::Testable) continue;
        if (row.strength != Strength::Must && row.strength != Strength::MustNot) continue;
        ++required_testable;
        EXPECT_FALSE(row.scenarios.empty()) << row.id;
        for (const auto& scenario : row.scenarios) {
            EXPECT_TRUE(work_list.contains(scenario)) << row.id << ": " << scenario << " is not on the work list";
        }
    }
    EXPECT_EQ(required_testable, 36u);
}

// The sorted distinct planned ids derived from the catalog equal the pinned L1d contract.
TEST(LiteCatalog, PlannedScenarioAndEvaluatorListsArePinned) {
    const auto catalog = lite_catalog(lite_source());
    std::set<std::string> scenarios;
    std::set<std::string> evaluators;
    for (const auto& row : catalog.requirements) {
        scenarios.insert(row.scenarios.begin(), row.scenarios.end());
        evaluators.insert(row.evaluators.begin(), row.evaluators.end());
    }
    EXPECT_EQ(std::vector<std::string>(scenarios.begin(), scenarios.end()), kPlannedScenarios);
    EXPECT_EQ(std::vector<std::string>(evaluators.begin(), evaluators.end()), kPlannedEvaluators);
}

}  // namespace
}  // namespace moq::interop::requirements
