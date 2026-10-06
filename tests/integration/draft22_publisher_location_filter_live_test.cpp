// Live draft 22 runs of d22-publisher-location-filter-parameter (D22-9-20-9-MAY-422): a picoquic publisher
// stand-in answers the runner's SUBSCRIBE, may PUBLISH on its own request stream, and reports Location
// Filters, against the production NativeRunManager on moqt-22.
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft22_publisher_location_filter.h"
#include "support/draft22_own_live.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace moq::interop {
namespace {

using namespace live22;

constexpr std::string_view kRow = "D22-9-20-9-MAY-422";

// SUBSCRIBE for (n)/t, Request ID 1, FORWARD=1, LOCATION_FILTER Type 0x02 {7, 0}.
Bytes subscribe() { return b({3, 0, 13, 1, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 2, 7, 0}); }
Bytes subscribe_ok() { return b({4, 0, 2, 1, 0}); }
// PUBLISH_STATE_NOTIFY with LARGEST_OBJECT {7, 9} and the filter `type` {7, 0}.
Bytes notify(unsigned type) { return b({0x22, 0, 8, 2, 0x09, 7, 9, 0x18, type, 7, 0}); }
// PUBLISH of (n)/t, Request ID 0, Track Alias 2, LOCATION_FILTER Type 0x05 (Next Object).
Bytes publish() { return b({0x1d, 0, 10, 0, 1, 1, 'n', 1, 't', 2, 1, 0x21, 5}); }

struct Publisher {
    Bytes notification{notify(2)};
    bool publishes{true};
};

struct Played {
    storage::RunRecord run;
    Bytes written;           // the runner's SUBSCRIBE
    Bytes publish_response;  // what the runner answered on the publisher's PUBLISH stream
    std::vector<std::optional<bool>> verdicts;
};

Played run(const Publisher& publisher) {
    const VerdictRecorder recorder(scenarios::kDraft22PublisherLocationFilterEvaluator,
                                   scenarios::kDraft22PublisherLocationFilter,
                                   scenarios::evaluate_draft22_publisher_location_filter);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {std::string(scenarios::kDraft22PublisherLocationFilter)}, 1500ms,
        app::TrackFixture{{"n"}, "t"}});
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status != app::RunStartStatus::Started) return {};
    EXPECT_TRUE(context_ready(store, started.id));
    Played played;
    auto client = Client::create({.port = started.endpoint.port, .alpn = alpn_of("moqt-22")});
    EXPECT_NE(client, nullptr);
    if (!client) return played;
    EXPECT_TRUE(pump_until(*client, [&] {
        const auto control = client->stream(3);
        return control && control->data == setup();
    }));
    EXPECT_TRUE(client->send_stream(2, setup(), false));
    EXPECT_TRUE(pump_until(*client, [&] {
        const auto stream = client->stream(request_stream(0));
        return stream && stream->data.size() >= subscribe().size();
    })) << "the SUBSCRIBE arrives";
    if (const auto stream = client->stream(request_stream(0))) played.written = stream->data;
    EXPECT_TRUE(client->send_stream(request_stream(0), subscribe_ok(), false));
    if (publisher.publishes) {
        EXPECT_TRUE(client->send_stream(0, publish(), false));
        EXPECT_TRUE(pump_until(*client, [&] {
            const auto stream = client->stream(0);
            return stream && !stream->data.empty();
        })) << "the runner accepts the PUBLISH";
        if (const auto stream = client->stream(0)) played.publish_response = stream->data;
    }
    if (!publisher.notification.empty()) {
        EXPECT_TRUE(client->send_stream(request_stream(0), publisher.notification, false));
    }
    pump_until_context_ends(*client, store, started.id);
    played.run = store->load(started.id);
    played.verdicts = recorder.verdicts();
    EXPECT_TRUE(manager.stop(started.id));
    return played;
}

TEST(Draft22PublisherLocationFilterLive, ScenarioIsImplementedAndNeedsATrack) {
    EXPECT_TRUE(app::executable_scenario(22, scenarios::kDraft22PublisherLocationFilter));
    EXPECT_TRUE(app::raw_probe_scenario(22, scenarios::kDraft22PublisherLocationFilter));
    EXPECT_TRUE(app::scenario_requires_track(22, scenarios::kDraft22PublisherLocationFilter));
    EXPECT_FALSE(app::scenario_requires_fetch(22, scenarios::kDraft22PublisherLocationFilter));
}

TEST(Draft22PublisherLocationFilterLive, ConformingPublisherPasses) {
    const auto played = run({});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, subscribe()) << "the draft 22 Absolute Start filter goes on the wire";
    ASSERT_FALSE(played.publish_response.empty());
    EXPECT_EQ(played.publish_response.front(), std::byte{0x07}) << "REQUEST_OK (PUBLISH_OK)";
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Pass);
}

TEST(Draft22PublisherLocationFilterLive, UndefinedLocationFilterTypeFails) {
    Publisher publisher;
    publisher.notification = notify(6);
    const auto played = run(publisher);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Fail);
}

TEST(Draft22PublisherLocationFilterLive, UnrequestedFilterChangeFails) {
    // Type 0x03 {7, 0, 0} ends the subscription at Group 7, which the subscriber never asked for.
    Publisher publisher;
    publisher.notification = b({0x22, 0, 6, 1, 0x21, 3, 7, 0, 0});
    const auto played = run(publisher);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Fail);
}

TEST(Draft22PublisherLocationFilterLive, PublisherSendingNoFilterIsNotScored) {
    Publisher publisher;
    publisher.notification = {};
    publisher.publishes = false;
    const auto played = run(publisher);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{std::nullopt}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop
