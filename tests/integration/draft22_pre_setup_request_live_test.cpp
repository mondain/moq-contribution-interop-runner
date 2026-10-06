// Live draft 22 runs of d22-request-stream-before-peer-setup (D22-6-3-MAY-159): a picoquic publisher
// stand-in receives the runner's request before the runner's SETUP is complete, against the production
// NativeRunManager on moqt-22.
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft22_pre_setup_request.h"
#include "support/draft22_own_live.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace moq::interop {
namespace {

using namespace live22;

constexpr std::string_view kRow = "D22-6-3-MAY-159";

// SUBSCRIBE for (n)/t, Request ID 1, FORWARD=0.
Bytes subscribe() { return b({3, 0, 9, 1, 1, 1, 'n', 1, 't', 1, 0x10, 0}); }
Bytes subscribe_ok() { return b({4, 0, 2, 1, 0}); }

enum class Behaviour {
    Reset,    // resets the request stream at once (the MAY)
    Buffer,   // answers once the runner's SETUP is complete (the SHOULD)
    Eager,    // answers at once, before the runner's SETUP is complete
};

struct Played {
    storage::RunRecord run;
    Bytes control_before;  // the runner's control stream when the request arrived
    Bytes written;
    std::vector<std::optional<bool>> verdicts;
};

Played run(Behaviour behaviour) {
    const VerdictRecorder recorder(scenarios::kDraft22PreSetupResetEvaluator, scenarios::kDraft22RequestBeforeSetup,
                                   scenarios::evaluate_draft22_pre_setup_request);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {std::string(scenarios::kDraft22RequestBeforeSetup)}, 1500ms,
        app::TrackFixture{{"n"}, "t"}});
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status != app::RunStartStatus::Started) return {};
    EXPECT_TRUE(context_ready(store, started.id));
    Played played;
    auto client = Client::create({.port = started.endpoint.port, .alpn = alpn_of("moqt-22")});
    EXPECT_NE(client, nullptr);
    if (!client) return played;
    EXPECT_TRUE(pump_until(*client, [&] { return client->established(); }));
    EXPECT_TRUE(client->send_stream(2, setup(), false));
    EXPECT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(request_stream(0));
        return request && request->data.size() >= subscribe().size();
    })) << "the early request arrives";
    if (const auto control = client->stream(3)) played.control_before = control->data;
    played.written = client->stream(request_stream(0))->data;
    switch (behaviour) {
    case Behaviour::Reset:
        EXPECT_TRUE(client->reset_stream(request_stream(0), 1));
        break;
    case Behaviour::Buffer:
        EXPECT_TRUE(pump_until(*client, [&] {
            const auto control = client->stream(3);
            return control && control->data == setup();
        })) << "the runner completes its SETUP";
        EXPECT_TRUE(client->send_stream(request_stream(0), subscribe_ok(), false));
        break;
    case Behaviour::Eager:
        EXPECT_TRUE(client->send_stream(request_stream(0), subscribe_ok(), false));
        break;
    }
    pump_until_context_ends(*client, store, started.id);
    played.run = store->load(started.id);
    played.verdicts = recorder.verdicts();
    EXPECT_TRUE(manager.stop(started.id));
    return played;
}

TEST(Draft22PreSetupRequestLive, ScenarioIsImplementedAndNeedsATrack) {
    EXPECT_TRUE(app::executable_scenario(22, scenarios::kDraft22RequestBeforeSetup));
    EXPECT_TRUE(app::raw_probe_scenario(22, scenarios::kDraft22RequestBeforeSetup));
    EXPECT_TRUE(app::scenario_requires_track(22, scenarios::kDraft22RequestBeforeSetup));
    EXPECT_FALSE(app::scenario_requires_fetch(22, scenarios::kDraft22RequestBeforeSetup));
}

TEST(Draft22PreSetupRequestLive, PublisherResettingTheEarlyRequestPasses) {
    const auto played = run(Behaviour::Reset);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.control_before, b({0xaf})) << "the request arrived before the runner's SETUP was complete";
    EXPECT_EQ(played.written, subscribe());
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Pass);
}

TEST(Draft22PreSetupRequestLive, PublisherBufferingTheEarlyRequestPasses) {
    const auto played = run(Behaviour::Buffer);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.control_before, b({0xaf}));
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Pass);
}

TEST(Draft22PreSetupRequestLive, PublisherAnsweringBeforeSetupIsNotScored) {
    // Neither permitted behaviour: the request was processed before the session was established.
    const auto played = run(Behaviour::Eager);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{std::nullopt}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop
