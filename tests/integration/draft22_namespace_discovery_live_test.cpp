// Live draft 22 runs of d22-discover-original-publisher-namespaces (D22-4-2-MUST-110): a picoquic publisher
// stand-in answers the runner's SUBSCRIBE and SUBSCRIBE_NAMESPACE requests against the production
// NativeRunManager on moqt-22.
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft22_namespace_discovery.h"
#include "support/draft22_own_live.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace moq::interop {
namespace {

using namespace live22;

constexpr std::string_view kRow = "D22-4-2-MUST-110";

// The runner's requests for track (moq)(n)/t: SUBSCRIBE with FORWARD=0, SUBSCRIBE_NAMESPACE with the empty
// prefix and, once that one is cancelled, with (moq) and (mo).
Bytes subscribe() { return b({3, 0, 13, 1, 2, 3, 'm', 'o', 'q', 1, 'n', 1, 't', 1, 0x10, 0}); }
Bytes empty() { return b({0x50, 0, 3, 3, 0, 0}); }
Bytes matching() { return b({0x50, 0, 7, 5, 1, 3, 'm', 'o', 'q', 0}); }
Bytes nonmatching() { return b({0x50, 0, 6, 7, 1, 2, 'm', 'o', 0}); }

Bytes subscribe_ok() { return b({4, 0, 2, 1, 0}); }
Bytes request_ok() { return b({7, 0, 1, 0}); }
Bytes request_error() { return b({5, 0, 3, 0x10, 0, 0}); }
Bytes namespace_n() { return b({8, 0, 3, 1, 1, 'n'}); }
Bytes namespace_full() { return b({8, 0, 7, 2, 3, 'm', 'o', 'q', 1, 'n'}); }

Bytes concat(Bytes left, const Bytes& right) {
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

// What the publisher answers. The runner asks the matching and nonmatching prefixes only after cancelling
// the settled empty one, which a FIN on the empty prefix's response keeps it from doing.
struct Publisher {
    Bytes empty_answer{concat(request_ok(), namespace_full())};
    bool empty_fin{false};
    Bytes matching_answer{concat(request_ok(), namespace_n())};
    bool matching_fin{false};
    bool asks_others{true};
};

struct Played {
    storage::RunRecord run;
    std::vector<Bytes> written;  // the runner's requests, in order, as far as they were sent
    std::vector<std::optional<bool>> verdicts;
};

// Waits until each of `requests` arrived on the runner's request streams `first`, `first` + 1, ...
void receive(Client& client, std::size_t first, const std::vector<Bytes>& requests, std::vector<Bytes>& written) {
    EXPECT_TRUE(pump_until(client, [&] {
        for (std::size_t index = 0; index < requests.size(); ++index) {
            const auto stream = client.stream(request_stream(first + index));
            if (!stream || stream->data.size() < requests[index].size()) return false;
        }
        return true;
    })) << "requests from " << first << " arrive";
    for (std::size_t index = 0; index < requests.size(); ++index)
        if (const auto stream = client.stream(request_stream(first + index))) written.push_back(stream->data);
}

Played run(const Publisher& publisher) {
    const VerdictRecorder recorder(scenarios::kDraft22NamespaceDiscoveryEvaluator, scenarios::kDraft22DiscoverNamespaces,
                                   scenarios::evaluate_draft22_namespace_discovery);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {std::string(scenarios::kDraft22DiscoverNamespaces)}, 1500ms,
        app::TrackFixture{{"moq", "n"}, "t"}});
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
    receive(*client, 0, {subscribe(), empty()}, played.written);
    EXPECT_TRUE(client->send_stream(request_stream(0), subscribe_ok(), false));
    EXPECT_TRUE(client->send_stream(request_stream(1), publisher.empty_answer, publisher.empty_fin));
    if (publisher.asks_others) {
        receive(*client, 2, {matching(), nonmatching()}, played.written);
        // The empty prefix was cancelled with STOP_SENDING (CANCELLED) before the later prefixes.
        const auto stopped = client->try_send_stream(request_stream(1), {}, false);
        EXPECT_EQ(stopped.status, transport::test::ClientStreamSendStatus::PeerStopped);
        EXPECT_EQ(stopped.application_error, 1u);
        EXPECT_TRUE(client->send_stream(request_stream(2), publisher.matching_answer, publisher.matching_fin));
        EXPECT_TRUE(client->send_stream(request_stream(3), request_error(), true));
    }
    pump_until_context_ends(*client, store, started.id);
    played.run = store->load(started.id);
    played.verdicts = recorder.verdicts();
    EXPECT_TRUE(manager.stop(started.id));
    return played;
}

TEST(Draft22NamespaceDiscoveryLive, ScenarioIsImplementedAndNeedsATrack) {
    EXPECT_TRUE(app::executable_scenario(22, scenarios::kDraft22DiscoverNamespaces));
    EXPECT_TRUE(app::raw_probe_scenario(22, scenarios::kDraft22DiscoverNamespaces));
    EXPECT_TRUE(app::scenario_requires_track(22, scenarios::kDraft22DiscoverNamespaces));
    EXPECT_FALSE(app::scenario_requires_fetch(22, scenarios::kDraft22DiscoverNamespaces));
}

TEST(Draft22NamespaceDiscoveryLive, ConformingPublisherPasses) {
    const auto played = run({});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, (std::vector<Bytes>{subscribe(), empty(), matching(), nonmatching()}));
    EXPECT_FALSE(harness_error(played.run));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Pass);
}

TEST(Draft22NamespaceDiscoveryLive, PrefixIgnoringPublisherFails) {
    // Under (moq) it announces the whole namespace as the suffix, i.e. (moq)(moq)(n), and ends the
    // response: the track's namespace was never announced under a prefix it matches.
    Publisher publisher;
    publisher.matching_answer = concat(request_ok(), namespace_full());
    publisher.matching_fin = true;
    const auto played = run(publisher);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Fail);
}

TEST(Draft22NamespaceDiscoveryLive, PublisherEndingTheEmptyPrefixWithoutTheNamespaceFails) {
    // The FIN keeps the runner from cancelling the empty prefix; the writes it sent still prove the failure.
    Publisher publisher;
    publisher.empty_answer = request_ok();
    publisher.empty_fin = true;
    publisher.asks_others = false;
    const auto played = run(publisher);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, (std::vector<Bytes>{subscribe(), empty()}));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::Fail);
}

TEST(Draft22NamespaceDiscoveryLive, SilentPublisherIsNotScored) {
    // Accepts the empty prefix and never sends NAMESPACE nor FIN: the duty has no deadline to miss.
    Publisher publisher;
    publisher.empty_answer = request_ok();
    publisher.asks_others = false;
    const auto played = run(publisher);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{std::nullopt}));
    EXPECT_EQ(state_of(played.run, kRow), requirements::OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop
