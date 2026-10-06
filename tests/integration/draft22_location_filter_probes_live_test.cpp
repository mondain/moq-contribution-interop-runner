// Live draft 22 runs of the Location Filter own scenarios (D22-9-20-9-MUST-424, End Group overflow): a
// picoquic publisher stand-in receives the runner's overflowing filter, against the production
// NativeRunManager on moqt-22.
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft22_location_filter_probes.h"
#include "support/draft22_own_live.h"

#include <gtest/gtest.h>

#include <optional>
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

}  // namespace
}  // namespace moq::interop
