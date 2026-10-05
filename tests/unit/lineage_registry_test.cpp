#include "moq/interop/app/lineage.h"
#include "moq/interop/app/scenario_registry.h"

#include <gtest/gtest.h>

#include <stdexcept>
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

// Pinned so a lineage change that moves executable scenarios is a visible event: 167 before D2 shared the
// 46 draft 21 LOCATION_FILTER scenarios, all of which have an executable draft 21 implementation.
TEST(LineageRegistry, ExecutableDraft22CountIsPinned) {
    EXPECT_EQ(shared_scenario_ids_22().size(), 307u);
    EXPECT_EQ(executable_scenarios(22).size(), 213u);
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

// ---- Own draft 22 scenarios (Task 8 dispatch seam) ----------------------------------------------------

constexpr std::string_view kStubOwn = "d22-request-stream-before-peer-setup";

TEST(LineageRegistryOwn, NoOwnScenarioIsImplementedInProductionYet) {
    EXPECT_TRUE(own_scenario_ids_22().empty());
    for (const auto id : requirements::lineage_data::kOwnScenarios22) {
        EXPECT_FALSE(own_scenario_22(id).has_value()) << id;
        EXPECT_FALSE(executable_scenario(22, id)) << id;
        EXPECT_FALSE(raw_probe_scenario(22, id)) << id;
        EXPECT_FALSE(implementation_scenario_id(id).has_value()) << id;
    }
}

TEST(LineageRegistryOwn, ARegisteredOwnScenarioIsExecutableOnlyWhileRegistered) {
    const auto before = executable_scenarios(22).size();
    EXPECT_FALSE(executable_scenario(22, kStubOwn));
    {
        const ScopedOwnScenario22 stub({{kStubOwn, false}, {}});
        ASSERT_EQ(own_scenario_ids_22().size(), 1u);
        EXPECT_EQ(own_scenario_ids_22().front(), kStubOwn);
        EXPECT_TRUE(executable_scenario(22, kStubOwn));
        EXPECT_TRUE(raw_probe_scenario(22, kStubOwn));
        EXPECT_FALSE(scenario_requires_track(22, kStubOwn));
        EXPECT_FALSE(scenario_requires_fetch(22, kStubOwn));
        EXPECT_FALSE(implementation_scenario_id(kStubOwn).has_value()) << "an own id has no draft 21 implementation";
        const auto ids = executable_scenarios(22);
        EXPECT_EQ(ids.size(), before + 1);
        EXPECT_EQ(ids.back(), kStubOwn) << "own ids follow the shared ones";
        // Other own ids and draft 18/21 are untouched.
        EXPECT_FALSE(executable_scenario(22, "d22-fetch-bounded-location-range"));
        EXPECT_FALSE(executable_scenario(21, kStubOwn));
        EXPECT_FALSE(executable_scenario(18, kStubOwn));
    }
    EXPECT_FALSE(executable_scenario(22, kStubOwn));
    EXPECT_TRUE(own_scenario_ids_22().empty());
    EXPECT_EQ(executable_scenarios(22).size(), before);
}

TEST(LineageRegistryOwn, TrackRequirementComesFromTheRegisteredTraits) {
    const ScopedOwnScenario22 stub({{"d22-subscribe-bounded-location-range", true}, {}});
    EXPECT_TRUE(scenario_requires_track(22, "d22-subscribe-bounded-location-range"));
}

TEST(LineageRegistryOwn, OnlyOwnIdsCanBeRegisteredAndOnlyOnce) {
    EXPECT_THROW(ScopedOwnScenario22({{"d22-location-filter-unknown-type", false}, {}}), std::invalid_argument);
    EXPECT_THROW(ScopedOwnScenario22({{"d22-duplicate-request-goaway", false}, {}}), std::invalid_argument)
        << "a shared id runs through its draft 21 implementation";
    EXPECT_THROW(ScopedOwnEvaluator22({"d22-no-such-evaluator", {}}), std::invalid_argument);
    const ScopedOwnScenario22 stub({{kStubOwn, false}, {}});
    EXPECT_THROW(ScopedOwnScenario22({{kStubOwn, false}, {}}), std::invalid_argument);
    EXPECT_TRUE(executable_scenario(22, kStubOwn)) << "a refused duplicate leaves the first registration";
}

TEST(LineageRegistryOwn, TheOwnFetchScenarioNeedsFetchWhetherOrNotItIsImplemented) {
    constexpr std::string_view fetch = "d22-fetch-bounded-location-range";
    EXPECT_TRUE(scenario_requires_fetch(22, fetch));
    EXPECT_EQ(scenario_required_capability(22, fetch), std::optional<std::string_view>("fetch"));
    const ScopedOwnScenario22 stub({{fetch, true}, {}});
    EXPECT_TRUE(scenario_requires_fetch(22, fetch));
    EXPECT_EQ(scenario_required_capability(22, fetch), std::optional<std::string_view>("fetch"));
    for (const auto id : requirements::lineage_data::kOwnScenarios22)
        if (id != fetch) EXPECT_FALSE(scenario_requires_fetch(22, id)) << id;
    // Draft 18/21 FETCH lists know nothing of it.
    EXPECT_FALSE(scenario_requires_fetch(21, fetch));
    EXPECT_FALSE(scenario_requires_fetch(18, fetch));
}

}  // namespace
}  // namespace moq::interop::app
