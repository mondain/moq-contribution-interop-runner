// Live end-to-end checks for the completeness-gap slice A scenarios: an actual
// picoquic publisher stand-in drives the runner through HTTP.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/http/server.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/scenarios/draft21_gap_a.h"
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

TEST(Draft21GapALive, ReservedNamespaceAttemptPassesWhenNothingIsPublishedAndFailsWhenItIs) {
    Harness harness;
    ASSERT_TRUE(harness.started);
    // Section 2.4.2: ".", the single period, is the fixture the publisher is told to publish.
    const auto scenario = "d21-attempt-single-period-track-publication";
    for (const bool violate : {false, true}) {
        SCOPED_TRACE(violate);
        const auto created = harness.api->Post("/api/v1/runs",
            harness.request(scenario, "2e", 500).dump(), "application/json");
        ASSERT_TRUE(created);
        ASSERT_EQ(created->status, 201) << created->body;
        const auto body = Json::parse(created->body);
        const auto id = body.at("run").at("id").get<std::string>();
        auto client = transport::test::PicoquicTestClient::create(
            {.port = body.at("publisher_endpoint").at("port").get<std::uint16_t>(),
             .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto setup = client->stream(3);
            return setup && setup->data == bytes({0xaf, 0, 0, 0});
        }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        if (violate) {
            // PUBLISH under "." (Request ID 0, namespace (.), track "t").
            ASSERT_TRUE(client->send_stream(0, bytes({0x1d, 0, 8, 0, 1, 1, '.', 1, 't', 2, 0}), false));
            // The runner rejects it with REQUEST_ERROR (Section 2.4.2).
            ASSERT_TRUE(pump_until(*client, [&] {
                const auto response = client->stream(0);
                return response && !response->data.empty() && response->data.front() == std::byte{0x05};
            }));
        }
        ASSERT_TRUE(pump_until(*client, [&] {
            return harness.store->load(id).state == storage::RunState::Finalized;
        }, std::chrono::seconds{5}));
        const auto result = harness.api->Get("/api/v1/runs/" + id);
        ASSERT_TRUE(result);
        const auto run = Json::parse(result->body).at("run");
        EXPECT_EQ(state_of(run, "D21-2-4-2-MUST-NOT-029"), violate ? "fail" : "pass");
        // The same publication also breaks the broader prohibition, which this scenario does not score.
        EXPECT_EQ(state_of(run, "D21-2-4-2-MUST-NOT-028"), "not_run");
    }
}

TEST(Draft21GapALive, ScenarioFixtureAndTransportRulesAreEnforcedAtStart) {
    Harness harness;
    ASSERT_TRUE(harness.started);
    // A single-period scenario with an ordinary namespace is an invalid configuration.
    auto response = harness.api->Post("/api/v1/runs",
        harness.request("d21-attempt-single-period-track-publication", "6d", 500).dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    // WebTransport-only and native-only scenarios refuse the other transport.
    response = harness.api->Post("/api/v1/runs",
        harness.request("d21-webtransport-publisher-setup", "6d", 500).dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 422);
    response = harness.api->Post("/api/v1/runs",
        harness.request("d21-native-publisher-uri-options", "6d", 500, "webtransport").dump(),
        "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 422);
    // Every new scenario is advertised as an executable profile.
    const auto health = harness.api->Get("/healthz");
    ASSERT_TRUE(health);
    const auto profiles = Json::parse(health->body).at("executable_profiles");
    for (const auto* scenario : {"d21-native-publisher-uri-options", "d21-control-stream-lifetime",
                                 "d21-token-delete-and-reuse", "d21-webtransport-server-sends-path"}) {
        EXPECT_TRUE(std::any_of(profiles.begin(), profiles.end(), [&](const Json& profile) {
            return profile.at("scenario") == scenario;
        })) << scenario;
    }
}

TEST(Draft21GapALive, RawProbeScoresTheResponseDirectionOfARealRequestStream) {
    Harness harness;
    ASSERT_TRUE(harness.started);
    const auto created = harness.api->Post("/api/v1/runs",
        harness.request("d21-publisher-request-response-before-fin", "6e", 2000).dump(), "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto body = Json::parse(created->body);
    const auto id = body.at("run").at("id").get<std::string>();
    auto client = transport::test::PicoquicTestClient::create(
        {.port = body.at("publisher_endpoint").at("port").get<std::uint16_t>(),
         .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(pump_until(*client, [&] { return client->established(); }));
    ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
    const auto probes = scenarios::draft21_gap_a_probes(std::chrono::milliseconds(2000), {bytes({'n'})},
                                                        bytes({'t'}));
    const auto expected = std::find_if(probes.begin(), probes.end(), [](const auto& probe) {
        return probe.definition.id == "d21-publisher-request-response-before-fin";
    });
    ASSERT_NE(expected, probes.end());
    // The runner subscribes with the exact bytes the probe defines, as a server request.
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(1);
        return request && request->data.size() >= 4;
    }));
    const auto request = client->stream(1);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->data.size(), 0u);
    // The fixture namespace is "n" in the probe above and "n" (0x6e) in the run request.
    EXPECT_EQ(request->data, expected->definition.writes.front().bytes);
    // SUBSCRIBE_OK, Track Alias 4, no parameters, keeps the stream open.
    ASSERT_TRUE(client->send_stream(1, bytes({4, 0, 2, 4, 0}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        return harness.store->load(id).state == storage::RunState::Finalized;
    }, std::chrono::seconds{6}));
    const auto result = harness.api->Get("/api/v1/runs/" + id);
    ASSERT_TRUE(result);
    const auto run = Json::parse(result->body).at("run");
    EXPECT_EQ(state_of(run, "D21-6-4-2-2-MUST-157"), "pass");
}

}  // namespace
}  // namespace moq::interop
