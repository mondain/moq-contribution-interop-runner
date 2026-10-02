#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"
#include "moq/interop/wire/draft18/messages.h"
#include "support/picoquic_client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace moq::interop {
namespace {
using Bytes = std::vector<std::byte>;
namespace d18 = wire::draft18;

Bytes literal(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
Bytes encoded(const d18::Message& message) {
    wire::ByteWriter writer(65546);
    if (!d18::encode_message(message, writer).has_value()) throw std::logic_error("unencodable");
    return {writer.bytes().begin(), writer.bytes().end()};
}
Bytes alpn() { return literal({'m', 'o', 'q', 't', '-', '1', '8'}); }

template <typename Predicate>
bool pump_until(transport::test::PicoquicTestClient& client, Predicate predicate,
                std::chrono::milliseconds limit = std::chrono::seconds{3}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

class Harness {
public:
    Harness()
        : store(std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}})),
          manager(catalog(18), catalog(21), store,
              {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
               .maximum_active_runs = 1,
               .certificate_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "cert.pem",
               .private_key_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "key.pem"}) {}

    static std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
        const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
        const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source, root / "requirements" / ("draft" + std::to_string(draft) + ".json")));
    }

    app::RunConfig config(const std::string& scenario, bool fixture) const {
        return {app::DraftVersion::Draft18, app::TransportKind::NativeQuic, app::RunMode::Observed, {scenario},
                std::chrono::milliseconds(1500),
                fixture ? std::optional<app::TrackFixture>{app::TrackFixture{{"n"}, "t"}} : std::nullopt};
    }

    // Connects a publisher peer and returns once the probe's SETUP arrived.
    std::unique_ptr<transport::test::PicoquicTestClient> connect(const app::RunStartResult& started) {
        auto client = transport::test::PicoquicTestClient::create({.port = started.endpoint.port, .alpn = alpn()});
        EXPECT_NE(client, nullptr);
        if (!client) return nullptr;
        EXPECT_TRUE(pump_until(*client, [&] {
            const auto setup = client->stream(3);
            return setup && setup->data.size() >= 4;
        }));
        return client;
    }

    requirements::OutcomeState outcome(const app::RunId& id, const std::string& requirement) {
        const auto run = store->load(id);
        for (const auto& item : run.outcomes)
            if (item.requirement_id == requirement) return item.state;
        ADD_FAILURE() << "no outcome for " << requirement;
        return requirements::OutcomeState::NotRun;
    }

    bool finalized(transport::test::PicoquicTestClient& client, const app::RunId& id) {
        return pump_until(client, [&] { return store->load(id).state == storage::RunState::Finalized; });
    }

    std::shared_ptr<storage::SqliteRunStore> store;
    app::NativeRunManager manager;
};


Bytes publish_blocked(const std::string& name) {
    Bytes track;
    for (const auto character : name) track.push_back(static_cast<std::byte>(character));
    return encoded(d18::PublishBlockedMessage{d18::TrackNamespace{{literal({'s'})}}, d18::TrackName{track}});
}

// Drives a publisher peer through the credit scenario on a real native QUIC
// connection. Returns the client once PUBLISH_BLOCKED was sent.
struct CreditRun {
    std::unique_ptr<transport::test::PicoquicTestClient> client;
    app::RunStartResult started;
};

CreditRun blocked_publisher(Harness& harness, const std::string& scenario) {
    CreditRun run;
    run.started = harness.manager.start(harness.config(scenario, true));
    EXPECT_EQ(run.started.status, app::RunStartStatus::Started);
    if (run.started.status != app::RunStartStatus::Started) return run;
    run.client = harness.connect(run.started);
    if (!run.client) return run;
    auto& client = *run.client;
    EXPECT_TRUE(client.send_stream(2, literal({0xaf, 0, 0, 0}), false));
    // The announcement spends the peer's only bidirectional stream.
    EXPECT_TRUE(client.send_stream(0, encoded(d18::PublishNamespaceMessage{
        0, d18::TrackNamespace{{literal({'n'})}}, {}}), false));
    EXPECT_TRUE(pump_until(client, [&] {
        const auto ack = client.stream(0);
        return ack && !ack->data.empty();
    }));
    // A second bidirectional stream cannot be opened: no credit is left.
    EXPECT_EQ(client.try_send_stream(4, literal({0}), false).status,
              transport::test::ClientStreamSendStatus::WouldBlock);
    EXPECT_TRUE(pump_until(client, [&] {
        const auto request = client.stream(1);
        return request && !request->data.empty();
    }));
    Bytes reply = encoded(d18::RequestOkMessage{{}, {}});
    const auto blocked = publish_blocked("t");
    reply.insert(reply.end(), blocked.begin(), blocked.end());
    EXPECT_TRUE(client.send_stream(1, reply, false));
    return run;
}

