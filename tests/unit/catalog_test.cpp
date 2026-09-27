#include "moq/interop/requirements/catalog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace moq::interop::requirements {
namespace {

DraftSource synthetic_source() {
    return {18, {}, "abc123", "Endpoint MUST send and log.\nOther text.\n", {0, 28}};
}

nlohmann::json valid_record() {
    return {{"id", "18-1-1-1"},
            {"strength", "Must"},
            {"source", {{"section", "1"}, {"first_line", 1}, {"last_line", 1},
                        {"occurrence", 1}, {"clause", 1}}},
            {"actor", "endpoint"},
            {"summary", "Send the object"},
            {"applicability", "Applicable"},
            {"testability", "Testable"},
            {"scenarios", {"send-object"}},
            {"evaluators", {"object-sent"}},
            {"rationale", "Publisher sends an observable object"}};
}

nlohmann::json valid_catalog() {
    return {{"draft", 18}, {"source_sha256", "abc123"}, {"complete", true},
            {"requirements", {valid_record()}}};
}

class CatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("moq-catalog-test-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    RequirementCatalog load(const nlohmann::json& value,
                            CatalogLoadMode mode = CatalogLoadMode::RequireComplete) {
        const auto path = directory_ / "catalog.json";
        std::ofstream output(path);
        output << value.dump();
        output.close();
        return RequirementCatalog::load(synthetic_source(), path, mode);
    }

    std::filesystem::path directory_;
};

TEST_F(CatalogTest, LoadsCompleteCatalogWithDistinctClausesOnOneKeyword) {
    auto value = valid_catalog();
    auto second = valid_record();
    second["id"] = "18-1-1-2";
    second["source"]["clause"] = 2;
    second["summary"] = "Log the object";
    value["requirements"].push_back(second);

    const auto catalog = load(value);
    ASSERT_EQ(catalog.requirements.size(), 2u);
    EXPECT_EQ(catalog.requirements[0].source.occurrence, 1u);
    EXPECT_EQ(catalog.requirements[1].source.clause, 2u);
    EXPECT_EQ(catalog.requirements[0].strength, Strength::Must);
}

TEST_F(CatalogTest, IncompleteEnvelopeRequiresExplicitAssemblyMode) {
    auto value = valid_catalog();
    value["complete"] = false;
    value["requirements"] = nlohmann::json::array();
    EXPECT_THROW(load(value), std::runtime_error);
    const auto catalog = load(value, CatalogLoadMode::AllowIncomplete);
    EXPECT_FALSE(catalog.complete);
    EXPECT_TRUE(catalog.requirements.empty());
}

TEST_F(CatalogTest, CheckedInEnvelopesArePinnedButRejectedForProduction) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    for (const unsigned number : {18u, 21u}) {
        const auto source = load_draft_source(number, root / "docs",
                                               root / "requirements/draft-digests.json");
        const auto path = root / "requirements" / ("draft" + std::to_string(number) + ".json");
        EXPECT_THROW(RequirementCatalog::load(source, path), std::runtime_error);
        const auto catalog = RequirementCatalog::load(source, path, CatalogLoadMode::AllowIncomplete);
        EXPECT_EQ(catalog.draft, number);
        EXPECT_EQ(catalog.source_sha256, source.sha256);
        EXPECT_FALSE(catalog.complete);
        EXPECT_TRUE(catalog.requirements.empty());
    }
}

TEST_F(CatalogTest, RejectsTrailingJsonData) {
    const auto path = directory_ / "catalog.json";
    std::ofstream output(path);
    output << valid_catalog().dump() << " garbage";
    output.close();
    EXPECT_THROW(RequirementCatalog::load(synthetic_source(), path), std::runtime_error);
}

TEST_F(CatalogTest, RejectsUnknownAndMissingFieldsAtEveryLevel) {
    auto value = valid_catalog();
    value["extra"] = 1;
    EXPECT_THROW(load(value), std::runtime_error);
    value = valid_catalog();
    value.erase("complete");
    EXPECT_THROW(load(value), std::runtime_error);
    value = valid_catalog();
    value["requirements"][0]["extra"] = 1;
    EXPECT_THROW(load(value), std::runtime_error);
    value = valid_catalog();
    value["requirements"][0].erase("actor");
    EXPECT_THROW(load(value), std::runtime_error);
    value = valid_catalog();
    value["requirements"][0]["source"]["extra"] = 1;
    EXPECT_THROW(load(value), std::runtime_error);
    value = valid_catalog();
    value["requirements"][0]["source"].erase("occurrence");
    EXPECT_THROW(load(value), std::runtime_error);
}

