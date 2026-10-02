// Enforces that the `requires_fetch` tag in the scenario registry is honest: it must name
// exactly the executable scenarios whose stimulus sends a FETCH message, decoded with the
// project's own wire code, plus the few scenarios whose stimulus cannot be decoded
// statically and are pinned below with the reason.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/publisher_capabilities.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/draft18.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft18/messages.h"
#include "moq/interop/wire/draft21/message_types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <variant>

namespace moq::interop::app {
namespace {

using scenarios::RawProbeChannel;
using scenarios::RawProbeDefinition;

// Writes whose bytes are produced at run time from the publisher's own replies
// (RawProbeWrite::prepare_bytes), so a static decode sees nothing. Each one is pinned
// with whether its stimulus is a FETCH, from reading the closure that builds it. A new
// dynamic scenario fails the test until it is added here, so it cannot be missed.
const std::map<std::string, bool> kDynamicDraft18{
    // standalone FETCH starting past the Largest Object that the SUBSCRIBE_OK reported
    {"receive-fetch-start-beyond-largest-published-object", true},
    // standalone FETCH for the first datagram Object that was observed
    {"fetch-object-previously-observed-as-datagram", true},
    // REQUEST_UPDATE, SUBSCRIBE, TRACK_STATUS, GOAWAY or SUBSCRIBE built from replies
    {"publisher-control-goaway-with-pending-request-at-cutoff", false},
    {"advance-start-location-while-subgroup-remains-incomplete", false},
    {"redeliver-previously-observed-object-in-later-subscription", false},
    {"register-delete-then-use-token-alias", false},
    {"register-valid-token-then-use-alias-in-later-request", false},
    {"register-token-in-rejected-request-then-use-alias", false},
    {"receive-subscribe-before-outstanding-publish-response", false},
    {"publish-objects-before-within-and-after-subscription-range", false},
    {"receive-control-goaway-with-new-session-uri", false},
};
const std::map<std::string, bool> kDynamicDraft21{
    // SUBSCRIBE or REQUEST_UPDATE built from the publisher's SETUP or Objects
    {"d21-range-filter-total-exceeds-negotiated-limit", false},
    {"d21-range-filter-with-zero-negotiated-limit", false},
    {"d21-range-filter-total-limit", false},
    {"d21-range-filter-default-zero-limit", false},
    {"d21-range-filter-update-total-limit", false},
    {"d21-request-update-overrun", false},
    {"d21-request-update-independent-streams", false},
    {"d21-filter-mutable-property", false},
    {"d21-filter-immutable-property", false},
    {"d21-forward-location-and-range-filter-conjunction", false},
};

// Scenarios without a raw-probe definition are typed controllers. Draft 18 has five; the
// draft 21 announcement and setup controllers only answer PUBLISH_NAMESPACE, PUBLISH and
// SETUP, so none sends a FETCH. The draft 18 ones are decoded through their step actions.
const std::set<std::string> kDraft18TypedControllers{
    "subscribe-to-publisher-track", "subscribe-again-to-established-publisher-track",
    "fetch-publisher-track-range", "subscribe-namespace-at-publisher",
    "subscribe-tracks-at-publisher"};

// Walks the length-framed messages in `bytes` (type varint, 16-bit length, body) and
// returns the message types. A truncated or malformed tail ends the walk: a probe may
// deliberately send a broken message, but the type of the frame is still readable.
std::vector<std::uint64_t> frame_types(std::span<const std::byte> bytes) {
    std::vector<std::uint64_t> types;
    wire::Cursor cursor(bytes);
    while (cursor.remaining() != 0) {
        const auto type = wire::read_vi64(cursor);
        const auto* value = std::get_if<std::uint64_t>(&type);
        if (!value) break;
        types.push_back(*value);
        const auto length = wire::read_bytes(cursor, 2);
        const auto* prefix = std::get_if<std::span<const std::byte>>(&length);
        if (!prefix) break;
        const auto size = (std::to_integer<std::size_t>((*prefix)[0]) << 8u) |
                          std::to_integer<std::size_t>((*prefix)[1]);
        if (!std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor, size))) break;
    }
    return types;
}

std::uint64_t draft18_fetch_type() {
    wire::draft18::FetchMessage message{
        1, wire::draft18::StandaloneFetch{{}, {}, {0, 0}, {0, 1}}, {}};
    wire::ByteWriter out(256);
    EXPECT_TRUE(wire::draft18::encode_message(message, out).has_value());
    wire::Cursor cursor(out.bytes());
    return std::get<std::uint64_t>(wire::read_vi64(cursor));
}

bool draft21_is_fetch(std::uint64_t type) {
    const auto info = wire::draft21::classify_message_type(type);
    return info && info->kind == wire::draft21::MessageKind::Fetch;
}

bool is_fetch(unsigned draft, std::uint64_t type) {
    return draft == 18 ? type == draft18_fetch_type() : draft21_is_fetch(type);
}

RunConfig run_config(unsigned draft, std::string_view id) {
    return RunConfig{draft == 18 ? DraftVersion::Draft18 : DraftVersion::Draft21,
                     TransportKind::NativeQuic, RunMode::Observed, {std::string(id)},
                     std::chrono::milliseconds(1000), TrackFixture{{"moq", "test"}, "video"}};
}

struct Stimulus {
    bool fetch{false};
    bool dynamic{false};
};

Stimulus inspect(unsigned draft, const RawProbeDefinition& definition) {
    Stimulus stimulus;
    for (const auto& write : definition.writes) {
        // Unidirectional and datagram writes carry Objects, never request messages.
        if (write.channel == RawProbeChannel::Datagram || write.channel == RawProbeChannel::NewUni ||
            write.channel == RawProbeChannel::Credit || write.channel == RawProbeChannel::UniCredit ||
            write.channel == RawProbeChannel::DropInbound || write.channel == RawProbeChannel::ResumeInbound)
            continue;
        if (write.bytes.empty() && write.prepare_bytes) stimulus.dynamic = true;
        for (const auto type : frame_types(write.bytes))
            if (is_fetch(draft, type)) stimulus.fetch = true;
    }
    return stimulus;
}

// A draft 18 typed controller is a ScenarioDefinition; read its step actions.
bool draft18_typed_sends_fetch(const std::string& id) {
    const std::vector<std::byte> ns_field{std::byte{'a'}};
    wire::draft18::TrackNamespace name_space{{ns_field}};
    wire::draft18::TrackName track{{std::byte{'t'}}};
    const auto ms = std::chrono::milliseconds(100);
    scenarios::ScenarioDefinition definition;
    if (id == "subscribe-to-publisher-track")
        definition = scenarios::subscribe_to_publisher_track(name_space, track, 1, ms, ms);
    else if (id == "subscribe-again-to-established-publisher-track")
        definition = scenarios::subscribe_again_to_established_publisher_track(name_space, track, 1, 3, ms, ms);
    else if (id == "fetch-publisher-track-range")
        definition = scenarios::fetch_publisher_track_range(name_space, track, 1, {0, 0}, {0, 1}, ms, ms);
    else if (id == "subscribe-namespace-at-publisher")
        definition = scenarios::subscribe_namespace_at_publisher(name_space, 1, ms, ms);
    else
        definition = scenarios::subscribe_tracks_at_publisher(name_space, 1, ms, ms);
    for (const auto& step : definition.steps)
        for (const auto& action : step.actions)
            if (const auto* open = std::get_if<scenarios::OpenRequestAction>(&action))
                if (std::holds_alternative<wire::draft18::FetchMessage>(open->message)) return true;
    return false;
}

TEST(PublisherCapabilityTagging, WireDecodersAgreeOnTheFetchMessageType) {
    EXPECT_EQ(draft18_fetch_type(), 0x16u);
    EXPECT_TRUE(draft21_is_fetch(0x16));
    EXPECT_FALSE(draft21_is_fetch(0x3));
}

TEST(PublisherCapabilityTagging, FrameWalkerFindsFetchAfterOtherMessagesAndInBrokenFrames) {
    const std::vector<std::byte> two{std::byte{0x3}, std::byte{0}, std::byte{0}, std::byte{0x16},
                                     std::byte{0}, std::byte{1}, std::byte{0}};
    EXPECT_EQ(frame_types(two), (std::vector<std::uint64_t>{0x3, 0x16}));
    const std::vector<std::byte> truncated{std::byte{0x16}, std::byte{0}, std::byte{9}, std::byte{0}};
    EXPECT_EQ(frame_types(truncated), (std::vector<std::uint64_t>{0x16}));
}

TEST(PublisherCapabilityTagging, TagsAreOnlyOnExecutableScenarios) {
    for (const auto id : kDraft18FetchScenarios) EXPECT_TRUE(executable_scenario(18, id)) << id;
    for (const auto id : kDraft21FetchScenarios) EXPECT_TRUE(executable_scenario(21, id)) << id;
    const std::set<std::string_view> unique18(kDraft18FetchScenarios.begin(), kDraft18FetchScenarios.end());
    const std::set<std::string_view> unique21(kDraft21FetchScenarios.begin(), kDraft21FetchScenarios.end());
    EXPECT_EQ(unique18.size(), kDraft18FetchScenarios.size());
    EXPECT_EQ(unique21.size(), kDraft21FetchScenarios.size());
    EXPECT_FALSE(scenario_requires_fetch(16, "fetch-publisher-track-range"));
    EXPECT_FALSE(scenario_requires_fetch(21, "fetch-publisher-track-range"));
}

TEST(PublisherCapabilityTagging, RequiresFetchMatchesEveryDecodedStimulus) {
    std::size_t decoded_fetch[2]{0, 0};
    for (const unsigned draft : {18u, 21u}) {
        const auto& dynamic = draft == 18 ? kDynamicDraft18 : kDynamicDraft21;
        std::set<std::string> seen_dynamic;
        std::set<std::string> typed;
        for (const auto id : executable_scenarios(draft)) {
            const std::string name(id);
            bool expect_fetch = false;
            if (!raw_probe_scenario(draft, id)) {
                typed.insert(name);
                if (draft == 18) {
                    ASSERT_TRUE(kDraft18TypedControllers.contains(name)) << name;
                    EXPECT_EQ(draft18_typed_sends_fetch(name), name == "fetch-publisher-track-range") << name;
                    expect_fetch = draft18_typed_sends_fetch(name);
                }
            } else {
                const auto definition = NativeRunManager::resolve_probe({}, run_config(draft, id), id);
                ASSERT_TRUE(definition.has_value()) << name;
                const auto stimulus = inspect(draft, *definition);
                expect_fetch = stimulus.fetch;
                if (stimulus.dynamic) {
                    seen_dynamic.insert(name);
                    ASSERT_TRUE(dynamic.contains(name))
                        << name << " builds a write at run time; pin whether it is a FETCH in the dynamic table";
                    expect_fetch = expect_fetch || dynamic.at(name);
                }
                if (stimulus.fetch) ++decoded_fetch[draft == 18 ? 0 : 1];
            }
            EXPECT_EQ(scenario_requires_fetch(draft, id), expect_fetch)
                << "draft " << draft << " " << name
                << (expect_fetch ? " sends a FETCH but is not tagged requires_fetch"
                                 : " is tagged requires_fetch but sends no FETCH");
        }
        // The pinned table must not outlive the scenarios it names.
        for (const auto& [name, fetch] : dynamic) {
            EXPECT_TRUE(seen_dynamic.contains(name)) << name << " is pinned but no longer builds a dynamic write";
            (void)fetch;
        }
        if (draft == 21) {
            // Draft 21 typed controllers (setup and announcement) never send a FETCH.
            for (const auto& name : typed) EXPECT_FALSE(scenario_requires_fetch(21, name)) << name;
        }
    }
    // The detection must see the large majority itself (guards against a decoder that
    // silently finds nothing and a tag list that then happens to agree).
    EXPECT_GE(decoded_fetch[0], 18u);
    EXPECT_GE(decoded_fetch[1], 20u);
}

requirements::Requirement row(std::string id, requirements::Strength strength, std::vector<std::string> scenarios) {
    return {std::move(id), strength, {"1", 1, 1, 1, 1}, "publisher", "behavior",
            requirements::Applicability::Applicable, requirements::Testability::Testable,
            std::move(scenarios), {"evaluator"}, "reason"};
}

std::vector<requirements::Outcome> all_not_run(const requirements::RequirementCatalog& catalog) {
    std::vector<requirements::Outcome> outcomes;
    for (const auto& requirement : catalog.requirements)
        outcomes.push_back({requirement.id, requirements::OutcomeState::NotRun});
    return outcomes;
}

