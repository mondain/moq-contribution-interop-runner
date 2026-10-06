// Live draft 22 runs of the Location Filter own scenarios (D22-9-20-9-MUST-424, End Group overflow) and the
// unscored Location Filter probes: a picoquic publisher stand-in receives the runner's filter, against the
// production NativeRunManager on moqt-22.
#include "moq/interop/app/own_scenarios_22.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft22_location_filter_probes.h"
#include "support/draft22_own_live.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace moq::interop {
namespace {

using namespace live22;

constexpr std::string_view kRow = "D22-9-20-9-MUST-424";

Bytes subscribe_ok() { return b({4, 0, 2, 1, 0}); }

enum class Behaviour {
    CloseProtocolViolation,  // closes the session with PROTOCOL_VIOLATION (the row)
    CloseInternalError,      // closes the session with INTERNAL_ERROR
    Serve,                   // accepts the subscription and answers the follow-up SUBSCRIBE: keeps serving
    Deliver,                 // accepts the subscription and sends Group 0 Objects 0 and 1
    Silent,                  // says nothing
};

struct Played {
    storage::RunRecord run;
    std::vector<Bytes> written;  // the stimulus each context's publisher received
    std::vector<std::optional<bool>> verdicts;
};

// One context of the run: the stand-in connects, sends its SETUP, reads the stimulus and reacts.
void play(const std::shared_ptr<storage::SqliteRunStore>& store, const app::RunId& id, std::uint16_t port,
          unsigned ordinal, Behaviour behaviour, Played& played) {
    EXPECT_TRUE(context_ready(store, id, ordinal));
    auto client = Client::create({.port = port, .alpn = alpn_of("moqt-22")});
    EXPECT_NE(client, nullptr);
    if (!client) return;
    EXPECT_TRUE(pump_until(*client, [&] { return client->established(); }));
    EXPECT_TRUE(client->send_stream(2, setup(), false));
    EXPECT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(request_stream(0));
        // Type (one byte here), Length (16), body.
        return request && request->data.size() >= 3 &&
               request->data.size() >= 3u + (std::to_integer<std::size_t>(request->data[1]) << 8u) +
                                           std::to_integer<std::size_t>(request->data[2]);
    })) << "the stimulus arrives";
    if (const auto request = client->stream(request_stream(0))) played.written.push_back(request->data);
    switch (behaviour) {
    case Behaviour::CloseProtocolViolation:
        EXPECT_TRUE(client->close(0x3, {}));
        break;
    case Behaviour::CloseInternalError:
        EXPECT_TRUE(client->close(0x1, {}));
        break;
    case Behaviour::Serve:
        EXPECT_TRUE(client->send_stream(request_stream(0), subscribe_ok(), false));
        EXPECT_TRUE(pump_until(*client, [&] {
            const auto follow_up = client->stream(request_stream(1));
            return follow_up && !follow_up->data.empty() && follow_up->data.front() == std::byte{3};
        })) << "the runner's follow-up SUBSCRIBE";
        EXPECT_TRUE(client->send_stream(request_stream(1), b({4, 0, 2, 2, 0}), false));
        break;
    case Behaviour::Deliver:
        EXPECT_TRUE(client->send_stream(request_stream(0), subscribe_ok(), false));
        // Subgroup stream (Section 11.3): flags 0x30, alias 1, Group 0, Objects 0 and 1 (one byte each).
        EXPECT_TRUE(client->send_stream(6, b({0x30, 1, 0, 0, 1, 'x', 0, 1, 'y'}), true));
        break;
    case Behaviour::Silent:
        break;
    }
    pump_until_context_ends(*client, store, id, ordinal);
}

