// Live end-to-end checks for the draft-21 slice-B scenarios that need real
// connections: an actual picoquic publisher stand-in migrates after a GOAWAY.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/http/server.h"
#include "moq/interop/requirements/draft_source.h"
#include "support/picoquic_client.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace moq::interop {
namespace {

using Json = nlohmann::json;
using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

template <class Predicate>
bool pump_until(transport::test::PicoquicTestClient& client, Predicate predicate,
                std::chrono::milliseconds limit = std::chrono::seconds{4}) {
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
    Harness() {
        const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
        const auto source = requirements::load_draft_source(
            21, root / "docs", root / "requirements/draft-digests.json");
        draft21 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog::load(source, root / "requirements/draft21.json"));
        draft18 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{18, "test", true, {}});
        store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        runs = std::make_shared<app::NativeRunManager>(
            draft18, draft21, store,
            app::NativeRunManagerConfig{
                .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
                .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
                .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
                .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"});
        server = std::make_unique<http::HttpServer>(draft18, draft21, store,
            app::BuildInfo{"test", "test", {}}, http::ServerConfig{.port = 0}, runs);
        started = server->start();
        if (started) api = std::make_unique<httplib::Client>("127.0.0.1", server->port());
    }

    Json request(const std::string& scenario, const std::string& namespace_hex, unsigned timeout_ms,
                 const std::string& transport = "native-quic") const {
        return {{"draft", 21}, {"transport", transport}, {"mode", "observed"},
                {"scenarios", Json::array({scenario})}, {"timeout_ms", timeout_ms},
                {"track", {{"namespace_hex", Json::array({namespace_hex})}, {"name_hex", "74"}}}};
    }

    std::shared_ptr<const requirements::RequirementCatalog> draft18, draft21;
    std::shared_ptr<storage::SqliteRunStore> store;
    std::shared_ptr<app::NativeRunManager> runs;
    std::unique_ptr<http::HttpServer> server;
    std::unique_ptr<httplib::Client> api;
    bool started{false};
};

std::string state_of(const Json& run, const std::string& requirement) {
    for (const auto& outcome : run.at("outcomes"))
        if (outcome.at("requirement_id") == requirement) return outcome.at("state");
    return "missing";
}

std::string client_setup(const std::string& path, const std::string& authority) {
    std::string body;
    const auto put = [&](unsigned delta, const std::string& value) {
        body.push_back(static_cast<char>(delta));
        body.push_back(static_cast<char>(value.size()));
        body += value;
    };
    put(1, path);
    put(4, authority);
    std::string result{static_cast<char>(0xaf), 0, 0, static_cast<char>(body.size())};
    return result + body;
}

Bytes as_bytes(const std::string& value) {
    Bytes result;
    for (const char c : value) result.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return result;
}

struct Migration {
    std::string run_id;
    std::uint16_t alternate_port{0};
    std::unique_ptr<transport::test::PicoquicTestClient> first;
};

// Starts the GOAWAY run, connects the first session and returns the port the
// GOAWAY's New Session URI names.
std::optional<Migration> start_migration(Harness& harness, unsigned timeout_ms) {
    const auto created = harness.api->Post("/api/v1/runs",
        harness.request("d21-publisher-goaway-alternate-uri", "6e", timeout_ms).dump(), "application/json");
    if (!created || created->status != 201) return std::nullopt;
    const auto body = Json::parse(created->body);
    Migration result;
    result.run_id = body.at("run").at("id").get<std::string>();
    result.first = transport::test::PicoquicTestClient::create(
        {.port = body.at("publisher_endpoint").at("port").get<std::uint16_t>(),
         .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
    if (!result.first) return std::nullopt;
    if (!pump_until(*result.first, [&] { return result.first->established(); })) return std::nullopt;
    if (!result.first->send_stream(2, bytes({0xaf, 0, 0, 0}), false)) return std::nullopt;
    // SETUP (4 bytes) then the GOAWAY on the control stream.
    if (!pump_until(*result.first, [&] {
            const auto control = result.first->stream(3);
            return control && control->data.size() > 4;
        })) return std::nullopt;
    const auto control = result.first->stream(3);
    std::string text;
    for (const auto byte : control->data) text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    const auto marker = text.find("moqt://127.0.0.1:");
    if (marker == std::string::npos || control->data[4] != std::byte{0x10}) return std::nullopt;
    result.alternate_port = static_cast<std::uint16_t>(std::stoul(text.substr(marker + 17)));
    return result;
}

std::unique_ptr<transport::test::PicoquicTestClient> connect_second(
    transport::test::PicoquicTestClient& first, std::uint16_t port, const std::string& path) {
    auto second = transport::test::PicoquicTestClient::create(
        {.port = port, .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
    if (!second) return nullptr;
    if (!pump_until(*second, [&] { first.pump(); return second->established(); })) return nullptr;
    const auto setup = as_bytes(client_setup(path, "127.0.0.1:" + std::to_string(port)));
    if (!second->send_stream(2, setup, false)) return nullptr;
    return second;
}

bool finalized(Harness& harness, const std::string& id, transport::test::PicoquicTestClient& first,
               transport::test::PicoquicTestClient* second) {
    return pump_until(first, [&] {
        if (second) second->pump();
        return harness.store->load(id).state == storage::RunState::Finalized;
    }, std::chrono::seconds{6});
}

std::string requirement_state(Harness& harness, const std::string& id, const std::string& requirement) {
    const auto result = harness.api->Get("/api/v1/runs/" + id);
    if (!result) return "unreachable";
    return state_of(Json::parse(result->body).at("run"), requirement);
}

TEST(Draft21D21bLive, GoawayUriIsUsedForTheReplacementSession) {
    Harness harness;
    ASSERT_TRUE(harness.started);
    auto migration = start_migration(harness, 3000);
    ASSERT_TRUE(migration.has_value());
    EXPECT_NE(migration->alternate_port, 0);
    auto second = connect_second(*migration->first, migration->alternate_port, "/moq");
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(finalized(harness, migration->run_id, *migration->first, second.get()));
    EXPECT_EQ(requirement_state(harness, migration->run_id, "D21-9-2-MUST-329"), "pass");
}

TEST(Draft21D21bLive, ReplacementSessionThatContradictsTheUriFails) {
    Harness harness;
    ASSERT_TRUE(harness.started);
    auto migration = start_migration(harness, 3000);
    ASSERT_TRUE(migration.has_value());
    auto second = connect_second(*migration->first, migration->alternate_port, "/elsewhere");
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(finalized(harness, migration->run_id, *migration->first, second.get()));
    EXPECT_EQ(requirement_state(harness, migration->run_id, "D21-9-2-MUST-329"), "fail");
}

TEST(Draft21D21bLive, NoReplacementSessionLeavesTheRowUnscored) {
    Harness harness;
    ASSERT_TRUE(harness.started);
    auto migration = start_migration(harness, 1200);
    ASSERT_TRUE(migration.has_value());
    // The publisher ignores the GOAWAY and stays on its first session.
    ASSERT_TRUE(finalized(harness, migration->run_id, *migration->first, nullptr));
    EXPECT_EQ(requirement_state(harness, migration->run_id, "D21-9-2-MUST-329"), "not_run");
}

}  // namespace
}  // namespace moq::interop
