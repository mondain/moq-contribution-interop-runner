#include "moq/interop/app/lineage.h"
#include "moq/interop/app/scenario_registry.h"

#include <gtest/gtest.h>

#include <string>

namespace moq::interop::app {
namespace {

TEST(LineageRegistry, ExecutableDraft22ScenariosAreTheSharedOnesWhoseDraft21ImplementationRuns) {
    const auto ids = executable_scenarios(22);
    ASSERT_FALSE(ids.empty());
    std::size_t expected = 0;
    for (const auto d22 : shared_scenario_ids_22()) {
        ASSERT_TRUE(d22.starts_with("d22-")) << d22;
        const auto implementation = implementation_scenario_id(d22);
        ASSERT_TRUE(implementation.has_value()) << d22;
        EXPECT_TRUE(implementation->starts_with("d21-")) << d22;
        const bool runs = executable_scenario(21, *implementation);
        expected += runs ? 1 : 0;
        EXPECT_EQ(executable_scenario(22, d22), runs) << d22;
    }
    EXPECT_EQ(ids.size(), expected);
    for (const auto id : ids) {
        const auto implementation = implementation_scenario_id(id);
        ASSERT_TRUE(implementation.has_value()) << id;
        EXPECT_TRUE(executable_scenario(21, *implementation)) << id;
    }
}

TEST(LineageRegistry, OwnAndUnknownIdsAreNotExecutableForDraft22) {
    for (const char* id : {"", "no-such-scenario", "d21-setup-unknown-options", "d22-no-such-scenario",
                           "d22-location-filter-unknown-type"}) {
        EXPECT_FALSE(executable_scenario(22, id)) << id;
        EXPECT_FALSE(implementation_scenario_id(id).has_value()) << id;
        EXPECT_FALSE(raw_probe_scenario(22, id)) << id;
        EXPECT_FALSE(scenario_requires_track(22, id)) << id;
        EXPECT_FALSE(scenario_requires_fetch(22, id)) << id;
        EXPECT_FALSE(scenario_required_capability(22, id).has_value()) << id;
        EXPECT_FALSE(gap_raw_scenario(22, id)) << id;
        EXPECT_FALSE(announcement_gap_scenario(22, id)) << id;
        EXPECT_FALSE(draft21_contribution_scenario(22, id)) << id;
    }
}

namespace {
// Every wrapped predicate answers for a draft 22 id exactly as draft 21 does for the implementation id.
void expect_forwards(std::string_view id, std::string_view impl) {
    EXPECT_EQ(raw_probe_scenario(22, id), raw_probe_scenario(21, impl)) << id;
    EXPECT_EQ(fetch_first_object_scenario(22, id), fetch_first_object_scenario(21, impl)) << id;
    EXPECT_EQ(immutable_repeat_scenario(22, id), immutable_repeat_scenario(21, impl)) << id;
    EXPECT_EQ(object_repeat_scenario(22, id), object_repeat_scenario(21, impl)) << id;
    EXPECT_EQ(fetch_group_order_scenario(22, id), fetch_group_order_scenario(21, impl)) << id;
    EXPECT_EQ(subscriber_notify_scenario(22, id), subscriber_notify_scenario(21, impl)) << id;
    EXPECT_EQ(established_update_scenario(22, id), established_update_scenario(21, impl)) << id;
    EXPECT_EQ(request_goaway_scenario(22, id), request_goaway_scenario(21, impl)) << id;
    EXPECT_EQ(discovery_overlap_scenario(22, id), discovery_overlap_scenario(21, impl)) << id;
    EXPECT_EQ(scenario_requires_track(22, id), scenario_requires_track(21, impl)) << id;
    EXPECT_EQ(draft21_contribution_scenario(22, id), draft21_contribution_scenario(21, impl)) << id;
    EXPECT_EQ(announcement_gap_scenario(22, id), announcement_gap_scenario(21, impl)) << id;
    EXPECT_EQ(gap_raw_scenario(22, id), gap_raw_scenario(21, impl)) << id;
    EXPECT_EQ(scenario_requires_fetch(22, id), scenario_requires_fetch(21, impl)) << id;
    EXPECT_EQ(scenario_required_capability(22, id), scenario_required_capability(21, impl)) << id;
}
}  // namespace

TEST(LineageRegistry, Draft22PredicatesForwardToDraft21WithTheImplementationId) {
    for (const auto id : executable_scenarios(22)) expect_forwards(id, *implementation_scenario_id(id));
}

TEST(LineageRegistry, Draft22PredicatesForwardForSharedButUnimplementedIdsToo) {
    for (const auto id : shared_scenario_ids_22()) expect_forwards(id, *implementation_scenario_id(id));
}

TEST(LineageRegistry, SomeDraft22IdsAreTrueForTheFamilyPredicates) {
    // Guards against a vacuous forward: at least one executable id is a raw probe and one needs a track.
    bool raw = false, track = false;
    for (const auto id : executable_scenarios(22)) {
        raw = raw || raw_probe_scenario(22, id);
        track = track || scenario_requires_track(22, id);
    }
    EXPECT_TRUE(raw);
    EXPECT_TRUE(track);
}

TEST(LineageRegistry, Draft18And21RegistriesAreUnchanged) {
    EXPECT_FALSE(executable_scenarios(18).empty());
    EXPECT_FALSE(executable_scenarios(21).empty());
    for (const auto id : executable_scenarios(21)) EXPECT_FALSE(executable_scenario(22, id)) << id;
    EXPECT_FALSE(implementation_scenario_id("d21-setup-unknown-options").has_value());
}

}  // namespace
}  // namespace moq::interop::app