TEST(Draft18GapBNative, ResponsePrecedesPublishBlockedWhenCreditIsExhausted) {
    Harness harness;
    auto run = blocked_publisher(harness, "subscribe-tracks-with-no-bidirectional-stream-credit");
    ASSERT_NE(run.client, nullptr);
    ASSERT_TRUE(harness.finalized(*run.client, run.started.id));
    EXPECT_EQ(harness.outcome(run.started.id, "D18-6-1-MUST-004"), requirements::OutcomeState::Pass);
}

TEST(Draft18GapBNative, PublishForBlockedTrackAfterCreditRestoredFails) {
    Harness harness;
    auto run = blocked_publisher(harness, "restore-bidi-stream-credit-after-publish-blocked");
    ASSERT_NE(run.client, nullptr);
    auto& client = *run.client;
    // The runner restores credit only after PUBLISH_BLOCKED; the stream now opens.
    d18::PublishMessage publish{2, d18::TrackNamespace{{literal({'n'}), literal({'s'})}},
                                d18::TrackName{literal({'t'})}, 9, {}, {}};
    bool opened = false;
    ASSERT_TRUE(pump_until(client, [&] {
        opened = client.try_send_stream(4, encoded(publish), false).status ==
                 transport::test::ClientStreamSendStatus::Success;
        return opened;
    }));
    ASSERT_TRUE(harness.finalized(client, run.started.id));
    EXPECT_EQ(harness.outcome(run.started.id, "D18-6-1-MUST-NOT-001"), requirements::OutcomeState::Fail);
}

TEST(Draft18GapBNative, OtherTrackAfterCreditRestoredPasses) {
    Harness harness;
    auto run = blocked_publisher(harness, "restore-bidi-stream-credit-after-publish-blocked");
    ASSERT_NE(run.client, nullptr);
    auto& client = *run.client;
    d18::PublishMessage publish{2, d18::TrackNamespace{{literal({'n'}), literal({'s'})}},
                                d18::TrackName{literal({'u'})}, 9, {}, {}};
    ASSERT_TRUE(pump_until(client, [&] {
        return client.try_send_stream(4, encoded(publish), false).status ==
               transport::test::ClientStreamSendStatus::Success;
    }));
    ASSERT_TRUE(harness.finalized(client, run.started.id));
    EXPECT_EQ(harness.outcome(run.started.id, "D18-6-1-MUST-NOT-001"), requirements::OutcomeState::Pass);
}

// Section 8: a subgroup stream reset is the only wire trace of an enforced timeout.
TEST(Draft18GapBNative, ExpiredObjectIsScoredByADeliveryTimeoutResetAfterCredit) {
    Harness harness;
    const auto started = harness.manager.start(harness.config("subgroup-object-expires-before-transport-handoff", true));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = harness.connect(started);
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->send_stream(2, literal({0xaf, 0, 0, 0}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(1);
        return request && !request->data.empty();
    }));
    ASSERT_TRUE(client->send_stream(1, encoded(d18::SubscribeOkMessage{4, {}, {}}), false));
    // The runner leaves the publisher no unidirectional stream for its subgroup.
    EXPECT_EQ(client->try_send_stream(6, literal({0x14}), false).status,
              transport::test::ClientStreamSendStatus::WouldBlock);
    // Credit arrives only after the timeout window; the publisher then resets.
    ASSERT_TRUE(pump_until(*client, [&] {
        return client->try_send_stream(6, literal({0x14}), false).status ==
               transport::test::ClientStreamSendStatus::Success;
    }));
    ASSERT_TRUE(client->reset_stream(6, 2));
    ASSERT_TRUE(harness.finalized(*client, started.id));
    EXPECT_EQ(harness.outcome(started.id, "D18-8-MUST-003"), requirements::OutcomeState::Pass);
}

