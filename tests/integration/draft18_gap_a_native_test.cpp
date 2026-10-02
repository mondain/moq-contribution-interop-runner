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

TEST(Draft18GapANative, DatagramNegotiationPassesOnceTheSessionIsEstablished) {
    Harness harness;
    const auto started = harness.manager.start(harness.config("establish-moqt-with-datagram-capable-peer", false));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = harness.connect(started);
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->send_stream(2, literal({0xaf, 0, 0, 0}), false));
    ASSERT_TRUE(harness.finalized(*client, started.id));
    EXPECT_EQ(harness.outcome(started.id, "D18-3-1-MUST-001"), requirements::OutcomeState::Pass);
}

TEST(Draft18GapANative, NativeClientSetupOptionsAreScoredFromTheActualSetup) {
    for (const bool complete : {true, false}) {
        Harness harness;
        const auto started = harness.manager.start(harness.config("native-quic-publisher-client-setup-from-moqt-uri", false));
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = harness.connect(started);
        ASSERT_NE(client, nullptr);
        d18::KeyValuePairs options;
        if (complete) options = {{1, d18::ByteValue{literal({'/', 'p'})}}, {5, d18::ByteValue{literal({'h', ':', '1'})}}};
        else options = {{1, d18::ByteValue{literal({'/', 'p'})}}};
        ASSERT_TRUE(client->send_stream(2, encoded(d18::SetupMessage{options}), false));
        ASSERT_TRUE(harness.finalized(*client, started.id));
        EXPECT_EQ(harness.outcome(started.id, "D18-3-2-MUST-001"),
                  complete ? requirements::OutcomeState::Pass : requirements::OutcomeState::Fail);
    }
}

TEST(Draft18GapANative, UnknownSetupOptionIsSentAndASubscribeResponseIsScored) {
    Harness harness;
    auto missing_fixture = harness.config("receive-setup-with-unknown-option", false);
    EXPECT_EQ(harness.manager.start(missing_fixture).status, app::RunStartStatus::InvalidConfig);
    const auto started = harness.manager.start(harness.config("receive-setup-with-unknown-option", true));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = harness.connect(started);
    ASSERT_NE(client, nullptr);
    // The grease option 0x9d, never assigned, rides in the probe's SETUP.
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto setup = client->stream(3);
        return setup && setup->data.size() == 8;
    }));
    EXPECT_EQ(client->stream(3)->data, literal({0xaf, 0, 0, 4, 0x80, 0x9d, 1, 0}));
    ASSERT_TRUE(client->send_stream(2, literal({0xaf, 0, 0, 0}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(1);
        return request && !request->data.empty();
    }));
    ASSERT_TRUE(client->send_stream(1, encoded(d18::SubscribeOkMessage{4, {}, {}}), false));
    ASSERT_TRUE(harness.finalized(*client, started.id));
    EXPECT_EQ(harness.outcome(started.id, "D18-10-3-MUST-001"), requirements::OutcomeState::Pass);
}

TEST(Draft18GapANative, PublisherInitiatedNamespaceRequestIsPlacementChecked) {
    Harness harness;
    const auto started = harness.manager.start(harness.config("initiate-namespace-publication", false));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = harness.connect(started);
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->send_stream(2, literal({0xaf, 0, 0, 0}), false));
    ASSERT_TRUE(client->send_stream(0, encoded(d18::PublishNamespaceMessage{0, d18::TrackNamespace{{literal({'n'})}}, {}}), false));
    ASSERT_TRUE(harness.finalized(*client, started.id));
    EXPECT_EQ(harness.outcome(started.id, "D18-10-MUST-005"), requirements::OutcomeState::Pass);
}

TEST(Draft18GapANative, NativeOnlyProbeIsUnsupportedOverWebTransport) {
    Harness harness;
    auto config = harness.config("native-quic-publisher-client-setup-from-moqt-uri", false);
    config.transport = app::TransportKind::WebTransport;
    EXPECT_EQ(harness.manager.start(config).status, app::RunStartStatus::Unsupported);
}

}  // namespace
}  // namespace moq::interop
