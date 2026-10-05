#include "moq/interop/requirements/lineage_translate.h"

#include "moq/interop/requirements/draft22_lineage_data.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::requirements {
namespace {

using lineage_data::kSharedRows;

OutcomeState state_of(const std::vector<Outcome>& outcomes, std::string_view id) {
    const auto it = std::find_if(outcomes.begin(), outcomes.end(),
                                 [&](const Outcome& outcome) { return outcome.requirement_id == id; });
    EXPECT_NE(it, outcomes.end()) << id;
    return it == outcomes.end() ? OutcomeState::NotRun : it->state;
}

std::vector<Outcome> translate(const std::vector<Outcome>& in) {
    return translate_shared_outcomes(std::span<const Outcome>(in));
}

TEST(LineageTranslate, MapsASharedRowToItsDraft22Id) {
    const auto& pair = kSharedRows.front();
    const auto result = translate({{std::string(pair.d21), OutcomeState::Pass}});
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(state_of(result, pair.d22), OutcomeState::Pass);
    EXPECT_TRUE(std::none_of(result.begin(), result.end(),
                             [&](const Outcome& o) { return o.requirement_id == pair.d21; }));
}

TEST(LineageTranslate, DropsOutcomesForRowsWithoutASharedCounterpart) {
    const auto result = translate({{"D21-0-MUST-000-not-a-row", OutcomeState::Fail},
                                   {"D22-1-MUST-001", OutcomeState::Pass}});
    EXPECT_TRUE(result.empty());
}

TEST(LineageTranslate, EmptyInputGivesEmptyOutput) {
    EXPECT_TRUE(translate({}).empty());
}

TEST(LineageTranslate, EveryDraft21IdMapsToEveryOfItsDraft22Targets) {
    std::map<std::string, std::vector<std::string>> targets;
    for (const auto& pair : kSharedRows) targets[std::string(pair.d21)].push_back(std::string(pair.d22));
    for (const auto& [d21, d22s] : targets) {
        const auto result = translate({{d21, OutcomeState::Pass}});
        EXPECT_EQ(result.size(), d22s.size()) << d21;
        for (const auto& id : d22s) EXPECT_EQ(state_of(result, id), OutcomeState::Pass) << id;
    }
}

TEST(LineageTranslate, SeveralDraft21RowsMergeIntoOneDraft22RowBySeverity) {
    // D22-9-MUST-294 merges D21-9-MUST-282 and D21-9-MUST-283 (a hand-established equivalence).
    const auto merged = [](OutcomeState a, OutcomeState b) {
        const auto result = translate({{"D21-9-MUST-282", a}, {"D21-9-MUST-283", b}});
        EXPECT_EQ(std::count_if(result.begin(), result.end(),
                                [](const Outcome& o) { return o.requirement_id == "D22-9-MUST-294"; }), 1);
        return state_of(result, "D22-9-MUST-294");
    };
    EXPECT_EQ(merged(OutcomeState::Pass, OutcomeState::Pass), OutcomeState::Pass);
    EXPECT_EQ(merged(OutcomeState::Pass, OutcomeState::Fail), OutcomeState::Fail);
    EXPECT_EQ(merged(OutcomeState::Fail, OutcomeState::Pass), OutcomeState::Fail);
    EXPECT_EQ(merged(OutcomeState::Fail, OutcomeState::NotRun), OutcomeState::Fail);
    EXPECT_EQ(merged(OutcomeState::Pass, OutcomeState::NotRun), OutcomeState::NotRun);
    EXPECT_EQ(merged(OutcomeState::NotTestable, OutcomeState::Pass), OutcomeState::Pass);
    EXPECT_EQ(merged(OutcomeState::NotApplicable, OutcomeState::NotTestable), OutcomeState::NotTestable);
    EXPECT_EQ(merged(OutcomeState::NotApplicable, OutcomeState::NotApplicable), OutcomeState::NotApplicable);
}

TEST(LineageTranslate, RepeatedDraft21IdMergesToOneFailEntry) {
    const auto result = translate({{"D21-9-MUST-282", OutcomeState::Pass},
                                   {"D21-9-MUST-282", OutcomeState::Fail}});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].requirement_id, "D22-9-MUST-294");
    EXPECT_EQ(result[0].state, OutcomeState::Fail);
}

TEST(LineageTranslate, OutputKeepsFirstSeenOrderAndNeverDuplicatesADraft22Id) {
    std::vector<Outcome> in;
    for (std::size_t i = 0; i < kSharedRows.size() && i < 20; ++i) in.push_back({std::string(kSharedRows[i].d21), OutcomeState::Pass});
    const auto out = translate(in);
    std::set<std::string> seen;
    for (const auto& outcome : out) EXPECT_TRUE(seen.insert(outcome.requirement_id).second) << outcome.requirement_id;
    // First-seen order: input rows are sorted by d21, so the output follows the pairs' order.
    std::vector<std::string> expected;
    for (std::size_t i = 0; i < kSharedRows.size() && i < 20; ++i) {
        const std::string id(kSharedRows[i].d22);
        if (std::find(expected.begin(), expected.end(), id) == expected.end()) expected.push_back(id);
    }
    ASSERT_EQ(out.size(), expected.size());
    for (std::size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i].requirement_id, expected[i]);
}

}  // namespace
}  // namespace moq::interop::requirements