TEST(Draft18GapBNative, ResetThatSurvivesTheWithheldPathScoresTheSubgroupTimeout) {
    Harness harness;
    const auto started = harness.manager.start(
        harness.config("withhold-subgroup-acknowledgements-after-application-completion", true));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = harness.connect(started);
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->send_stream(2, literal({0xaf, 0, 0, 0}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(1);
        return request && !request->data.empty();
    }));
    // The publisher sends the subgroup, then resets it as its timer fires; the
    // runner discards both until the path resumes.
    ASSERT_TRUE(client->send_stream(6, literal({0x14, 4, 7, 0, 0, 1, 'p'}), false));
    ASSERT_TRUE(client->reset_stream(6, 2));
    ASSERT_TRUE(harness.finalized(*client, started.id));
    EXPECT_EQ(harness.outcome(started.id, "D18-8-MUST-006"), requirements::OutcomeState::Pass);
}

// Reads the New Session URI of the runner's control-stream GOAWAY.
std::optional<std::string> goaway_uri(transport::test::PicoquicTestClient& client) {
    const auto control = client.stream(3);
    if (!control || control->data.size() <= 4) return std::nullopt;
    wire::Cursor cursor(std::span<const std::byte>(control->data).subspan(4));
    const auto decoded = d18::decode_message(d18::StreamRole::Control, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    const auto* goaway = message ? std::get_if<d18::GoawayMessage>(message) : nullptr;
    if (!goaway) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(goaway->new_session_uri.data()),
                       goaway->new_session_uri.size());
}

TEST(Draft18GapBNative, PublisherMigratesToTheGoawayUri) {
    for (const bool use_offered_path : {true, false}) {
        Harness harness;
        const auto started = harness.manager.start(harness.config("receive-control-goaway-with-new-session-uri", false));
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = harness.connect(started);
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(client->send_stream(2, literal({0xaf, 0, 0, 0}), false));
        std::optional<std::string> uri;
        ASSERT_TRUE(pump_until(*client, [&] { uri = goaway_uri(*client); return uri.has_value(); }));
        // moqt://127.0.0.1:<port>/moq-next names a second, live listener.
        ASSERT_EQ(uri->rfind("moqt://127.0.0.1:", 0), 0u) << *uri;
        const auto colon = uri->rfind(':');
        const auto slash = uri->find('/', colon);
        const auto port = static_cast<std::uint16_t>(std::stoul(uri->substr(colon + 1, slash - colon - 1)));
        const auto path = uri->substr(slash);
        EXPECT_EQ(path, "/moq-next");
        EXPECT_NE(port, started.endpoint.port);
        auto replacement = transport::test::PicoquicTestClient::create({.port = port, .alpn = alpn()});
        ASSERT_NE(replacement, nullptr);
        const std::string stated_path = use_offered_path ? path : "/moq";
        const std::string authority = "127.0.0.1:" + std::to_string(port);
        d18::KeyValuePairs options{{1, d18::ByteValue{Bytes(reinterpret_cast<const std::byte*>(stated_path.data()),
                                       reinterpret_cast<const std::byte*>(stated_path.data()) + stated_path.size())}},
                                   {5, d18::ByteValue{Bytes(reinterpret_cast<const std::byte*>(authority.data()),
                                       reinterpret_cast<const std::byte*>(authority.data()) + authority.size())}}};
        ASSERT_TRUE(pump_until(*replacement, [&] { return replacement->established(); }));
        ASSERT_TRUE(replacement->send_stream(2, encoded(d18::SetupMessage{options}), false));
        // The runner answers the replacement session with its own SETUP.
        ASSERT_TRUE(pump_until(*replacement, [&] {
            const auto setup = replacement->stream(3);
            return setup && setup->data.size() >= 4;
        }));
        ASSERT_TRUE(harness.finalized(*replacement, started.id));
        EXPECT_EQ(harness.outcome(started.id, "D18-10-4-MUST-004"),
                  use_offered_path ? requirements::OutcomeState::Pass : requirements::OutcomeState::Fail);
    }
}

TEST(Draft18GapBNative, NoMigrationLeavesTheGoawayUriRowUnscored) {
    Harness harness;
    auto config = harness.config("receive-control-goaway-with-new-session-uri", false);
    config.timeout = std::chrono::milliseconds(600);
    const auto started = harness.manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = harness.connect(started);
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->send_stream(2, literal({0xaf, 0, 0, 0}), false));
    ASSERT_TRUE(harness.finalized(*client, started.id));
    EXPECT_EQ(harness.outcome(started.id, "D18-10-4-MUST-004"), requirements::OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop
