#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft18_gap_a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

namespace moq::interop {
namespace {

TEST(Draft18GapARegistry, ScenarioListMatchesProbeProfilesExactly) {
    const auto probes = scenarios::draft18_gap_a_probes();
    ASSERT_EQ(probes.size(), app::kDraft18GapAScenarios.size());
    std::set<std::string> requirements;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        EXPECT_EQ(probes[i].definition.id, app::kDraft18GapAScenarios[i]);
        EXPECT_TRUE(requirements.insert(probes[i].requirement_id).second) << probes[i].requirement_id;
        EXPECT_TRUE(scenarios::draft18_gap_a_scenario(probes[i].definition.id));
        EXPECT_TRUE(app::executable_scenario(18, probes[i].definition.id));
        EXPECT_TRUE(app::raw_probe_scenario(18, probes[i].definition.id));
        EXPECT_FALSE(app::executable_scenario(21, probes[i].definition.id));
        const auto in_track_list = std::find(app::kDraft18GapATrackScenarios.begin(),
            app::kDraft18GapATrackScenarios.end(), probes[i].definition.id) !=
            app::kDraft18GapATrackScenarios.end();
        EXPECT_EQ(scenarios::draft18_gap_a_requires_track(probes[i].definition.id), in_track_list)
            << probes[i].definition.id;
        EXPECT_EQ(app::scenario_requires_track(18, probes[i].definition.id), in_track_list);
    }
    for (const auto& id : app::kDraft18GapATrackScenarios)
        EXPECT_TRUE(scenarios::draft18_gap_a_requires_track(id)) << id;
}

TEST(Draft18GapARegistry, CoreScenarioListIsUnchangedAndPrecedesGapA) {
    const auto all = app::executable_scenarios(18);
    ASSERT_EQ(all.size(), app::kDraft18ExecutableScenarios.size() + app::kDraft18GapAScenarios.size());
    EXPECT_EQ(all.front(), "subscribe-to-publisher-track");
    EXPECT_TRUE(std::equal(app::kDraft18GapAScenarios.begin(), app::kDraft18GapAScenarios.end(),
        all.begin() + static_cast<std::ptrdiff_t>(app::kDraft18ExecutableScenarios.size())));
}

TEST(Draft18GapARegistry, RejectsInvalidDeadlineAndFixture) {
    EXPECT_THROW(scenarios::draft18_gap_a_probes(std::chrono::milliseconds(0)), std::invalid_argument);
    EXPECT_THROW(scenarios::draft18_gap_a_probes(std::chrono::milliseconds(10), {{std::byte{'.'}}}),
        std::invalid_argument);
    EXPECT_THROW(scenarios::draft18_gap_a_probes(std::chrono::milliseconds(10), {{}}), std::invalid_argument);
    EXPECT_NO_THROW(scenarios::draft18_gap_a_probes());
}

}  // namespace
}  // namespace moq::interop
