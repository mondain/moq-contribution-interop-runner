#include "moq/interop/requirements/lineage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace moq::interop::requirements {
namespace {

Requirement row(std::string id, std::vector<std::string> scenarios, std::vector<std::string> evaluators,
                bool testable = true) {
    return {std::move(id), Strength::Must, {"1", 1, 1, 1, 1}, "endpoint", "summary",
            testable ? Applicability::Applicable : Applicability::NotApplicable,
            testable ? Testability::Testable : Testability::NotApplicable,
            testable ? std::move(scenarios) : std::vector<std::string>{},
            testable ? std::move(evaluators) : std::vector<std::string>{}, "rationale"};
}

DeltaEntry entry(std::string d22, std::string d21, std::string change, std::string note = "",
                 std::vector<std::string> tags = {}) {
    return {std::move(d22), std::move(d21), std::move(change), std::move(note), std::move(tags)};
}

bool has_pair(const std::vector<NamePair>& pairs, const std::string& d22, const std::string& d21) {
    return std::any_of(pairs.begin(), pairs.end(), [&](const auto& p) { return p.d22 == d22 && p.d21 == d21; });
}

struct Fixture {
    RequirementCatalog c21{21, "x", true, {}};
    RequirementCatalog c22{22, "y", false, {}};
    std::vector<DeltaEntry> delta;
    std::vector<Equivalence> equivalences;
    std::set<std::string> filter_scenarios;
    Lineage build() { return build_lineage({c21, c22, delta, equivalences, filter_scenarios}); }
};

TEST(LineageTest, IdenticalMovedAndEditorialRowsAreSharedWithTheirNames) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {"d21-a"}, {"d21-ea"}), row("D21-1-MUST-002", {"d21-b"}, {"d21-eb"}),
                          row("D21-1-MUST-003", {"d21-c"}, {"d21-ec"})};
    f.c22.requirements = {row("D22-1-MUST-001", {"d22-a"}, {"d22-ea"}), row("D22-2-MUST-002", {"d22-b"}, {"d22-eb"}),
                          row("D22-1-MUST-003", {"d22-c"}, {"d22-ec"})};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "identical"),
               entry("D22-2-MUST-002", "D21-1-MUST-002", "moved", "section moved"),
               entry("D22-1-MUST-003", "D21-1-MUST-003", "reworded", "editorial rewording")};
    const auto lineage = f.build();
    EXPECT_EQ(lineage.shared_rows.size(), 3u);
    EXPECT_TRUE(lineage.own_rows.empty());
    EXPECT_TRUE(has_pair(lineage.shared_scenarios, "d22-a", "d21-a"));
    EXPECT_TRUE(has_pair(lineage.shared_scenarios, "d22-b", "d21-b"));
    EXPECT_TRUE(has_pair(lineage.shared_scenarios, "d22-c", "d21-c"));
    EXPECT_TRUE(has_pair(lineage.shared_evaluators, "d22-ec", "d21-ec"));
    EXPECT_TRUE(lineage.own_scenarios.empty());
}

TEST(LineageTest, TaggedObligationChangedNewAndUnpairedRowsAreOwn) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {"d21-a"}, {"d21-ea"}), row("D21-1-MUST-002", {"d21-b"}, {"d21-eb"}),
                          row("D21-1-MUST-003", {"d21-c"}, {"d21-ec"})};
    f.c22.requirements = {row("D22-1-MUST-001", {"d22-a"}, {"d22-ea"}), row("D22-1-MUST-002", {"d22-b"}, {"d22-eb"}),
                          row("D22-1-MUST-003", {"d22-c"}, {"d22-ec"}), row("D22-1-MUST-004", {"d22-n"}, {"d22-en"})};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "identical", "", {"location_filter"}),
               entry("D22-1-MUST-002", "D21-1-MUST-002", "reworded", "obligation changed: x"),
               entry("D22-1-MUST-003", "D21-1-MUST-003", "reworded", "similarity=0.80"),
               entry("D22-1-MUST-004", "", "new", "unreviewed new")};
    const auto lineage = f.build();
    EXPECT_TRUE(lineage.shared_rows.empty());
    EXPECT_EQ(lineage.own_rows.size(), 4u);
    EXPECT_EQ(lineage.own_scenarios, (std::vector<std::string>{"d22-a", "d22-b", "d22-c", "d22-n"}));
}

TEST(LineageTest, RePairedByHandCountsAsTheSameObligation) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {"d21-a"}, {"d21-ea"})};
    f.c22.requirements = {row("D22-1-MUST-001", {"d22-a"}, {"d22-ea"})};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "reworded", "re-paired by hand: same obligation")};
    EXPECT_EQ(f.build().shared_rows.size(), 1u);
}