TEST_F(CatalogTest, RejectsUnknownEnums) {
    auto value = valid_catalog();
    for (const auto& field : {"strength", "applicability", "testability"}) {
        auto invalid = value;
        invalid["requirements"][0][field] = "Unknown";
        EXPECT_THROW(load(invalid), std::runtime_error) << field;
    }
}

TEST_F(CatalogTest, RejectsDuplicateIdsAndSemanticClauseAnchors) {
    auto value = valid_catalog();
    value["requirements"].push_back(valid_record());
    EXPECT_THROW(load(value), std::runtime_error);
    value["requirements"][1]["id"] = "different-id";
    EXPECT_THROW(load(value), std::runtime_error);
}

TEST_F(CatalogTest, RejectsDraftAndDigestMismatch) {
    auto value = valid_catalog();
    value["draft"] = 21;
    EXPECT_THROW(load(value), std::runtime_error);
    value = valid_catalog();
    value["source_sha256"] = "wrong";
    EXPECT_THROW(load(value), std::runtime_error);
}

TEST_F(CatalogTest, RejectsInvalidAndReversedCitationsAndOrdinals) {
    for (const auto& [field, invalid] : std::vector<std::pair<std::string, int>>{
             {"first_line", 0}, {"first_line", 3}, {"last_line", 0},
             {"last_line", 3}, {"occurrence", 0}, {"clause", 0}}) {
        auto value = valid_catalog();
        value["requirements"][0]["source"][field] = invalid;
        EXPECT_THROW(load(value), std::runtime_error) << field;
    }
    auto value = valid_catalog();
    value["requirements"][0]["source"]["first_line"] = 2;
    EXPECT_THROW(load(value), std::runtime_error);
}

TEST_F(CatalogTest, RejectsCitationEndingBeforeWrappedPhrase) {
    const DraftSource source{18, {}, "abc123", "Endpoint MUST\n NOT send.\n", {0, 14}};
    auto value = valid_catalog();
    value["requirements"][0]["strength"] = "MustNot";
    const auto path = directory_ / "catalog.json";
    {
        std::ofstream output(path);
        output << value.dump();
    }
    EXPECT_THROW(RequirementCatalog::load(source, path), std::runtime_error);
    value["requirements"][0]["source"]["last_line"] = 2;
    {
        std::ofstream output(path);
        output << value.dump();
    }
    const auto catalog = RequirementCatalog::load(source, path);
    ASSERT_EQ(catalog.requirements.size(), 1u);
    EXPECT_EQ(catalog.requirements[0].source.last_line, 2u);
}

TEST_F(CatalogTest, RejectsIncompatibleClassificationAndMissingIdentifiers) {
    auto value = valid_catalog();
    value["requirements"][0]["applicability"] = "Informative";
    EXPECT_THROW(load(value), std::runtime_error);
    value = valid_catalog();
    value["requirements"][0]["testability"] = "NotApplicable";
    EXPECT_THROW(load(value), std::runtime_error);
    for (const auto& field : {"scenarios", "evaluators"}) {
        value = valid_catalog();
        value["requirements"][0][field] = nlohmann::json::array();
        EXPECT_THROW(load(value), std::runtime_error) << field;
        value["requirements"][0][field] = {""};
        EXPECT_THROW(load(value), std::runtime_error) << field;
    }
}

TEST_F(CatalogTest, AcceptsExplicitNonApplicableAndInformativeRecords) {
    auto value = valid_catalog();
    auto& row = value["requirements"][0];
    row["applicability"] = "Informative";
    row["testability"] = "NotApplicable";
    row["scenarios"] = nlohmann::json::array();
    row["evaluators"] = nlohmann::json::array();
    EXPECT_EQ(load(value).requirements[0].applicability, Applicability::Informative);
    row["applicability"] = "NotApplicable";
    EXPECT_EQ(load(value).requirements[0].applicability, Applicability::NotApplicable);
}

}  // namespace
}  // namespace moq::interop::requirements
