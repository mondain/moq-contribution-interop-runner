#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/draft21_evaluators.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::requirements {
namespace {

const RequirementCatalog& catalog21() {
    static const auto catalog = [] {
        const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
        const auto source = load_draft_source(
            21, root / "docs", root / "requirements/draft-digests.json");
        return RequirementCatalog::load(source, root / "requirements/draft21.json");
    }();
    return catalog;
}

// Rows closed by slice A. Each must have every named scenario registered and
// every named evaluator bound with usable evidence.
const std::vector<std::string_view> kClaimedRows{
    "D21-6-3-MUST-NOT-141", "D21-9-1-MUST-NOT-289", "D21-9-1-1-MUST-NOT-292",
    "D21-9-1-2-MUST-NOT-299", "D21-9-1-1-MUST-294", "D21-9-1-2-MUST-301",
};

TEST(Draft21GapA, ClaimedRowsHaveCompleteExecutableBindings) {
    const auto report = audit_completeness(
        catalog21(), draft21_executable_bindings(), app::executable_scenarios(21));
    for (const auto row : kClaimedRows) {
        for (const auto& finding : report.findings) {
            EXPECT_NE(finding.requirement_id, row) << finding.code << ' ' << finding.detail;
        }
    }
}

TEST(Draft21GapA, AnnouncementScenariosRouteToTheAnnouncementController) {
    for (const auto id : app::kDraft21GapAnnouncementScenarios) {
        EXPECT_TRUE(app::executable_scenario(21, id)) << id;
        EXPECT_TRUE(app::announcement_gap_scenario(21, id)) << id;
        EXPECT_FALSE(app::raw_probe_scenario(21, id)) << id;
        EXPECT_TRUE(app::scenario_requires_track(21, id)) << id;
        EXPECT_FALSE(app::executable_scenario(18, id)) << id;
    }
}

TEST(Draft21GapA, BindingsNeverNameUnregisteredScenarios) {
    const auto report = audit_completeness(
        catalog21(), draft21_executable_bindings(), app::executable_scenarios(21));
    for (const auto& finding : report.findings) {
        EXPECT_NE(finding.code, "nonexecutable_scenario") << finding.requirement_id;
        EXPECT_NE(finding.code, "mismatched_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "duplicate_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "orphan_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "missing_evidence_schema") << finding.requirement_id;
    }
}

}  // namespace
}  // namespace moq::interop::requirements
