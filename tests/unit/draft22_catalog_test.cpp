#include "../support/catalog_partition.h"
#include "moq/interop/requirements/carry_forward.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <regex>

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
    return nlohmann::json::parse(input);
}

class Draft22CatalogTest : public test::CatalogPartitionTest {};

TEST_F(Draft22CatalogTest, PartitionsTileTheWholeDraftAndCoverEveryOccurrence) {
    const auto files = partitions();
    ASSERT_FALSE(files.empty());
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    EXPECT_EQ(files.front().first, 1u);
    EXPECT_EQ(files.back().last, source.line_offsets.size());
    std::size_t covered = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i > 0) {
            EXPECT_EQ(files[i].first, files[i - 1].last + 1) << files[i].name;
        }
        std::size_t expected = 0;
        for (const auto& occurrence : scan_normative_occurrences(source)) {
            expected += occurrence.first_line >= files[i].first && occurrence.first_line <= files[i].last;
        }
        ASSERT_NO_FATAL_FAILURE(load_partition(22, files[i].name));
        expect_partition_coverage(files[i].first, files[i].last, expected);
        covered += expected;
    }
    EXPECT_EQ(covered, scan_normative_occurrences(source).size());
}

TEST_F(Draft22CatalogTest, MergedCatalogIsThePartitionsInOrderAndIsIncomplete) {
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto merged = RequirementCatalog::load(source, kRoot / "requirements/draft22.json",
                                                 CatalogLoadMode::AllowIncomplete);
    EXPECT_FALSE(merged.complete);
    std::vector<std::string> expected;
    for (const auto& file : partitions()) {
        for (const auto& row : read_json(kRoot / "requirements/parts" / file.name).at("requirements")) {
            expected.push_back(row.at("id").get<std::string>());
        }
    }
    std::vector<std::string> actual;
    for (const auto& row : merged.requirements) {
        actual.push_back(row.id);
    }
    EXPECT_EQ(actual, expected);
    EXPECT_THROW(RequirementCatalog::load(source, kRoot / "requirements/draft22.json",
                                          CatalogLoadMode::RequireComplete),
                 std::runtime_error);
}

TEST_F(Draft22CatalogTest, IdsAreUniqueAcrossAllThreeDrafts) {
    std::set<std::string> ids;
    for (const auto draft : {18u, 21u, 22u}) {
        const auto source = load_draft_source(draft, kRoot / "docs", kRoot / "requirements/draft-digests.json");
        for (const auto& entry : std::filesystem::directory_iterator(kRoot / "requirements/parts")) {
            if (!entry.path().filename().string().starts_with("draft" + std::to_string(draft) + "-")) {
                continue;
            }
            const auto part = RequirementCatalog::load(source, entry.path(), CatalogLoadMode::AllowIncomplete);
            for (const auto& row : part.requirements) {
                EXPECT_TRUE(ids.insert(row.id).second) << entry.path() << ": " << row.id;
            }
        }
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

    std::map<std::string, nlohmann::json> by_22;
    std::set<std::string> seen_21;
    for (const auto& entry : delta.at("entries")) {
        EXPECT_TRUE(entry.at("reviewed").get<bool>())
            << entry.at("draft22_id") << " / " << entry.at("draft21_id") << " is unreviewed";
        if (const auto id = entry.at("draft21_id").get<std::string>(); !id.empty()) {
            seen_21.insert(id);
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
    for (const auto& row : catalog22.requirements) {
        SCOPED_TRACE(row.id);
        const auto& entry = by_22.at(row.id);
        if (entry.at("change") != "identical") {
            continue;
        }
        const auto* before = find(contexts21, *rows21.at(entry.at("draft21_id").get<std::string>()));
        const auto* after = find(contexts22, row);
        ASSERT_NE(before, nullptr);
        ASSERT_NE(after, nullptr);
        EXPECT_FALSE(after->sentence.empty());
        EXPECT_EQ(before->sentence, after->sentence);
        EXPECT_EQ(before->ordinal_in_sentence, after->ordinal_in_sentence);
    }
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

TEST_F(Draft22CatalogTest, EveryLocationFilterRowIsTaggedAndReviewed) {
    const auto delta = read_json(kRoot / "requirements/draft21-to-22-delta.json");
    const auto source = load_draft_source(22, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, kRoot / "requirements/draft22.json",
                                                  CatalogLoadMode::AllowIncomplete);
    const auto contexts = extract_contexts(source);
    std::map<std::string, std::vector<std::string>> tags;
    for (const auto& entry : delta.at("entries")) {
        tags[entry.at("draft22_id")] = entry.at("tags").get<std::vector<std::string>>();
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
                EXPECT_TRUE(std::find(tags[row.id].begin(), tags[row.id].end(), "location_filter") !=
                            tags[row.id].end()) << row.id;
                ++tagged;
            }
        }
    }
    EXPECT_GT(tagged, 0u);
}

}  // namespace
}  // namespace moq::interop::requirements
