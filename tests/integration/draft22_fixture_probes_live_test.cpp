// Live draft 22 runs of the draft 21 family probes a draft 22 run shares, against a picoquic publisher
// stand-in that behaves like a single-track contribution publisher (moqxr): it refuses any namespace or track
// but the run's own, and after accepting a SUBSCRIBE_TRACKS it sends a PUBLISH and reads nothing more until
// that PUBLISH is answered, closing with NO_ERROR when it is not. The probes must reach their stimulus.
#include "support/draft22_own_live.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace moq::interop {
namespace {

using namespace live22;

// The run's track fixture is (n)/t.
Bytes subscribe() { return b({3, 0, 7, 1, 1, 1, 'n', 1, 't', 0}); }
Bytes subscribe_namespace() { return b({0x50, 0, 5, 1, 1, 1, 'n', 0}); }
Bytes subscribe_tracks_n() { return b({0x51, 0, 5, 3, 1, 1, 'n', 0}); }
// SUBSCRIBE_TRACKS with the empty prefix, Request ID 1, no parameters.
Bytes subscribe_tracks_empty() { return b({0x51, 0, 3, 1, 0, 0}); }
Bytes barrier() { return b({0x50, 0, 5, 5, 1, 1, 'c', 0}); }
// REQUEST_UPDATE with an AUTHORIZATION TOKEN USE_ALIAS 0 (unregistered), and GOAWAY with no URI.
Bytes failing_update() { return b({2, 0, 6, 3, 1, 3, 2, 2, 0}); }
Bytes goaway() { return b({0x10, 0, 3, 0, 0xa7, 0x10}); }

Bytes subscribe_ok() { return b({4, 0, 2, 1, 0}); }
Bytes request_ok() { return b({7, 0, 1, 0}); }
Bytes request_error() { return b({5, 0, 3, 1, 0, 0}); }
// DOES_NOT_EXIST, as moqxr refuses a name it does not serve.
Bytes does_not_exist() { return b({5, 0, 3, 0x10, 0, 0}); }
Bytes update_failed_done() { return b({0x0b, 0, 3, 8, 0, 0}); }
// PUBLISH of (n)/t on the publisher's own request stream: Request ID 0, Track Alias 1, no parameters.
Bytes publish() { return b({0x1d, 0, 8, 0, 1, 1, 'n', 1, 't', 1, 0}); }

Bytes concat(Bytes left, const Bytes& right) {
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

std::unique_ptr<Client> connect(const std::shared_ptr<storage::SqliteRunStore>& store,
                                const app::RunStartResult& started, unsigned ordinal) {
    EXPECT_TRUE(context_ready(store, started.id, ordinal));
    auto client = Client::create({.port = started.endpoint.port, .alpn = alpn_of("moqt-22")});
    EXPECT_NE(client, nullptr);
    if (!client) return client;
    EXPECT_TRUE(pump_until(*client, [&] {
        const auto control = client->stream(3);
        return control && control->data == setup();
    }));
    EXPECT_TRUE(client->send_stream(2, setup(), false));
    return client;
}

// The first complete message the runner sent on `stream`, once it arrived.
std::optional<Bytes> first_message(Client& client, std::uint64_t stream) {
    std::optional<Bytes> result;
    (void)pump_until(client, [&] {
        const auto observed = client.stream(stream);
        if (!observed || observed->data.size() < 3) return false;
        const auto size = 3 + ((std::to_integer<std::size_t>(observed->data[1]) << 8u) |
                               std::to_integer<std::size_t>(observed->data[2]));
        if (observed->data.size() < size) return false;
        result = Bytes(observed->data.begin(), observed->data.begin() + static_cast<std::ptrdiff_t>(size));
        return true;
    });
    return result;
}

// Answers a request for the run's name with `accepted`; refuses anything else as moqxr does.
bool answer(Client& client, std::uint64_t stream, const Bytes& expected, const Bytes& accepted) {
    const auto request = first_message(client, stream);
    EXPECT_EQ(request, expected) << "the runner names the run's fixture on stream " << stream;
    if (!request) return false;
    if (*request != expected) {
        EXPECT_TRUE(client.send_stream(stream, does_not_exist(), true));
        return false;
    }
    EXPECT_TRUE(client.send_stream(stream, accepted, false));
    return true;
}

// Whether `stream` carried `expected` after its first `offset` bytes (and a FIN, when `fin`).
bool follows(Client& client, std::uint64_t stream, std::size_t offset, const Bytes& expected, bool fin = false) {
    return pump_until(client, [&] {
        const auto observed = client.stream(stream);
        return observed && observed->data.size() >= offset + expected.size() &&
               Bytes(observed->data.begin() + static_cast<std::ptrdiff_t>(offset),
                     observed->data.begin() + static_cast<std::ptrdiff_t>(offset + expected.size())) == expected &&
               (!fin || observed->fin);
    }, 3s);
}

// A publisher that sends PUBLISH and blocks on its answer: true once the runner answered it with REQUEST_OK.
bool publish_answered(Client& client) {
    EXPECT_TRUE(client.send_stream(0, publish(), false));
    const bool answered = pump_until(client, [&] {
        const auto stream = client.stream(0);
        return stream && stream->data.size() >= request_ok().size();
    }, 1s);
    if (!answered) {
        // moqxr: "timed out waiting for stream data", then a NO_ERROR close.
        EXPECT_TRUE(client.close(0, {}));
        return false;
    }
    const auto stream = client.stream(0);
    EXPECT_EQ(stream->data, request_ok()) << "PUBLISH_OK";
    return true;
}

storage::RunRecord finish(app::NativeRunManager& manager, const std::shared_ptr<storage::SqliteRunStore>& store,
                          const app::RunId& id) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (store->load(id).state != storage::RunState::Finalized && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    auto run = store->load(id);
    EXPECT_TRUE(manager.stop(id));
    return run;
}

TEST(Draft22FixtureProbesLive, SubscriptionCleanupSubscribesToTheRunsTrackAndReachesItsUpdate) {
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"d22-failed-subscription-update-cleanup"}, 1500ms, app::TrackFixture{{"n"}, "t"}});
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = connect(store, started, 1);
    ASSERT_NE(client, nullptr);
    if (answer(*client, request_stream(0), subscribe(), subscribe_ok())) {
        EXPECT_TRUE(follows(*client, request_stream(0), subscribe().size(), failing_update(), true))
            << "the REQUEST_UPDATE reaches the publisher";
        EXPECT_TRUE(client->send_stream(request_stream(0), concat(request_error(), update_failed_done()), true));
    }
    pump_until_context_ends(*client, store, started.id);
    const auto run = finish(manager, store, started.id);
    EXPECT_FALSE(harness_error(run));
    EXPECT_EQ(state_of(run, "D22-9-5-1-MUST-357"), requirements::OutcomeState::Pass);
}

