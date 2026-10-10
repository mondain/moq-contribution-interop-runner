#include "moq/interop/requirements/lite_baseline.h"

#include "moq/interop/requirements/carry_forward.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace moq::interop::requirements {
namespace {

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

DraftSource lite_source() {
    return load_draft_source(106, kRoot / "docs", kRoot / "requirements/draft-digests.json");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

class LiteBaselineTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("moq-lite-baseline-test-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
    }
    void TearDown() override { std::filesystem::remove_all(directory_); }
    std::filesystem::path directory_;
};

TEST_F(LiteBaselineTest, OneUnreviewedRowPerNormativeOccurrenceWithUniqueGrammarIds) {
    const auto source = lite_source();
    const auto rows = lite_baseline_rows(source);
    const auto occurrences = scan_normative_occurrences(source);
    ASSERT_EQ(rows.size(), occurrences.size());
    const std::regex grammar(R"(^L06-[0-9]+(-[0-9]+)*-(MUST|MUST-NOT|SHOULD|SHOULD-NOT|MAY)-[0-9]{3}$)");
    std::set<std::string> ids;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        EXPECT_TRUE(std::regex_match(row.id, grammar)) << row.id;
        EXPECT_TRUE(ids.insert(row.id).second) << row.id;
        EXPECT_EQ(row.source.first_line, occurrences[i].first_line);
        EXPECT_EQ(row.source.occurrence, occurrences[i].occurrence_on_line);
        EXPECT_EQ(row.source.clause, 1u);
        EXPECT_EQ(row.strength, occurrences[i].normalized_strength);
        EXPECT_FALSE(row.reviewed);
        EXPECT_EQ(row.actor, "endpoint");
        EXPECT_EQ(row.applicability, Applicability::Applicable);
        EXPECT_EQ(row.testability, Testability::NotTestable);
        EXPECT_TRUE(row.scenarios.empty());
        EXPECT_TRUE(row.evaluators.empty());
        EXPECT_EQ(row.rationale, "Unreviewed: classification pending.");
        EXPECT_FALSE(row.source.section.empty()) << row.id;
        EXPECT_LE(row.summary.size(), 160u);
        EXPECT_FALSE(row.summary.empty());
        // The sequence is the 1-based occurrence index over the whole document.
        std::ostringstream suffix;
        suffix << '-' << (i + 1 < 10 ? "00" : i + 1 < 100 ? "0" : "") << (i + 1);
        EXPECT_TRUE(row.id.ends_with(suffix.str())) << row.id;
    }
}

TEST_F(LiteBaselineTest, SectionsFollowTheHeadingStructure) {
    const auto rows = lite_baseline_rows(lite_source());
    const Requirement* stream_type = nullptr;
    const Requirement* unknown_type = nullptr;
    const Requirement* routing = nullptr;
    for (const auto& row : rows) {
        if (row.source.first_line == 1518) stream_type = &row;
        if (row.source.first_line == 1520) unknown_type = &row;
        if (row.source.section == "5.1.1.1" && routing == nullptr) routing = &row;
    }
    ASSERT_NE(stream_type, nullptr);
    EXPECT_EQ(stream_type->source.section, "7.2");
    EXPECT_EQ(stream_type->strength, Strength::Must);
    EXPECT_TRUE(stream_type->id.starts_with("L06-7-2-MUST-")) << stream_type->id;
    ASSERT_NE(unknown_type, nullptr);
    EXPECT_EQ(unknown_type->strength, Strength::MustNot);
    EXPECT_TRUE(unknown_type->id.starts_with("L06-7-2-MUST-NOT-")) << unknown_type->id;
    ASSERT_NE(routing, nullptr);
    EXPECT_TRUE(routing->id.starts_with("L06-5-1-1-1-")) << routing->id;
    EXPECT_GE(routing->source.first_line, 867u);
    EXPECT_LT(routing->source.first_line, 936u);
}

