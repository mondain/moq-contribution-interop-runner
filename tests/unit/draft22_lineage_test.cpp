#include "moq/interop/requirements/draft22_lineage_data.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/lineage_policy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace moq::interop::requirements {
namespace {

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

RequirementCatalog load(unsigned draft, CatalogLoadMode mode) {
    const auto source = load_draft_source(draft, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    return RequirementCatalog::load(source, kRoot / "requirements" / ("draft" + std::to_string(draft) + ".json"), mode);
}

Lineage generate() {
    const auto c21 = load(21, CatalogLoadMode::RequireComplete);
    const auto c22 = load(22, CatalogLoadMode::AllowIncomplete);
    return build_lineage({c21, c22, load_delta_entries(kRoot / "requirements/draft21-to-22-delta.json"),
                          draft22_equivalences(), draft22_filter_building_scenarios()});
}

TEST(Draft22Lineage, CheckedInHeaderMatchesWhatTheGeneratorProduces) {
    std::ifstream file(kRoot / "include/moq/interop/requirements/draft22_lineage_data.h");
    ASSERT_TRUE(file);
    std::ostringstream actual;
    actual << file.rdbuf();
    EXPECT_EQ(actual.str(), render_lineage_header(generate()))
        << "regenerate with: moq-interop-catalog-carry lineage";
}

TEST(Draft22Lineage, TheOnlyRowsThatChangedTheirObligationAreOwn) {
    const auto lineage = generate();
    const auto own = [&](const std::string& id) {
        return std::find(lineage.own_rows.begin(), lineage.own_rows.end(), id) != lineage.own_rows.end();
    };
    for (const char* id : {"D22-3-3-1-MUST-NOT-069", "D22-4-1-MAY-101", "D22-6-3-MAY-159", "D22-4-2-MUST-110",
                           "D22-9-20-9-MAY-422", "D22-9-20-9-MUST-424", "D22-7-5-MAY-218"}) {
        EXPECT_TRUE(own(id)) << id;
    }
    for (const char* id : {"D22-8-4-MAY-263", "D22-9-MUST-294", "D22-4-2-MUST-108", "D22-3-6-MUST-082"}) {
        EXPECT_FALSE(own(id)) << id;
    }
}

TEST(Draft22Lineage, EveryDraft21SourceRowAndNameExistsAndEveryNameIsClassifiedOnce) {
    const auto c21 = load(21, CatalogLoadMode::RequireComplete);
    const auto c22 = load(22, CatalogLoadMode::AllowIncomplete);
    std::set<std::string> ids21;
    std::set<std::string> scenarios21;
    for (const auto& row : c21.requirements) {
        ids21.insert(row.id);
        scenarios21.insert(row.scenarios.begin(), row.scenarios.end());
    }
    for (const auto& pair : lineage_data::kSharedRows) {
        EXPECT_TRUE(ids21.contains(std::string(pair.d21))) << pair.d21;
    }
    for (const auto& pair : lineage_data::kSharedScenarios) {
        EXPECT_TRUE(scenarios21.contains(std::string(pair.d21))) << pair.d21;
    }
    std::set<std::string> planned22;
    for (const auto& row : c22.requirements) {
        if (row.applicability == Applicability::Applicable && row.testability == Testability::Testable) {
            planned22.insert(row.scenarios.begin(), row.scenarios.end());
        }
    }
    EXPECT_EQ(lineage_data::kSharedScenarios.size() + lineage_data::kOwnScenarios22.size(), planned22.size());
    for (const auto name : lineage_data::kOwnScenarios22) {
        for (const auto& pair : lineage_data::kSharedScenarios) EXPECT_NE(pair.d22, name);
    }
}

// Pinned so a change in the delta file is a visible event. Update only with a reviewed delta change.
TEST(Draft22Lineage, CountsArePinned) {
    EXPECT_EQ(lineage_data::kSharedRows.size(), 597);
    EXPECT_EQ(lineage_data::kOwnRows22.size(), 17);
    EXPECT_EQ(lineage_data::kSharedScenarios.size(), 259);
    EXPECT_EQ(lineage_data::kOwnScenarios22.size(), 56);
    EXPECT_EQ(lineage_data::kSharedEvaluators.size(), 262);
    EXPECT_EQ(lineage_data::kOwnEvaluators22.size(), 6);
}

}  // namespace
}  // namespace moq::interop::requirements
