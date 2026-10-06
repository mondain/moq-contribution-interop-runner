#include "../support/catalog_partition.h"
#include "moq/interop/requirements/carry_forward.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace moq::interop::requirements {
namespace {

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

struct PartitionFile {
    std::string name;
    std::size_t first;
    std::size_t last;
};

std::vector<PartitionFile> partitions() {
    static const std::regex pattern(R"(draft22-lines-([0-9]{4})-([0-9]{4})\.json)");
    std::vector<PartitionFile> result;
    for (const auto& entry : std::filesystem::directory_iterator(kRoot / "requirements/parts")) {
        const auto name = entry.path().filename().string();
        std::smatch match;
        if (std::regex_match(name, match, pattern)) {
            result.push_back({name, std::stoul(match[1]), std::stoul(match[2])});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return result;
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return nlohmann::json::parse(input);
}

class Draft22CatalogTest : public test::CatalogPartitionTest {};

TEST_F(Draft22CatalogTest, PartitionsTileTheWholeDraftAndCoverEveryOccurrence) {
    const auto files = partitions();
    ASSERT_FALSE(files.empty());
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    EXPECT_EQ(files.front().first, 1u);
    EXPECT_EQ(files.back().last, source.line_offsets.size());
    const auto occurrences = scan_normative_occurrences(source);
    std::size_t covered = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i > 0) {
            EXPECT_EQ(files[i].first, files[i - 1].last + 1) << files[i].name;
        }
        std::size_t expected = 0;
        for (const auto& occurrence : occurrences) {
            expected += occurrence.first_line >= files[i].first && occurrence.first_line <= files[i].last;
        }
        ASSERT_NO_FATAL_FAILURE(load_partition(22, files[i].name));
        expect_partition_coverage(files[i].first, files[i].last, expected);
        covered += expected;
    }
    EXPECT_EQ(covered, occurrences.size());
}

// The merged catalog was complete:false until D3 Task 4: the draft 22 completeness gate passes (170 of 170 required
// rows bound, no blocking finding), so it is complete and loads for production.
TEST_F(Draft22CatalogTest, MergedCatalogIsThePartitionsInOrderAndIsComplete) {
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto merged = RequirementCatalog::load(source, kRoot / "requirements/draft22.json",
                                                 CatalogLoadMode::RequireComplete);
    EXPECT_TRUE(merged.complete);
    std::vector<std::string> expected;
    for (const auto& file : partitions()) {
        const auto part = read_json(kRoot / "requirements/parts" / file.name);
        for (const auto& row : part.at("requirements")) {
            expected.push_back(row.at("id").get<std::string>());
        }
    }
    std::vector<std::string> actual;
    for (const auto& row : merged.requirements) {
        actual.push_back(row.id);
    }
    EXPECT_EQ(actual, expected);
}

TEST_F(Draft22CatalogTest, IdsAreUniqueAcrossAllThreeDrafts) {
    std::set<std::string> ids;
    std::map<unsigned, std::size_t> counts;
    for (const auto draft : {18u, 21u, 22u}) {
        const auto source = load_draft_source(draft, kRoot / "docs", kRoot / "requirements/draft-digests.json");
        for (const auto& entry : std::filesystem::directory_iterator(kRoot / "requirements/parts")) {
            if (!entry.path().filename().string().starts_with("draft" + std::to_string(draft) + "-")) {
                continue;
            }
            const auto part = RequirementCatalog::load(source, entry.path(), CatalogLoadMode::AllowIncomplete);
            for (const auto& row : part.requirements) {
                EXPECT_TRUE(ids.insert(row.id).second) << entry.path() << ": " << row.id;
                ++counts[draft];
            }
        }
    }
    for (const auto draft : {18u, 21u, 22u}) {
        EXPECT_GT(counts[draft], 0u) << "no catalog rows found in parts for draft " << draft;
    }
    const auto source22 = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto merged22 = RequirementCatalog::load(source22, kRoot / "requirements/draft22.json",
                                                   CatalogLoadMode::AllowIncomplete);
    EXPECT_EQ(merged22.requirements.size(), counts[22]);
    std::set<std::string> merged_ids;
    for (const auto& row : merged22.requirements) {
        EXPECT_TRUE(merged_ids.insert(row.id).second) << "duplicate id in draft22.json: " << row.id;
        EXPECT_TRUE(ids.contains(row.id)) << "draft22.json id not in the id set: " << row.id;
    }
}

TEST_F(Draft22CatalogTest, DeltaFileAccountsForEveryRowAndEveryDraft21Row) {
    const auto delta = read_json(kRoot / "requirements/draft21-to-22-delta.json");
    const auto source21 = load_draft_source(21, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto source22 = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto catalog21 = RequirementCatalog::load(source21, kRoot / "requirements/draft21.json");
    const auto catalog22 = RequirementCatalog::load(source22, kRoot / "requirements/draft22.json",
                                                    CatalogLoadMode::AllowIncomplete);
    EXPECT_EQ(delta.at("from_sha256"), source21.sha256);
    EXPECT_EQ(delta.at("to_sha256"), source22.sha256);

    std::set<std::string> ids21;
    for (const auto& row : catalog21.requirements) {
        ids21.insert(row.id);
    }
    static const std::set<std::string> kChanges = {"identical", "moved", "reworded", "new", "removed"};
    std::map<std::string, nlohmann::json> by_22;
    std::set<std::string> seen_21;
    for (const auto& entry : delta.at("entries")) {
        EXPECT_TRUE(entry.at("reviewed").get<bool>())
            << entry.at("draft22_id") << " / " << entry.at("draft21_id") << " is unreviewed";
        const auto change = entry.at("change").get<std::string>();
        const auto id21 = entry.at("draft21_id").get<std::string>();
        const auto id22 = entry.at("draft22_id").get<std::string>();
        EXPECT_TRUE(kChanges.contains(change)) << "bad change '" << change << "' for " << id22 << " / " << id21;
        if (change == "removed") {
            EXPECT_TRUE(id22.empty()) << "removed entry has draft22_id " << id22;
            EXPECT_TRUE(ids21.contains(id21)) << "removed entry is not a draft 21 row: " << id21;
        } else if (change == "new") {
            EXPECT_TRUE(id21.empty()) << "new entry has draft21_id " << id21;
            EXPECT_FALSE(id22.empty()) << "new entry has no draft22_id";
        } else if (change == "identical" || change == "moved" || change == "reworded") {
            EXPECT_FALSE(id22.empty()) << change << " entry has no draft22_id (" << id21 << ")";
            EXPECT_TRUE(ids21.contains(id21)) << change << " entry draft21_id not a draft 21 row: " << id21;
        }
        if (!id21.empty()) {
            seen_21.insert(id21);
        }
        if (const auto id = entry.at("draft22_id").get<std::string>(); !id.empty()) {
            EXPECT_TRUE(by_22.emplace(id, entry).second) << "duplicate delta entry " << id;
        }
    }
    for (const auto& row : catalog21.requirements) {
        EXPECT_TRUE(seen_21.contains(row.id)) << "draft 21 row not accounted for: " << row.id;
    }
    EXPECT_EQ(by_22.size(), catalog22.requirements.size());

    const auto contexts21 = extract_contexts(source21);
    const auto contexts22 = extract_contexts(source22);
    const auto find = [](const std::vector<OccurrenceContext>& contexts, const Requirement& row) {
        for (const auto& context : contexts) {
            if (context.first_line == row.source.first_line &&
                context.occurrence_on_line == row.source.occurrence) {
                return &context;
            }
        }
        return static_cast<const OccurrenceContext*>(nullptr);
    };
    std::map<std::string, const Requirement*> rows21;
    for (const auto& row : catalog21.requirements) {
        rows21[row.id] = &row;
    }
    std::size_t text_checked = 0;
    for (const auto& row : catalog22.requirements) {
        SCOPED_TRACE(row.id);
        ASSERT_TRUE(by_22.contains(row.id)) << "no delta entry for " << row.id;
        const auto& entry = by_22.at(row.id);
        const auto change = entry.at("change").get<std::string>();
        if (change != "identical" && change != "moved") {
            continue;
        }
        ASSERT_TRUE(rows21.contains(entry.at("draft21_id").get<std::string>()));
        const auto* before = find(contexts21, *rows21.at(entry.at("draft21_id").get<std::string>()));
        const auto* after = find(contexts22, row);
        ASSERT_NE(before, nullptr);
        ASSERT_NE(after, nullptr);
        EXPECT_FALSE(after->sentence.empty());
        EXPECT_EQ(before->sentence, after->sentence);
        EXPECT_EQ(before->ordinal_in_sentence, after->ordinal_in_sentence);
        ++text_checked;
    }
    EXPECT_GT(text_checked, 0u) << "no identical/moved entry was checked";
}

TEST_F(Draft22CatalogTest, WireDeltaRecordsTheLocationFilterChangeWithAConclusion) {
    const auto delta = read_json(kRoot / "requirements/draft21-to-22-delta.json");
    const auto& wire = delta.at("wire_delta");
    EXPECT_FALSE(wire.at("conclusion").get<std::string>().empty());
    bool found = false;
    for (const char* key : {"added", "changed"}) {
        for (const auto& name : wire.at(key)) {
            found = found || name.get<std::string>().find("LOCATION_FILTER") != std::string::npos;
        }
    }
    EXPECT_TRUE(found) << "LOCATION_FILTER must appear in wire_delta.added or wire_delta.changed";
}

TEST_F(Draft22CatalogTest, EveryLocationFilterRowIsTagged) {
    const auto delta = read_json(kRoot / "requirements/draft21-to-22-delta.json");
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, kRoot / "requirements/draft22.json",
                                                  CatalogLoadMode::AllowIncomplete);
    const auto contexts = extract_contexts(source);
    std::map<std::string, nlohmann::json> entries;
    for (const auto& entry : delta.at("entries")) {
        const auto id = entry.at("draft22_id").get<std::string>();
        if (!id.empty()) {
            entries[id] = entry;
        }
    }
    std::size_t tagged = 0;
    for (const auto& row : catalog.requirements) {
        for (const auto& context : contexts) {
            if (context.first_line != row.source.first_line ||
                context.occurrence_on_line != row.source.occurrence) {
                continue;
            }
            auto text = context.sentence + " " + context.section_title;
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (text.find("location filter") != std::string::npos ||
                text.find("location_filter") != std::string::npos) {
                ASSERT_TRUE(entries.contains(row.id)) << "no delta entry for " << row.id;
                const auto tags = entries.at(row.id).at("tags").get<std::vector<std::string>>();
                EXPECT_TRUE(std::find(tags.begin(), tags.end(), "location_filter") != tags.end()) << row.id;
                EXPECT_TRUE(entries.at(row.id).at("reviewed").get<bool>()) << row.id << " is unreviewed";
                ++tagged;
            }
        }
    }
    EXPECT_GT(tagged, 0u);
}

}  // namespace
}  // namespace moq::interop::requirements