Played run(std::vector<std::string> scenarios, std::vector<Behaviour> behaviours) {
    // Both scenarios share the evaluator; the recorder keeps the verdicts of either, in run order.
    const VerdictRecorder recorder(scenarios::kDraft22LocationFilterOverflowEvaluator, "",
                                   scenarios::evaluate_draft22_location_filter_overflow);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, std::move(scenarios), 1500ms, app::TrackFixture{{"n"}, "t"}});
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status != app::RunStartStatus::Started) return {};
    Played played;
    for (std::size_t index = 0; index < behaviours.size(); ++index)
        play(store, started.id, started.endpoint.port, static_cast<unsigned>(index + 1), behaviours[index], played);
    played.run = store->load(started.id);
    played.verdicts = recorder.verdicts();
    EXPECT_TRUE(manager.stop(started.id));
    return played;
}

Played run_top_level(Behaviour behaviour) {
    return run({std::string(scenarios::kDraft22LocationFilterOverflow)}, {behaviour});
}

// SUBSCRIBE, Request ID 1, (n), t, LOCATION_FILTER Type 03 {2^64 - 1, 0, 1}.
const Bytes kTopLevel = b({3, 0, 0x14, 1, 1, 1, 'n', 1, 't', 1, 0x21, 3,
                           0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 1});
// SUBSCRIBE, Request ID 1, (n), t, FILL_PARAMETERS {LOCATION_FILTER Type 04 {2^64 - 1, 0, 1, 0}}.
const Bytes kFill = b({3, 0, 0x17, 1, 1, 1, 'n', 1, 't', 1, 0x23, 0x0e, 0x21, 4,
                       0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 1, 0});

Played run_both(Behaviour top_level, Behaviour fill) {
    return run({std::string(scenarios::kDraft22LocationFilterOverflow),
                std::string(scenarios::kDraft22FillLocationFilterOverflow)},
               {top_level, fill});
}

TEST(Draft22LocationFilterProbesLive, ScenarioIsImplementedAndNeedsATrack) {
    for (const auto id : {scenarios::kDraft22LocationFilterOverflow, scenarios::kDraft22FillLocationFilterOverflow}) {
        EXPECT_TRUE(app::executable_scenario(22, id)) << id;
        EXPECT_TRUE(app::raw_probe_scenario(22, id)) << id;
        EXPECT_TRUE(app::scenario_requires_track(22, id)) << id;
        EXPECT_FALSE(app::scenario_requires_fetch(22, id)) << id;
    }
}

TEST(Draft22LocationFilterProbesLive, PublisherClosingWithProtocolViolationPassesTheScenario) {
    const auto played = run_top_level(Behaviour::CloseProtocolViolation);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, std::vector<Bytes>{kTopLevel});
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true}));
    // The row also needs the FILL_PARAMETERS scenario.
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::NotRun);
}

TEST(Draft22LocationFilterProbesLive, PublisherClosingWithAnotherCodeFails) {
    const auto played = run_top_level(Behaviour::CloseInternalError);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Fail);
}

TEST(Draft22LocationFilterProbesLive, PublisherThatKeepsServingFails) {
    const auto played = run_top_level(Behaviour::Serve);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Fail);
}

TEST(Draft22LocationFilterProbesLive, PublisherRejectingBothOverflowsPassesTheRow) {
    const auto played = run_both(Behaviour::CloseProtocolViolation, Behaviour::CloseProtocolViolation);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, (std::vector<Bytes>{kTopLevel, kFill}));
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true, true}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Pass);
}

TEST(Draft22LocationFilterProbesLive, PublisherIgnoringTheNestedOverflowFailsTheRow) {
    const auto played = run_both(Behaviour::CloseProtocolViolation, Behaviour::Serve);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, (std::vector<Bytes>{kTopLevel, kFill}));
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true, false}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Fail);
}

TEST(Draft22LocationFilterProbesLive, ASilentFillContextLeavesTheRowNotRun) {
    const auto played = run_both(Behaviour::CloseProtocolViolation, Behaviour::Silent);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true, std::nullopt}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::NotRun);
}