TEST(Draft22FixtureProbesLive, GoawayProbesNameTheRunsNamespaceAndReachTheirGoaways) {
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"d22-duplicate-request-goaway", "d22-goaway-on-distinct-request-streams"}, 1500ms,
        app::TrackFixture{{"n"}, "t"}});
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    {
        SCOPED_TRACE("d22-duplicate-request-goaway");
        auto client = connect(store, started, 1);
        ASSERT_NE(client, nullptr);
        if (answer(*client, request_stream(0), subscribe_namespace(), request_ok())) {
            EXPECT_TRUE(follows(*client, request_stream(0), subscribe_namespace().size(), concat(goaway(), goaway())))
                << "both GOAWAYs reach the publisher on one request stream";
            EXPECT_TRUE(client->close(3, {}));  // PROTOCOL_VIOLATION
        }
        pump_until_context_ends(*client, store, started.id, 1);
    }
    {
        SCOPED_TRACE("d22-goaway-on-distinct-request-streams");
        auto client = connect(store, started, 2);
        ASSERT_NE(client, nullptr);
        if (answer(*client, request_stream(0), subscribe_namespace(), request_ok()) &&
            answer(*client, request_stream(1), subscribe_tracks_n(), request_ok()) && publish_answered(*client)) {
            EXPECT_TRUE(follows(*client, request_stream(0), subscribe_namespace().size(), goaway()));
            EXPECT_TRUE(follows(*client, request_stream(1), subscribe_tracks_n().size(), goaway()));
            EXPECT_TRUE(follows(*client, request_stream(2), 0, barrier())) << "the barrier request arrives";
            EXPECT_TRUE(client->send_stream(request_stream(2), does_not_exist(), true));
        }
        pump_until_context_ends(*client, store, started.id, 2);
    }
    const auto run = finish(manager, store, started.id);
    EXPECT_FALSE(harness_error(run));
    EXPECT_EQ(state_of(run, "D22-9-2-MUST-339"), requirements::OutcomeState::Pass);
}

