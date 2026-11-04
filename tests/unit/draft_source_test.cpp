#include "moq/interop/requirements/draft_source.h"

#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/carry_forward.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

namespace moq::interop::requirements {
namespace {

const std::filesystem::path kProjectRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

class DraftSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("moq-interop-draft-source-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(temp_dir_); }

    std::filesystem::path temp_dir_;
};

TEST_F(DraftSourceTest, LoadsDraft18WithVerifiedDigestAndOneBasedLines) {
    const auto draft = load_draft_source(18, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");

    EXPECT_EQ(draft.number, 18u);
    EXPECT_EQ(draft.path, kProjectRoot / "docs/draft-ietf-moq-transport-18.txt");
    EXPECT_EQ(draft.sha256, "9e6b32cb7797c151e9e127374c1291af3ed546b2d453cd5bbb15946977eeeeb6");
    EXPECT_EQ(draft.text.size(), 314260u);
    ASSERT_FALSE(draft.line_offsets.empty());
    EXPECT_EQ(draft.line_offsets.front(), 0u);
    const auto last_line = draft.line_offsets.size();
    EXPECT_EQ(draft.lines(1, 1), "\n");
    EXPECT_EQ(draft.lines(16, 16), "                      draft-ietf-moq-transport-18\n");
    EXPECT_EQ(draft.lines(17, 18), "\nAbstract\n");
    EXPECT_EQ(draft.lines(last_line, last_line),
              "Nandakumar, et al.      Expires 13 November 2026              [Page 140]\n");
    EXPECT_THROW(draft.lines(0, 1), std::out_of_range);
    EXPECT_THROW(draft.lines(18, 17), std::out_of_range);
    EXPECT_THROW(draft.lines(last_line + 1, last_line + 1), std::out_of_range);
}

TEST_F(DraftSourceTest, LoadsDraft21WithVerifiedDigestAndOneBasedLines) {
    const auto draft = load_draft_source(21, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");

    EXPECT_EQ(draft.number, 21u);
    EXPECT_EQ(draft.path, kProjectRoot / "docs/draft-ietf-moq-transport-21.txt");
    EXPECT_EQ(draft.sha256, "8c1d80849f46026b634fa19f8607ce983be2b1db9ae4ee1990878969c7b7e142");
    EXPECT_EQ(draft.text.size(), 358078u);
    ASSERT_FALSE(draft.line_offsets.empty());
    const auto last_line = draft.line_offsets.size();
    EXPECT_EQ(draft.lines(1, 1), "\n");
    EXPECT_EQ(draft.lines(16, 16), "                      draft-ietf-moq-transport-21\n");
    EXPECT_EQ(draft.lines(17, 18), "\nAbstract\n");
    EXPECT_EQ(draft.lines(last_line, last_line),
              "Nandakumar, et al.        Expires 12 March 2027               [Page 159]\n");
    EXPECT_THROW(draft.lines(last_line + 1, last_line + 1), std::out_of_range);
}

TEST_F(DraftSourceTest, ReportsExpectedAndActualDigestForOneByteChange) {
    const auto altered = temp_dir_ / "draft-ietf-moq-transport-18.txt";
    std::filesystem::copy_file(kProjectRoot / "docs/draft-ietf-moq-transport-18.txt", altered);
    {
        std::fstream file(altered, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.put('X');
        ASSERT_TRUE(file.good());
    }

    try {
        load_draft_source(18, temp_dir_, kProjectRoot / "requirements/draft-digests.json");
        FAIL() << "Expected a digest mismatch";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("18"), std::string::npos);
        EXPECT_NE(message.find("9e6b32cb7797c151e9e127374c1291af3ed546b2d453cd5bbb15946977eeeeb6"),
                  std::string::npos);
        EXPECT_NE(message.find("8f9074999c885f0fed2e8352bcb9c3c9e884db6a43a33b99fdbf69606522a3cc"),
                  std::string::npos);
    }
}

TEST_F(DraftSourceTest, LoadsDraft22WithVerifiedDigest) {
    const auto draft = load_draft_source(22, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");

    EXPECT_EQ(draft.number, 22u);
    EXPECT_EQ(draft.path, kProjectRoot / "docs/draft-ietf-moq-transport-22.txt");
    EXPECT_EQ(draft.line_offsets.size(), 9240u);
    EXPECT_EQ(draft.sha256.size(), 64u);
}

std::size_t independent_line_of(const std::string& text, std::size_t offset) {
    std::size_t newlines = 0;
    for (std::size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') ++newlines;
    }
    return newlines + 1;
}

TEST_F(DraftSourceTest, NamesTheDraftTextFileForEachSupportedDraft) {
    EXPECT_EQ(draft_source_filename(18), "draft-ietf-moq-transport-18.txt");
    EXPECT_EQ(draft_source_filename(21), "draft-ietf-moq-transport-21.txt");
    EXPECT_EQ(draft_source_filename(22), "draft-ietf-moq-transport-22.txt");
    EXPECT_EQ(draft_source_filename(23), "draft-ietf-moq-transport-23.txt");
    EXPECT_EQ(draft_source_filename(106), "draft-lcurley-moq-lite-06.txt");
    EXPECT_THROW(draft_source_filename(105), std::invalid_argument);
}

TEST_F(DraftSourceTest, LoadsDraft23WithVerifiedDigestAndCompleteOccurrenceBaseline) {
    const auto draft = load_draft_source(23, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");
    EXPECT_EQ(draft.number, 23u);
    EXPECT_EQ(draft.sha256, "d02aa279326b524113cbb622571f241e54b28cfcd3421f97d38ad13b0eed0fe7");
    EXPECT_EQ(draft.line_offsets.size(), 9856u);
    const auto catalog = RequirementCatalog::load(draft, kProjectRoot / "requirements/draft23.json",
                                                   CatalogLoadMode::AllowIncomplete);
    EXPECT_FALSE(catalog.complete);
    EXPECT_EQ(catalog.requirements.size(), 532u);
    EXPECT_TRUE(audit_normative_occurrences_staged(draft, catalog).ok());
    EXPECT_THROW(RequirementCatalog::load(draft, kProjectRoot / "requirements/draft23.json"),
                 std::runtime_error);
    for (const auto& row : catalog.requirements) {
        EXPECT_TRUE(row.id.starts_with("D23-"));
        if (!row.reviewed) {
            EXPECT_TRUE(row.scenarios.empty());
            EXPECT_TRUE(row.evaluators.empty());
        }
    }
}

TEST_F(DraftSourceTest, Draft23DeltaAccountsForEveryOldRowAndNewOccurrenceWithoutClaimingReuse) {
    std::ifstream input(kProjectRoot / "requirements/draft22-to-23-delta.json");
    const auto delta = nlohmann::json::parse(input);
    const auto old_source = load_draft_source(22, kProjectRoot / "docs",
                                              kProjectRoot / "requirements/draft-digests.json");
    const auto new_source = load_draft_source(23, kProjectRoot / "docs",
                                              kProjectRoot / "requirements/draft-digests.json");
    const auto old_catalog = RequirementCatalog::load(old_source, kProjectRoot / "requirements/draft22.json");
    const auto new_catalog = RequirementCatalog::load(new_source, kProjectRoot / "requirements/draft23.json",
                                                       CatalogLoadMode::AllowIncomplete);
    EXPECT_EQ(delta.at("from_sha256"), old_source.sha256);
    EXPECT_EQ(delta.at("to_sha256"), new_source.sha256);
    EXPECT_EQ(delta.at("scenario_reuse_policy").at("default"), "disabled");
    EXPECT_EQ(delta.at("normative_changes").size(), 35u);
    for (const auto& change : delta.at("normative_changes")) {
        const auto first = change.at("first_line").get<std::size_t>();
        const auto last = change.at("last_line").get<std::size_t>();
        auto cited = normalize_text(new_source.lines(first, last));
        ASSERT_TRUE(cited.starts_with("* ")) << first << ": " << cited;
        cited.erase(0, 2);
        EXPECT_EQ(cited, change.at("summary").get<std::string>()) << first;
    }

    std::set<std::string> seen_old, seen_new;
    for (const auto& entry : delta.at("entries")) {
        EXPECT_FALSE(entry.at("reviewed").get<bool>());
        for (const auto& old_id : entry.at("draft22_ids")) seen_old.insert(old_id.get<std::string>());
        const auto new_id = entry.at("draft23_id").get<std::string>();
        if (!new_id.empty()) EXPECT_TRUE(seen_new.insert(new_id).second);
    }
    EXPECT_EQ(seen_old.size(), old_catalog.requirements.size());
    EXPECT_EQ(seen_new.size(), new_catalog.requirements.size());
    for (const auto& row : old_catalog.requirements) EXPECT_TRUE(seen_old.contains(row.id)) << row.id;
    for (const auto& row : new_catalog.requirements) EXPECT_TRUE(seen_new.contains(row.id)) << row.id;
}

TEST_F(DraftSourceTest, LoadsMoqLite06AsDraft106) {
    const auto draft = load_draft_source(106, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");

    EXPECT_EQ(draft.number, 106u);
    EXPECT_EQ(draft.path, kProjectRoot / "docs/draft-lcurley-moq-lite-06.txt");
    EXPECT_EQ(draft.sha256.size(), 64u);
    EXPECT_EQ(draft.sha256.find_first_not_of("0123456789abcdef"), std::string::npos);

    std::size_t newline_count = 0;
    for (const char c : draft.text) {
        if (c == '\n') ++newline_count;
    }
    ASSERT_FALSE(draft.text.empty());
    ASSERT_EQ(draft.text.back(), '\n');
    EXPECT_EQ(draft.line_offsets.size(), newline_count);
    EXPECT_EQ(draft.lines(1, 1), "\n");
    const auto first_three = draft.lines(1, 3);
    EXPECT_EQ(std::count(first_three.begin(), first_three.end(), '\n'), 3);
    // A form feed is not a line break: the page-break line holds only the form feed.
    EXPECT_EQ(draft.lines(57, 57), "\f\n");
    EXPECT_EQ(draft.lines(58, 58).substr(0, 14), "Internet-Draft");
}

TEST_F(DraftSourceTest, ReportsMoqLiteDigestMismatchAndMissingKey) {
    const auto wrong = temp_dir_ / "wrong.json";
    {
        std::ofstream file(wrong);
        file << R"({"106": ")" << std::string(64, '0') << R"("})";
    }
    try {
        load_draft_source(106, kProjectRoot / "docs", wrong);
        FAIL() << "Expected a digest mismatch";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("SHA-256 mismatch"), std::string::npos);
    }

    const auto missing = temp_dir_ / "missing.json";
    {
        std::ofstream file(missing);
        file << R"({"18": ")" << std::string(64, '0') << R"("})";
    }
    try {
        load_draft_source(106, kProjectRoot / "docs", missing);
        FAIL() << "Expected a missing digest";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("No SHA-256 digest for draft 106"),
                  std::string::npos);
    }
}

TEST_F(DraftSourceTest, ScansMoqLiteOccurrencesWithCorrectLinesAroundFormFeeds) {
    const auto draft = load_draft_source(106, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");
    const auto occurrences = scan_normative_occurrences(draft);
    ASSERT_FALSE(occurrences.empty());

    // Every reported line really contains the phrase's first word.
    for (const auto& occurrence : occurrences) {
        const auto first_word = occurrence.phrase.substr(0, occurrence.phrase.find(' '));
        EXPECT_NE(draft.lines(occurrence.first_line, occurrence.first_line).find(first_word),
                  std::string_view::npos)
            << "line " << occurrence.first_line;
    }

    // Find, independently of the scanner, the first " MUST " on a page after at least
    // one earlier form feed, and derive its line by counting newlines.
    std::size_t form_feeds_seen = 0;
    std::size_t target = std::string::npos;
    for (std::size_t i = 0; i < draft.text.size() && target == std::string::npos; ++i) {
        if (draft.text[i] != '\f') continue;
        ++form_feeds_seen;
        if (form_feeds_seen < 2) continue;
        const auto next_ff = draft.text.find('\f', i + 1);
        const auto hit = draft.text.find(" MUST ", i);
        if (hit != std::string::npos && (next_ff == std::string::npos || hit < next_ff)) {
            target = hit + 1;
        }
    }
    ASSERT_NE(target, std::string::npos);
    const auto expected_line = independent_line_of(draft.text, target);
    EXPECT_GT(expected_line, 57u);

    bool found = false;
    for (const auto& occurrence : occurrences) {
        if (occurrence.phrase == "MUST" && occurrence.first_line == expected_line) found = true;
    }
    EXPECT_TRUE(found) << "expected MUST on line " << expected_line;
}

TEST_F(DraftSourceTest, RejectsDraftLargerThanTheBound) {
    const auto oversized = temp_dir_ / "draft-ietf-moq-transport-18.txt";
    {
        std::ofstream file(oversized, std::ios::binary);
        file << std::string(1024 * 1024 + 1, 'X');
        ASSERT_TRUE(file.good());
    }

    try {
        load_draft_source(18, temp_dir_, kProjectRoot / "requirements/draft-digests.json");
        FAIL() << "Expected the draft size limit to reject the file";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("size limit"), std::string::npos);
    }
}

}  // namespace
}  // namespace moq::interop::requirements