TEST(Draft22LocationFilterProbesLive, SilentPublisherIsNotScored) {
    const auto played = run_top_level(Behaviour::Silent);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{std::nullopt}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::NotRun);
}

// ---- unscored probes ---------------------------------------------------------------------------------

// SUBSCRIBE, Request ID 1, (n), t, LOCATION_FILTER Type 06.
const Bytes kUnknownType = b({3, 0, 9, 1, 1, 1, 'n', 1, 't', 1, 0x21, 6});

// The details of the run's unscored_probe_verdict events for `scenario`.
std::vector<std::string> unscored_verdicts(const storage::RunRecord& run, std::string_view scenario) {
    std::vector<std::string> details;
    for (const auto& event : run.events)
        if (event.kind == "unscored_probe_verdict" && event.scenario_id == std::string(scenario))
            details.push_back(event.detail);
    return details;
}

// Every outcome is a catalog row's, so scoring never sees an unscored probe.
void expect_only_catalog_outcomes(const storage::RunRecord& run) {
    std::set<std::string> rows;
    const auto draft22 = catalog(22);
    for (const auto& row : draft22->requirements) rows.insert(row.id);
    EXPECT_FALSE(run.outcomes.empty());
    for (const auto& outcome : run.outcomes) EXPECT_TRUE(rows.contains(outcome.requirement_id)) << outcome.requirement_id;
}

TEST(Draft22LocationFilterProbesLive, UnknownTypeProbeIsExecutableButUnscored) {
    const auto id = scenarios::kDraft22LocationFilterUnknownType;
    EXPECT_TRUE(app::unscored_probe_22(id));
    EXPECT_TRUE(app::executable_scenario(22, id));
    EXPECT_TRUE(app::raw_probe_scenario(22, id));
    EXPECT_TRUE(app::scenario_requires_track(22, id));
}

TEST(Draft22LocationFilterProbesLive, UnknownTypeClosedWithProtocolViolationIsRecordedAsAPass) {
    const auto played = run({std::string(scenarios::kDraft22LocationFilterUnknownType)},
                            {Behaviour::CloseProtocolViolation});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, std::vector<Bytes>{kUnknownType});
    EXPECT_FALSE(harness_error(played.run));
    const auto verdicts = unscored_verdicts(played.run, scenarios::kDraft22LocationFilterUnknownType);
    ASSERT_EQ(verdicts.size(), 1u);
    EXPECT_TRUE(verdicts.front().starts_with(
        "evaluator=d22-location-filter-unknown-type-protocol-violation verdict=pass scored=false"))
        << verdicts.front();
    EXPECT_TRUE(verdicts.front().ends_with(" ordinal=1")) << "stamped with its own context";
    expect_only_catalog_outcomes(played.run);
    EXPECT_TRUE(played.verdicts.empty()) << "the row 424 evaluator is never asked about the probe";
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::NotRun);
}

TEST(Draft22LocationFilterProbesLive, UnknownTypeClosedWithAnotherCodeFailsNoRow) {
    const auto played = run({std::string(scenarios::kDraft22LocationFilterUnknownType)},
                            {Behaviour::CloseInternalError});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    const auto verdicts = unscored_verdicts(played.run, scenarios::kDraft22LocationFilterUnknownType);
    ASSERT_EQ(verdicts.size(), 1u);
    EXPECT_NE(verdicts.front().find(" verdict=fail "), std::string::npos) << verdicts.front();
    expect_only_catalog_outcomes(played.run);
    EXPECT_TRUE(std::none_of(played.run.outcomes.begin(), played.run.outcomes.end(),
                             [](const auto& outcome) { return outcome.state == requirements::OutcomeState::Fail; }))
        << "an unscored verdict never fails a row";
}

