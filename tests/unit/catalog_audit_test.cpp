#include "moq/interop/requirements/catalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace moq::interop::requirements {
namespace {

DraftSource source_from_text(std::string text) {
    DraftSource source{18, {}, "synthetic", std::move(text), {0}};
    for (std::size_t i = 0; i + 1 < source.text.size(); ++i) {
        if (source.text[i] == '\n') {
            source.line_offsets.push_back(i + 1);
        }
    }
    return source;
}

Requirement requirement(std::string id, Strength strength, std::size_t line,
                        unsigned occurrence, unsigned clause = 1) {
    return {std::move(id), strength, {"1", line, line, occurrence, clause}, "endpoint",
            "Observable behavior", Applicability::Applicable, Testability::Testable,
            {"scenario"}, {"evaluator"}, "Synthetic test obligation"};
}

TEST(CatalogAuditTest, ScansWrappedNegationAndSameLineOccurrencesWithOrdinalAnchors) {
    const auto source = source_from_text("Endpoint MUST\n NOT send. SHOULD NOT forward; MAY retry.\n");
    const auto found = scan_normative_occurrences(source);
    ASSERT_EQ(found.size(), 3u);
    EXPECT_EQ(found[0].phrase, "MUST NOT");
    EXPECT_EQ(found[0].normalized_strength, Strength::MustNot);
    EXPECT_EQ(found[0].first_line, 1u);
    EXPECT_EQ(found[1].phrase, "SHOULD NOT");
    EXPECT_EQ(found[1].first_line, 2u);
    EXPECT_EQ(found[1].occurrence_on_line, 1u);
    EXPECT_EQ(found[2].normalized_strength, Strength::May);
    EXPECT_EQ(found[2].occurrence_on_line, 2u);
}

TEST(CatalogAuditTest, NormalizesAllSynonymsWithoutLosingSourcePhrase) {
    const auto source = source_from_text(
        "MUST NOT SHALL NOT SHOULD NOT NOT RECOMMENDED MUST SHALL SHOULD RECOMMENDED "
        "REQUIRED OPTIONAL MAY\n");
    const auto found = scan_normative_occurrences(source);
    ASSERT_EQ(found.size(), 11u);
    const std::vector<Strength> expected = {
        Strength::MustNot, Strength::MustNot, Strength::ShouldNot, Strength::ShouldNot,
        Strength::Must, Strength::Must, Strength::Should, Strength::Should,
        Strength::Must, Strength::May, Strength::May};
    for (std::size_t i = 0; i < found.size(); ++i) {
        EXPECT_EQ(found[i].normalized_strength, expected[i]) << i;
        EXPECT_EQ(found[i].occurrence_on_line, i + 1) << i;
    }
    EXPECT_EQ(found[3].phrase, "NOT RECOMMENDED");
}

TEST(CatalogAuditTest, IncludesQuotedVocabularyAndHistoricalText) {
    const auto source = source_from_text(
        "The key words \"MUST\", \"MUST NOT\" in this document are BCP 14 terms.\n"
        "Appendix: * MUST for historical relay behavior.\n");
    const auto found = scan_normative_occurrences(source);
    ASSERT_EQ(found.size(), 3u);
    EXPECT_TRUE(found[0].quoted_bcp14_vocabulary);
    EXPECT_TRUE(found[1].quoted_bcp14_vocabulary);
    EXPECT_FALSE(found[2].quoted_bcp14_vocabulary);
    EXPECT_EQ(found[2].first_line, 2u);
}

TEST(CatalogAuditTest, ReportsMissingOccurrenceAndAllowsDistinctClauses) {
    const auto source = source_from_text("Endpoint MUST send and log; MAY retry.\n");
    RequirementCatalog catalog{18, "synthetic", true,
                               {requirement("send", Strength::Must, 1, 1),
                                requirement("log", Strength::Must, 1, 1, 2)}};
    auto report = audit_normative_occurrences(source, catalog);
    ASSERT_EQ(report.missing.size(), 1u);
    EXPECT_EQ(report.missing[0].phrase, "MAY");
    EXPECT_FALSE(report.ok());
    catalog.requirements.push_back(requirement("retry", Strength::May, 1, 2));
    EXPECT_TRUE(audit_normative_occurrences(source, catalog).ok());
}

TEST(CatalogAuditTest, ReportsConflictingOrDuplicateCoverage) {
    const auto source = source_from_text("Endpoint MUST send and log.\n");
    RequirementCatalog catalog{18, "synthetic", true,
                               {requirement("send", Strength::Must, 1, 1),
                                requirement("wrong-strength", Strength::Should, 1, 1, 2)}};
    auto report = audit_normative_occurrences(source, catalog);
    ASSERT_EQ(report.multiply_classified.size(), 1u);
    EXPECT_FALSE(report.ok());
    catalog.requirements[1].strength = Strength::Must;
    catalog.requirements[1].source.clause = 1;
    report = audit_normative_occurrences(source, catalog);
    EXPECT_EQ(report.multiply_classified.size(), 1u);
}

TEST(CatalogAuditTest, ReportsRecordsWithNoSourceOccurrence) {
    const auto source = source_from_text("Endpoint MUST send.\nOther text.\n");
    RequirementCatalog catalog{18, "synthetic", true,
                               {requirement("send", Strength::Must, 1, 1),
                                requirement("invented", Strength::May, 2, 1)}};
    const auto report = audit_normative_occurrences(source, catalog);
    EXPECT_FALSE(report.ok());
    EXPECT_FALSE(report.errors.empty());
}

TEST(CatalogAuditTest, IncompleteCatalogCannotPassAuditEvenWithEveryAnchorPresent) {
    const auto source = source_from_text("Endpoint MUST send.\n");
    RequirementCatalog catalog{18, "synthetic", false,
                               {requirement("send", Strength::Must, 1, 1)}};
    const auto report = audit_normative_occurrences(source, catalog);
    EXPECT_FALSE(report.ok());
    EXPECT_FALSE(report.errors.empty());
}

TEST(CatalogAuditTest, RealDraftOccurrenceCountsStayPinned) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    for (const auto& [number, count] : std::vector<std::pair<unsigned, std::size_t>>{
             {18, 471}, {21, 507}}) {
        const auto source = load_draft_source(number, root / "docs",
                                               root / "requirements/draft-digests.json");
        EXPECT_EQ(scan_normative_occurrences(source).size(), count) << number;
    }
}

}  // namespace
}  // namespace moq::interop::requirements
