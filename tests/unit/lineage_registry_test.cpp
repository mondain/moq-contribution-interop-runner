#include "moq/interop/app/lineage.h"
#include "moq/interop/app/scenario_registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace moq::interop::app {
namespace {

// Own draft 22 scenarios implemented in production (kOwnScenarioTraits22).
bool production_own(std::string_view id) {
    return std::any_of(kOwnScenarioTraits22.begin(), kOwnScenarioTraits22.end(),
                       [&](const auto& traits) { return traits.id == id; });
}

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
    // Followed by the own scenarios implemented in production, which have no draft 21 implementation.
    EXPECT_EQ(ids.size(), expected + kOwnScenarioTraits22.size());
    for (const auto id : ids) {
        if (production_own(id)) {
            EXPECT_FALSE(implementation_scenario_id(id).has_value()) << id;
            continue;
        }
        const auto implementation = implementation_scenario_id(id);
        ASSERT_TRUE(implementation.has_value()) << id;
        EXPECT_TRUE(executable_scenario(21, *implementation)) << id;
    }
}

// Pinned so a lineage change that moves executable scenarios is a visible event: 167 before D2 shared the
// 46 draft 21 LOCATION_FILTER scenarios, all of which have an executable draft 21 implementation. The own
// scenarios implemented in production follow them (D2 Tasks 9-10 add them one at a time).
TEST(LineageRegistry, ExecutableDraft22CountIsPinned) {
    EXPECT_EQ(shared_scenario_ids_22().size(), 307u);
    // Task 9a: the three row 069 scenarios; Task 9b: d22-discover-original-publisher-namespaces (row 110),
    // d22-publisher-location-filter-parameter (row 422) and d22-request-stream-before-peer-setup (row 159);
    // Task 10: d22-location-filter-end-group-overflow and d22-fill-location-filter-end-group-overflow (row 424).
    EXPECT_EQ(kOwnScenarioTraits22.size(), 8u);
    EXPECT_EQ(executable_scenarios(22).size(), 213u + kOwnScenarioTraits22.size());
}