TEST(Draft22LocationFilterProbesLive, AnUnscoredProbeNextToScoredScenariosLeavesTheirRowsAlone) {
    const auto played = run({std::string(scenarios::kDraft22LocationFilterOverflow),
                             std::string(scenarios::kDraft22LocationFilterUnknownType),
                             std::string(scenarios::kDraft22FillLocationFilterOverflow)},
                            {Behaviour::CloseProtocolViolation, Behaviour::Silent, Behaviour::CloseProtocolViolation});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, (std::vector<Bytes>{kTopLevel, kUnknownType, kFill}));
    EXPECT_FALSE(harness_error(played.run));
    const auto verdicts = unscored_verdicts(played.run, scenarios::kDraft22LocationFilterUnknownType);
    ASSERT_EQ(verdicts.size(), 1u);
    EXPECT_NE(verdicts.front().find(" verdict=not_run "), std::string::npos) << verdicts.front();
    EXPECT_TRUE(verdicts.front().ends_with(" ordinal=2")) << verdicts.front();
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true, true}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Pass);
    expect_only_catalog_outcomes(played.run);
}

// SUBSCRIBE, Request ID 1, (n), t, LOCATION_FILTER Type 02 {0, 0}.
const Bytes kAbsoluteOrigin = b({3, 0, 11, 1, 1, 1, 'n', 1, 't', 1, 0x21, 2, 0, 0});

Played run_absolute_origin(Behaviour behaviour) {
    return run({std::string(scenarios::kDraft22LocationFilterAbsoluteOrigin)}, {behaviour});
}

TEST(Draft22LocationFilterProbesLive, AbsoluteOriginProbeIsExecutableButUnscored) {
    const auto id = scenarios::kDraft22LocationFilterAbsoluteOrigin;
    EXPECT_TRUE(app::unscored_probe_22(id));
    EXPECT_TRUE(app::executable_scenario(22, id));
    EXPECT_TRUE(app::raw_probe_scenario(22, id));
    EXPECT_TRUE(app::scenario_requires_track(22, id));
}

TEST(Draft22LocationFilterProbesLive, AbsoluteOriginAcceptedAndDeliveredIsRecordedAsAPass) {
    const auto played = run_absolute_origin(Behaviour::Deliver);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, std::vector<Bytes>{kAbsoluteOrigin});
    EXPECT_FALSE(harness_error(played.run));
    const auto verdicts = unscored_verdicts(played.run, scenarios::kDraft22LocationFilterAbsoluteOrigin);
    ASSERT_EQ(verdicts.size(), 1u);
    EXPECT_TRUE(verdicts.front().starts_with(
        "evaluator=d22-location-filter-absolute-origin-delivery verdict=pass scored=false"))
        << verdicts.front();
    expect_only_catalog_outcomes(played.run);
}

TEST(Draft22LocationFilterProbesLive, AbsoluteOriginReadAsMalformedIsRecordedAsAFail) {
    const auto played = run_absolute_origin(Behaviour::CloseProtocolViolation);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    const auto verdicts = unscored_verdicts(played.run, scenarios::kDraft22LocationFilterAbsoluteOrigin);
    ASSERT_EQ(verdicts.size(), 1u);
    EXPECT_NE(verdicts.front().find(" verdict=fail "), std::string::npos) << verdicts.front();
    expect_only_catalog_outcomes(played.run);
    EXPECT_TRUE(std::none_of(played.run.outcomes.begin(), played.run.outcomes.end(),
                             [](const auto& outcome) { return outcome.state == requirements::OutcomeState::Fail; }));
}

TEST(Draft22LocationFilterProbesLive, AbsoluteOriginWithoutDeliveryIsNotRun) {
    const auto played = run_absolute_origin(Behaviour::Silent);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    const auto verdicts = unscored_verdicts(played.run, scenarios::kDraft22LocationFilterAbsoluteOrigin);
    ASSERT_EQ(verdicts.size(), 1u);
    EXPECT_NE(verdicts.front().find(" verdict=not_run "), std::string::npos) << verdicts.front();
}

}  // namespace
}  // namespace moq::interop