TEST(Draft22FixtureProbesLive, SubscribeTracksUpdateCloseAnswersThePublishAndReachesItsUpdate) {
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"d22-failed-subscribe-tracks-update-close"}, 1500ms, app::TrackFixture{{"n"}, "t"}});
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = connect(store, started, 1);
    ASSERT_NE(client, nullptr);
    if (answer(*client, request_stream(0), subscribe_tracks_empty(), request_ok()) && publish_answered(*client)) {
        EXPECT_TRUE(follows(*client, request_stream(0), subscribe_tracks_empty().size(), failing_update(), true))
            << "the REQUEST_UPDATE reaches the publisher";
        EXPECT_TRUE(client->send_stream(request_stream(0), request_error(), true));
    }
    pump_until_context_ends(*client, store, started.id);
    const auto run = finish(manager, store, started.id);
    EXPECT_FALSE(harness_error(run));
    EXPECT_EQ(state_of(run, "D22-9-5-1-MUST-360"), requirements::OutcomeState::Pass);
}

// --- SUBSCRIBE / TRACK_STATUS probes that named (), "x" --------------------------------------------------

// A request's Track Namespace field count, read after its type, length and one-byte Request ID.
std::optional<std::uint64_t> namespace_fields(const Bytes& request) {
    if (request.size() < 5) return std::nullopt;
    return std::to_integer<std::uint64_t>(request[4]);
}

// Plays a publisher that, like imquic, closes 0x3 (PROTOCOL_VIOLATION) on a SUBSCRIBE or TRACK_STATUS with
// no namespace field before it reads the rest. Returns the first request the runner sent, once it named
// the run's track (n)/t; the caller then plays the probe's real condition.
std::optional<Bytes> named_request(Client& client) {
    const auto request = first_message(client, request_stream(0));
    EXPECT_TRUE(request.has_value());
    if (!request) return std::nullopt;
    if (namespace_fields(*request) == 0u) {
        EXPECT_TRUE(client.close(3, {})) << "Invalid number of namespaces";
        ADD_FAILURE() << "the runner sent an empty namespace";
        return std::nullopt;
    }
    return request;
}

// SUBSCRIBE Request ID 1 for (n)/t with one parameter: an undecodable AUTHORIZATION_TOKEN (Alias Type 3 with
// no Token Type), and the unknown parameter 0x7e.
Bytes undecodable_token_subscribe() { return b({3, 0, 10, 1, 1, 1, 'n', 1, 't', 1, 3, 1, 3}); }
Bytes unknown_parameter_subscribe() { return b({3, 0, 9, 1, 1, 1, 'n', 1, 't', 1, 0x7e, 0}); }
// The runner's liveness follow-up: SUBSCRIBE Request ID 7 for (n)/t, no parameters.
Bytes liveness_subscribe() { return b({3, 0, 7, 7, 1, 1, 'n', 1, 't', 0}); }

TEST(Draft22FixtureProbesLive, UndecodableTokenProbeNamesTheRunsTrackAndReachesTheToken) {
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"d22-request-undecodable-authorization-token"}, 1500ms,
        app::TrackFixture{{"n"}, "t"}});
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = connect(store, started, 1);
    ASSERT_NE(client, nullptr);
    if (const auto request = named_request(*client)) {
        EXPECT_EQ(*request, undecodable_token_subscribe());
        // Section 8.9: an undecodable token is a KEY_VALUE_FORMATTING_ERROR.
        EXPECT_TRUE(client->close(6, {}));
    }
    pump_until_context_ends(*client, store, started.id);
    const auto run = finish(manager, store, started.id);
    EXPECT_FALSE(harness_error(run));
    EXPECT_EQ(state_of(run, "D22-8-9-MUST-279"), requirements::OutcomeState::Pass);
}

TEST(Draft22FixtureProbesLive, UnknownParameterProbeIsJudgedOnThePublishersAnswerNotOnTheName) {
    // The publisher accepts the unknown parameter (as if it ignored it) and stays live: that is a genuine
    // failure of Section 9.20. Before, the empty namespace drew the 0x3 close the row expects: a false pass.
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"d22-unknown-message-parameter"}, 3000ms, app::TrackFixture{{"n"}, "t"}});
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = connect(store, started, 1);
    ASSERT_NE(client, nullptr);
    if (const auto request = named_request(*client)) {
        EXPECT_EQ(*request, unknown_parameter_subscribe());
        EXPECT_TRUE(client->send_stream(request_stream(0), subscribe_ok(), false));
        EXPECT_TRUE(answer(*client, request_stream(1), liveness_subscribe(), subscribe_ok()))
            << "the liveness follow-up asks for the run's track";
    }
    pump_until_context_ends(*client, store, started.id);
    const auto run = finish(manager, store, started.id);
    EXPECT_FALSE(harness_error(run));
    EXPECT_EQ(state_of(run, "D22-9-20-MUST-390"), requirements::OutcomeState::Fail);
}

}  // namespace
}  // namespace moq::interop
