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

}  // namespace
}  // namespace moq::interop