TEST(LineageTest, AScenarioCoveringASharedAndAnOwnRowIsOwn) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {"d21-a"}, {"d21-ea"}), row("D21-1-MUST-002", {"d21-a"}, {"d21-eb"})};
    f.c22.requirements = {row("D22-1-MUST-001", {"d22-a"}, {"d22-ea"}), row("D22-1-MUST-002", {"d22-a"}, {"d22-eb"})};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "identical"),
               entry("D22-1-MUST-002", "D21-1-MUST-002", "reworded", "obligation changed")};
    const auto lineage = f.build();
    EXPECT_EQ(lineage.shared_rows.size(), 1u);
    EXPECT_TRUE(lineage.shared_scenarios.empty());
    EXPECT_EQ(lineage.own_scenarios, std::vector<std::string>{"d22-a"});
    // The evaluator that only scores the shared row stays shared.
    EXPECT_TRUE(has_pair(lineage.shared_evaluators, "d22-ea", "d21-ea"));
    EXPECT_EQ(lineage.own_evaluators, std::vector<std::string>{"d22-eb"});
}

TEST(LineageTest, FilterBuildingScenariosAreOwnEvenWhenTheirRowsAreShared) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {"d21-filter"}, {"d21-ea"})};
    f.c22.requirements = {row("D22-1-MUST-001", {"d22-filter"}, {"d22-ea"})};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "identical")};
    f.filter_scenarios = {"d21-filter"};
    const auto lineage = f.build();
    EXPECT_EQ(lineage.shared_rows.size(), 1u);
    EXPECT_TRUE(lineage.shared_scenarios.empty());
    EXPECT_EQ(lineage.own_scenarios, std::vector<std::string>{"d22-filter"});
}

TEST(LineageTest, ARowWhosePlannedIdsDifferFromItsCounterpartIsNotShared) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {"d21-a"}, {"d21-ea"})};
    f.c22.requirements = {row("D22-1-MUST-001", {"d22-different"}, {"d22-ea"})};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "identical")};
    const auto lineage = f.build();
    EXPECT_TRUE(lineage.shared_rows.empty());
    EXPECT_EQ(lineage.own_rows, std::vector<std::string>{"D22-1-MUST-001"});
}

TEST(LineageTest, EquivalencesOverrideTheDeltaAndMapOneRowToSeveral) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {"d21-a"}, {"d21-ea"}), row("D21-1-MUST-002", {"d21-a"}, {"d21-ea"})};
    f.c22.requirements = {row("D22-1-MUST-001", {"d22-a"}, {"d22-ea"})};
    f.delta = {entry("D22-1-MUST-001", "", "new", "text identical; the tool missed it")};
    f.equivalences = {{"D22-1-MUST-001", {"D21-1-MUST-001", "D21-1-MUST-002"}, "page-break miss"}};
    const auto lineage = f.build();
    ASSERT_EQ(lineage.shared_rows.size(), 2u);
    EXPECT_EQ(lineage.shared_rows[0].d21, "D21-1-MUST-001");
    EXPECT_EQ(lineage.shared_rows[1].d21, "D21-1-MUST-002");
    EXPECT_EQ(lineage.shared_rows[0].d22, "D22-1-MUST-001");
    EXPECT_TRUE(has_pair(lineage.shared_scenarios, "d22-a", "d21-a"));
}

TEST(LineageTest, NonTestableRowsAreSharedByTheirDeltaButNameNoScenarios) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-001", {}, {}, false)};
    f.c22.requirements = {row("D22-1-MUST-001", {}, {}, false)};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "identical")};
    const auto lineage = f.build();
    EXPECT_EQ(lineage.shared_rows.size(), 1u);
    EXPECT_TRUE(lineage.shared_scenarios.empty());
    EXPECT_TRUE(lineage.own_scenarios.empty());
}

TEST(LineageTest, RenderedHeaderIsDeterministicSortedAndSelfContained) {
    Fixture f;
    f.c21.requirements = {row("D21-1-MUST-002", {"d21-b"}, {"d21-eb"}), row("D21-1-MUST-001", {"d21-a"}, {"d21-ea"})};
    f.c22.requirements = {row("D22-1-MUST-002", {"d22-b"}, {"d22-eb"}), row("D22-1-MUST-001", {"d22-a"}, {"d22-ea"}),
                          row("D22-1-MUST-009", {"d22-n"}, {"d22-en"})};
    f.delta = {entry("D22-1-MUST-001", "D21-1-MUST-001", "identical"),
               entry("D22-1-MUST-002", "D21-1-MUST-002", "identical"), entry("D22-1-MUST-009", "", "new")};
    const auto lineage = f.build();
    const auto first = render_lineage_header(lineage);
    EXPECT_EQ(first, render_lineage_header(f.build()));
    EXPECT_NE(first.find("#pragma once"), std::string::npos);
    EXPECT_NE(first.find("namespace moq::interop::requirements::lineage_data"), std::string::npos);
    const auto a = first.find("\"d22-a\"");
    const auto b = first.find("\"d22-b\"");
    ASSERT_NE(a, std::string::npos);
    ASSERT_NE(b, std::string::npos);
    EXPECT_LT(a, b);
    EXPECT_NE(first.find("kSharedRows"), std::string::npos);
    EXPECT_NE(first.find("kSharedScenarios"), std::string::npos);
    EXPECT_NE(first.find("kSharedScenarioIds22"), std::string::npos);
    EXPECT_NE(first.find("kOwnScenarios22"), std::string::npos);
    EXPECT_NE(first.find("GENERATED"), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::requirements