TEST(PublisherCapabilityScoring, OnlyRowsWhoseEveryScenarioNeedsFetchAreNotApplicable) {
    using requirements::OutcomeState;
    const requirements::RequirementCatalog catalog{18, "synthetic", true,
        {row("all-fetch", requirements::Strength::Must,
             {"fetch-publisher-track-range", "receive-fetch-with-unknown-type"}),
         row("mixed", requirements::Strength::Must,
             {"fetch-publisher-track-range", "subscribe-to-publisher-track"}),
         row("no-fetch", requirements::Strength::Should, {"subscribe-to-publisher-track"}),
         row("unnamed", requirements::Strength::May, {})}};
    auto outcomes = all_not_run(catalog);
    apply_publisher_capabilities(18, catalog, PublisherCapabilities{.fetch = false}, outcomes);
    EXPECT_EQ(outcomes[0].state, OutcomeState::NotApplicable);
    EXPECT_EQ(outcomes[1].state, OutcomeState::NotRun) << "a mixed row keeps needing every scenario";
    EXPECT_EQ(outcomes[2].state, OutcomeState::NotRun);
    EXPECT_EQ(outcomes[3].state, OutcomeState::NotRun);

    // A capable publisher changes nothing; evidence is never overwritten.
    auto capable = all_not_run(catalog);
    apply_publisher_capabilities(18, catalog, PublisherCapabilities{}, capable);
    for (const auto& outcome : capable) EXPECT_EQ(outcome.state, OutcomeState::NotRun);
    auto observed = all_not_run(catalog);
    observed[0].state = OutcomeState::Pass;
    apply_publisher_capabilities(18, catalog, PublisherCapabilities{.fetch = false}, observed);
    EXPECT_EQ(observed[0].state, OutcomeState::Pass);

    EXPECT_NE(row_not_applicable_reason(18, catalog.requirements[0], {.fetch = false})
                  .value_or("").find("publisher declared no FETCH support"), std::string::npos);
    EXPECT_FALSE(row_not_applicable_reason(18, catalog.requirements[1], {.fetch = false}));
    // Draft 21 ids are not draft 18 scenarios and the other way round.
    EXPECT_FALSE(row_not_applicable_reason(21, catalog.requirements[0], {.fetch = false}));
}

TEST(PublisherCapabilityScoring, RealCatalogsLeaveTheDenominatorsByExactlyTheExcludedRows) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    for (const unsigned draft : {18u, 21u}) {
        const auto source = requirements::load_draft_source(
            draft, root / "docs", root / "requirements" / "draft-digests.json");
        const auto catalog = requirements::RequirementCatalog::load(
            source, root / "requirements" / ("draft" + std::to_string(draft) + ".json"));
        const auto baseline = requirements::score(catalog, all_not_run(catalog));
        // Rows with no scenario or non-scored rows are not NotRun-able the same way.
        std::vector<requirements::Outcome> outcomes;
        for (const auto& requirement : catalog.requirements) {
            const bool scored = requirement.applicability == requirements::Applicability::Applicable &&
                                requirement.testability == requirements::Testability::Testable;
            outcomes.push_back({requirement.id,
                scored ? requirements::OutcomeState::NotRun
                       : requirement.applicability == requirements::Applicability::Applicable
                           ? requirements::OutcomeState::NotTestable
                           : requirements::OutcomeState::NotApplicable});
        }
        const auto before = requirements::score(catalog, outcomes);
        auto adjusted = outcomes;
        apply_publisher_capabilities(draft, catalog, {.fetch = false}, adjusted);
        const auto after = requirements::score(catalog, adjusted);
        (void)baseline;

        std::uint64_t required = 0, weighted = 0;
        std::size_t excluded = 0, mixed = 0;
        for (const auto& requirement : catalog.requirements) {
            if (requirement.scenarios.empty()) continue;
            if (row_not_applicable_reason(draft, requirement, {.fetch = false})) {
                ++excluded;
                weighted += requirements::score_weight(requirement.strength);
                if (requirement.strength == requirements::Strength::Must ||
                    requirement.strength == requirements::Strength::MustNot)
                    required += requirements::score_weight(requirement.strength);
            } else if (std::any_of(requirement.scenarios.begin(), requirement.scenarios.end(),
                                   [&](const auto& id) { return scenario_requires_fetch(draft, id); })) {
                ++mixed;
            }
        }
        EXPECT_GE(excluded, 20u) << "draft " << draft;
        EXPECT_EQ(before.required.possible - after.required.possible, required);
        EXPECT_EQ(before.weighted.possible - after.weighted.possible, weighted);
        EXPECT_EQ(before.coverage.possible - after.coverage.possible, weighted);
        EXPECT_EQ(after.required.earned, 0u);
        EXPECT_NE(after.verdict, requirements::RunVerdict::Error);
        EXPECT_NE(after.verdict, requirements::RunVerdict::Fail);
        // Mixed rows stay in the denominators (draft 21 has one; draft 18 has none).
        if (draft == 21) EXPECT_GE(mixed, 1u);
    }
}

}  // namespace
}  // namespace moq::interop::app