TEST(LineageRegistry, OwnAndUnknownIdsAreNotExecutableForDraft22) {
    // d22-location-filter-unknown-type was listed here until Task 10 made it an executable unscored probe.
    for (const char* id : {"", "no-such-scenario", "d21-setup-unknown-options", "d22-no-such-scenario",
                           "d22-location-filter-no-such-probe"}) {
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
    for (const auto id : executable_scenarios(22))
        if (!production_own(id)) expect_forwards(id, *implementation_scenario_id(id));
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

// The own id the overlay tests register a stub for. Since Task 10 every own id has a production
// implementation, so the stub shadows one (an overlay entry wins over a production entry with the same id).
constexpr std::string_view kStubOwn = "d22-fill-location-filter-end-group-overflow";

// Task 10 implemented the last own scenarios: every lineage-own draft 22 scenario now has a production
// implementation (its probe and evaluator tables are checked in raw_family_driver_test.cpp).
TEST(LineageRegistryOwn, EveryOwnScenarioHasAProductionImplementation) {
    ASSERT_EQ(requirements::lineage_data::kOwnScenarios22.size(), kOwnScenarioTraits22.size());
    for (const auto id : requirements::lineage_data::kOwnScenarios22) EXPECT_TRUE(production_own(id)) << id;
}

TEST(LineageRegistryOwn, OnlyTheProductionOwnScenariosAreImplemented) {
    EXPECT_EQ(own_scenario_ids_22().size(), kOwnScenarioTraits22.size());
    for (const auto id : requirements::lineage_data::kOwnScenarios22) {
        const bool production = production_own(id);
        EXPECT_EQ(own_scenario_22(id).has_value(), production) << id;
        EXPECT_EQ(executable_scenario(22, id), production) << id;
        EXPECT_EQ(raw_probe_scenario(22, id), production) << id;
        EXPECT_FALSE(implementation_scenario_id(id).has_value()) << id;
    }
}

// Was "ARegisteredOwnScenarioIsExecutableOnlyWhileRegistered" while an own id lacked an implementation. Now
// the stub shadows a production one: its traits apply only while registered, and the id is listed once.
TEST(LineageRegistryOwn, ARegisteredOwnScenarioShadowsTheProductionOneOnlyWhileRegistered) {
    const auto before = executable_scenarios(22).size();
    const auto own_before = own_scenario_ids_22().size();
    ASSERT_TRUE(production_own(kStubOwn));
    EXPECT_TRUE(scenario_requires_track(22, kStubOwn));
    {
        const ScopedOwnScenario22 stub({{kStubOwn, false}, {}});
        EXPECT_EQ(own_scenario_ids_22().size(), own_before) << "listed once";
        const auto own = own_scenario_ids_22();
        EXPECT_EQ(std::count(own.begin(), own.end(), kStubOwn), 1);
        EXPECT_TRUE(executable_scenario(22, kStubOwn));
        EXPECT_TRUE(raw_probe_scenario(22, kStubOwn));
        EXPECT_FALSE(scenario_requires_track(22, kStubOwn)) << "the overlay's traits win";
        EXPECT_FALSE(scenario_requires_fetch(22, kStubOwn));
        EXPECT_FALSE(implementation_scenario_id(kStubOwn).has_value()) << "an own id has no draft 21 implementation";
        const auto ids = executable_scenarios(22);
        EXPECT_EQ(ids.size(), before);
        const auto shared = shared_scenario_ids_22();
        const auto first_own = std::find_if(ids.begin(), ids.end(), [&](std::string_view id) {
            return std::find(shared.begin(), shared.end(), id) == shared.end();
        });
        EXPECT_NE(std::find(first_own, ids.end(), kStubOwn), ids.end()) << "own ids follow the shared ones";
        EXPECT_TRUE(std::all_of(first_own, ids.end(), [](std::string_view id) { return own_scenario_22(id).has_value(); }));
        // Other own ids and draft 18/21 are untouched.
        for (const auto id : requirements::lineage_data::kOwnScenarios22)
            if (id != kStubOwn) EXPECT_EQ(executable_scenario(22, id), production_own(id)) << id;
        EXPECT_FALSE(executable_scenario(21, kStubOwn));
        EXPECT_FALSE(executable_scenario(18, kStubOwn));
    }
    EXPECT_TRUE(scenario_requires_track(22, kStubOwn)) << "the production traits again";
    EXPECT_EQ(own_scenario_ids_22().size(), own_before);
    EXPECT_EQ(executable_scenarios(22).size(), before);
}

TEST(LineageRegistryOwn, TrackRequirementComesFromTheRegisteredTraits) {
    const ScopedOwnScenario22 stub({{"d22-subscribe-bounded-location-range", true}, {}});
    EXPECT_TRUE(scenario_requires_track(22, "d22-subscribe-bounded-location-range"));
}

TEST(LineageRegistryOwn, OnlyOwnIdsCanBeRegisteredAndOnlyOnce) {
    // An unscored probe is not an own scenario either: the overlay refuses it.
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

// ---- Unscored draft 22 probes (Task 10) ----------------------------------------------------------------

TEST(LineageRegistryUnscored, ProbesAreExecutableByIdButAreNeitherCatalogNorLineageScenarios) {
    ASSERT_FALSE(kUnscoredProbeTraits22.empty());
    EXPECT_EQ(kUnscoredEvaluators22.size(), kUnscoredProbeTraits22.size()) << "one evaluator per probe";
    const auto& own = requirements::lineage_data::kOwnScenarios22;
    const auto& own_evaluators = requirements::lineage_data::kOwnEvaluators22;
    const auto listed = executable_scenarios(22);
    const auto shared = shared_scenario_ids_22();
    for (const auto& traits : kUnscoredProbeTraits22) {
        const auto id = traits.id;
        EXPECT_TRUE(id.starts_with("d22-")) << id;
        EXPECT_TRUE(unscored_probe_22(id)) << id;
        EXPECT_TRUE(executable_scenario(22, id)) << id;
        EXPECT_TRUE(raw_probe_scenario(22, id)) << id;
        EXPECT_EQ(scenario_requires_track(22, id), traits.requires_track) << id;
        EXPECT_FALSE(scenario_requires_fetch(22, id)) << id;
        EXPECT_FALSE(implementation_scenario_id(id).has_value()) << id;
        EXPECT_EQ(std::find(own.begin(), own.end(), id), own.end()) << id << " is a lineage own scenario";
        EXPECT_EQ(std::find(shared.begin(), shared.end(), id), shared.end()) << id << " is a shared scenario";
        EXPECT_EQ(std::find(listed.begin(), listed.end(), id), listed.end())
            << id << ": executable_scenarios(22) lists catalog scenarios only";
        EXPECT_FALSE(executable_scenario(21, id)) << id;
        EXPECT_FALSE(executable_scenario(18, id)) << id;
    }
    for (const auto id : kUnscoredEvaluators22)
        EXPECT_EQ(std::find(own_evaluators.begin(), own_evaluators.end(), id), own_evaluators.end()) << id;
    // They do not count among the implemented own scenarios.
    for (const auto id : own_scenario_ids_22()) EXPECT_FALSE(unscored_probe_22(id)) << id;
    EXPECT_FALSE(unscored_probe_22("d22-location-filter-end-group-overflow"));
}

}  // namespace
}  // namespace moq::interop::app