TEST_F(LiteBaselineTest, PageBreakStraddlingSentenceKeepsItsFullRangeAndNoPageFurniture) {
    const auto source = lite_source();
    const auto rows = lite_baseline_rows(source);
    const Requirement* straddle = nullptr;
    for (const auto& row : rows) {
        if (row.source.first_line == 1060) straddle = &row;
    }
    ASSERT_NE(straddle, nullptr);
    EXPECT_EQ(straddle->source.section, "5.1.2");
    EXPECT_GE(straddle->source.last_line, 1069u);
    EXPECT_EQ(straddle->summary.find("Internet-Draft"), std::string::npos) << straddle->summary;
    EXPECT_EQ(straddle->summary.find("[Page"), std::string::npos) << straddle->summary;
    bool saw_straddle = false;
    for (const auto& context : extract_contexts(source)) {
        EXPECT_EQ(context.sentence.find("Internet-Draft"), std::string::npos) << context.first_line;
        EXPECT_EQ(context.sentence.find("[Page"), std::string::npos) << context.first_line;
        if (context.first_line == 1060) {
            saw_straddle = true;
            EXPECT_NE(context.sentence.find("stragglers within the range"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_straddle);
}

TEST_F(LiteBaselineTest, NormalizesRequiredAndRecommendedStrength) {
    const auto source = lite_source();
    const auto occurrences = scan_normative_occurrences(source);
    const auto rows = lite_baseline_rows(source);
    for (std::size_t i = 0; i < occurrences.size(); ++i) {
        if (occurrences[i].phrase == "REQUIRED") EXPECT_EQ(rows[i].strength, Strength::Must);
        if (occurrences[i].phrase == "RECOMMENDED") EXPECT_EQ(rows[i].strength, Strength::Should);
    }
}

TEST_F(LiteBaselineTest, CommittedCatalogEqualsGeneratorOutputByteForByte) {
    // Hand classification (L1c Tasks 4-6) replaces rows in place; every row still marked
    // reviewed:false must be the generator's row, and putting the generator's rows back in place
    // of the reviewed ones must reproduce the generator output byte for byte (same order, ids,
    // sources, strengths and formatting).
    const auto source = lite_source();
    write_lite_baseline(source, LiteBaselineOptions{directory_, false});
    const auto committed = kRoot / "requirements/moq-lite-06.json";
    ASSERT_TRUE(std::filesystem::exists(committed));
    const auto generated_text = read_file(directory_ / "moq-lite-06.json");
    const auto generated = nlohmann::ordered_json::parse(generated_text);
    const auto committed_text = read_file(committed);
    auto document = nlohmann::ordered_json::parse(committed_text);
    // Raw formatting check: classification edits keep the generator's serialization.
    EXPECT_EQ(committed_text, document.dump(2) + "\n");
    ASSERT_EQ(document["requirements"].size(), generated["requirements"].size());
    // The one deliberate difference from the generator since L2c: every row is reviewed, so the catalog is complete.
    EXPECT_TRUE(document["complete"].get<bool>());
    EXPECT_FALSE(generated["complete"].get<bool>());
    document["complete"] = generated["complete"];
    for (std::size_t i = 0; i < generated["requirements"].size(); ++i) {
        auto& row = document["requirements"][i];
        const auto& baseline = generated["requirements"][i];
        EXPECT_EQ(row["id"], baseline["id"]);
        EXPECT_EQ(row["strength"], baseline["strength"]) << row["id"];
        EXPECT_EQ(row["source"], baseline["source"]) << row["id"];
        if (row.contains("reviewed") && row["reviewed"] == false) {
            EXPECT_EQ(row, baseline) << row["id"];
        } else {
            row = baseline;
        }
    }
    EXPECT_EQ(document.dump(2) + "\n", generated_text);
    const auto catalog = RequirementCatalog::load(source, committed, CatalogLoadMode::AllowIncomplete);
    EXPECT_TRUE(catalog.complete);
    EXPECT_EQ(catalog.draft, 106u);
    EXPECT_EQ(catalog.source_sha256, source.sha256);
    EXPECT_EQ(catalog.requirements.size(), scan_normative_occurrences(source).size());
}

TEST_F(LiteBaselineTest, RegeneratingOverAReviewedRowThrowsUnlessForced) {
    const auto source = lite_source();
    write_lite_baseline(source, LiteBaselineOptions{directory_, false});
    const auto path = directory_ / "moq-lite-06.json";
    EXPECT_NO_THROW(write_lite_baseline(source, LiteBaselineOptions{directory_, false}));

    auto document = nlohmann::ordered_json::parse(read_file(path));
    document["requirements"][0]["reviewed"] = true;
    write_file(path, document.dump(2) + "\n");
    const auto before = read_file(path);
    EXPECT_THROW(write_lite_baseline(source, LiteBaselineOptions{directory_, false}),
                 std::runtime_error);
    EXPECT_EQ(read_file(path), before);

    // A row without the flag counts as reviewed.
    document["requirements"][0].erase("reviewed");
    write_file(path, document.dump(2) + "\n");
    EXPECT_THROW(write_lite_baseline(source, LiteBaselineOptions{directory_, false}),
                 std::runtime_error);

    EXPECT_NO_THROW(write_lite_baseline(source, LiteBaselineOptions{directory_, true}));
    const auto fresh = directory_ / "fresh";
    std::filesystem::create_directories(fresh);
    write_lite_baseline(source, LiteBaselineOptions{fresh, false});
    EXPECT_EQ(read_file(path), read_file(fresh / "moq-lite-06.json"));
}

TEST_F(LiteBaselineTest, StagedAuditPassesTheBaselineAndFailsWhenARowIsRemoved) {
    const auto source = lite_source();
    auto catalog = RequirementCatalog::load(source, kRoot / "requirements/moq-lite-06.json",
                                            CatalogLoadMode::AllowIncomplete);
    const auto staged = audit_normative_occurrences_staged(source, catalog);
    EXPECT_TRUE(staged.ok());
    EXPECT_TRUE(staged.missing.empty());
    EXPECT_TRUE(staged.multiply_classified.empty());
    EXPECT_TRUE(staged.errors.empty());
    // Complete since L2c: the complete audit accepts the catalog too (it refused the staged one).
    EXPECT_TRUE(catalog.complete);
    EXPECT_TRUE(audit_normative_occurrences(source, catalog).ok());

    catalog.requirements.erase(catalog.requirements.begin() + 5);
    const auto broken = audit_normative_occurrences_staged(source, catalog);
    EXPECT_FALSE(broken.ok());
    EXPECT_EQ(broken.missing.size(), 1u);
    EXPECT_FALSE(audit_normative_occurrences(source, catalog).ok());
}

}  // namespace
}  // namespace moq::interop::requirements
