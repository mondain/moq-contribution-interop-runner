// Draft 22 runs through the HTTP API, wired as src/app/main.cpp wires them: one draft 22 catalog object shared
// by the run manager and the server. A picoquic publisher stand-in plays against the production
// NativeRunManager on moqt-22; the tests read the run back through the API and audit the stored run.
#include "moq/interop/app/draft_traits.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/http/server.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/draft22_evaluators.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/execution_audit.h"
#include "moq/interop/storage/run_store.h"
#include "support/draft22_publisher_scripts.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moq::interop {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / ("requirements/draft" + std::to_string(draft) + ".json")));
}

class Draft22HttpRunLive : public ::testing::Test {
protected:
    void SetUp() override { build({}, {}); }

    // (Re)builds the manager and the server as main.cpp wires them, with an optional publisher driver.
    void build(const std::filesystem::path& driver, const std::filesystem::path& driver_logs) {
        api_.reset();
        server_.reset();
        runs_.reset();
        store_ = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        // As main.cpp: the draft 22 catalog loads RequireComplete, and the one object serves both.
        auto draft18 = catalog(18);
        auto draft21 = catalog(21);
        draft22_ = catalog(22);
        runs_ = std::make_shared<app::NativeRunManager>(
            draft18, draft21, store_,
            app::NativeRunManagerConfig{
                .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
                .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
                .certificate_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "cert.pem",
                .private_key_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "key.pem",
                .driver_executable = driver, .driver_log_root = driver_logs},
            draft22_);
        http::ServerConfig config;
        config.port = 0;
        config.draft22_catalog = draft22_;
        server_ = std::make_unique<http::HttpServer>(draft18, draft21, store_, app::BuildInfo{"test", "test", {}},
                                                     config, runs_);
        ASSERT_TRUE(server_->start());
        api_ = std::make_unique<httplib::Client>("127.0.0.1", server_->port());
        api_->set_connection_timeout(2s);
        api_->set_read_timeout(5s);
    }
    void TearDown() override {
        api_.reset();
        server_.reset();
        runs_.reset();
    }

    Json get(const std::string& path) {
        const auto response = api_->Get(path);
        EXPECT_TRUE(response) << path;
        if (!response) return {};
        EXPECT_EQ(response->status, 200) << path << ": " << response->body;
        return Json::parse(response->body);
    }

    static Json request(unsigned draft, std::vector<std::string> ids) {
        return {{"draft", draft}, {"transport", "native-quic"}, {"mode", "observed"},
                {"scenarios", ids}, {"timeout_ms", 2000},
                {"track", {{"namespace_hex", Json::array({"6e"})}, {"name_hex", "74"}}}};
    }

    // Every stored scenario id of a draft 22 run is a draft 22 id.
    static void expect_draft22_identity(const storage::RunRecord& run) {
        std::size_t stamped = 0;
        for (const auto& event : run.events) {
            if (!event.scenario_id || event.scenario_id->empty()) continue;
            ++stamped;
            EXPECT_TRUE(event.scenario_id->starts_with("d22-")) << event.kind << " " << *event.scenario_id;
        }
        EXPECT_GT(stamped, 0u);
    }

    void expect_audited(const storage::RunRecord& run) {
        const auto bindings = requirements::draft22_executable_bindings();
        const auto audit = requirements::audit_execution(*draft22_, bindings, std::span(&run, 1));
        EXPECT_EQ(audit.run_count, 1u);
        EXPECT_GT(audit.scored_rows, 0u);
        EXPECT_TRUE(audit.consistent());
        for (const auto& finding : audit.findings) ADD_FAILURE() << finding.code << " " << finding.requirement_id;
    }

    std::shared_ptr<storage::SqliteRunStore> store_;
    std::shared_ptr<const requirements::RequirementCatalog> draft22_;
    std::shared_ptr<app::NativeRunManager> runs_;
    std::unique_ptr<http::HttpServer> server_;
    std::unique_ptr<httplib::Client> api_;
};

TEST_F(Draft22HttpRunLive, AdvertisesDraft22AsRunnableAndConfigured) {
    const auto drafts = get("/api/v1/drafts");
    ASSERT_EQ(drafts.at("drafts").size(), 3u);
    for (const auto& entry : drafts.at("drafts")) EXPECT_TRUE(entry.at("runnable")) << entry.at("draft");
    const auto health = get("/healthz");
    EXPECT_EQ(health.at("supported_drafts"), Json::array({18, 21, 22}));
    std::size_t draft22_observed = 0;
    for (const auto& profile : health.at("executable_profiles")) {
        if (profile.at("draft") != 22) continue;
        // Observed profiles run on this manager; driven ones need --driver-executable, as for drafts 18/21.
        EXPECT_EQ(profile.at("configured").get<bool>(), profile.at("mode") == "observed") << profile.dump();
        draft22_observed += profile.at("mode") == "observed";
    }
    EXPECT_GT(draft22_observed, app::executable_scenarios(22).size());
}

// A shared draft 22 selection (the raw FETCH pair, run on draft 21's family) through POST /api/v1/runs: the same
// 201 and body shape as a draft 18/21 run, the moqt-22 endpoint, a stored draft 22 run with draft 22 evidence.
TEST_F(Draft22HttpRunLive, SharedDraft22SelectionRunsThroughTheApi) {
    const std::vector<std::string> ids = {"d22-fetch-accepted", "d22-fetch-rejected"};
    const auto created = api_->Post("/api/v1/runs", request(22, ids).dump(), "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto body = Json::parse(created->body);
    EXPECT_EQ(body.at("schema_version"), 1);
    EXPECT_EQ(body.at("publisher_endpoint").at("alpn"), "moqt-22");
    EXPECT_EQ(body.at("run").at("config").at("draft"), 22);
    EXPECT_EQ(body.at("run").at("config").at("scenarios"), Json(ids));
    const auto id = body.at("run").at("id").get<std::string>();
    const auto port = body.at("publisher_endpoint").at("port").get<std::uint16_t>();
    ASSERT_TRUE(d22pub::play_fetch_pair(*store_, id, port));
    const auto run = d22pub::finalized(*store_, id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(run.config.draft, app::DraftVersion::Draft22);
    EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(),
                             [](const auto& event) { return event.kind == "harness_error"; }));
    expect_draft22_identity(run);
    expect_audited(run);

    const auto stored = get("/api/v1/runs/" + id).at("run");
    EXPECT_EQ(stored.at("config").at("draft"), 22);
    EXPECT_EQ(stored.at("state"), "finalized");
    EXPECT_TRUE(std::any_of(stored.at("outcomes").begin(), stored.at("outcomes").end(), [](const Json& outcome) {
        return outcome.at("requirement_id") == "D22-3-2-MUST-057" && outcome.at("state") == "pass";
    }));
    const auto result = get("/results/" + id + ".json");
    EXPECT_FALSE(result.empty());
    const auto completeness = get("/results/completeness.json");
    const auto entry = std::find_if(completeness.at("drafts").begin(), completeness.at("drafts").end(),
                                    [](const Json& value) { return value.at("draft") == 22; });
    ASSERT_NE(entry, completeness.at("drafts").end());
    const auto& native = entry->at("transports").at(0);
    EXPECT_EQ(native.at("transport"), "native-quic");
    EXPECT_EQ(native.at("run_count"), 1);
    EXPECT_TRUE(native.at("execution_consistent"));
    EXPECT_GE(native.at("observed_requirement_count").get<unsigned>(), 1u);
}

// An own draft 22 scenario (no draft 21 implementation) through the API.
TEST_F(Draft22HttpRunLive, OwnDraft22ScenarioRunsThroughTheApi) {
    const auto created = api_->Post("/api/v1/runs", request(22, {"d22-request-stream-before-peer-setup"}).dump(),
                                    "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto body = Json::parse(created->body);
    const auto id = body.at("run").at("id").get<std::string>();
    ASSERT_TRUE(d22pub::play_pre_setup_reset(*store_, id, body.at("publisher_endpoint").at("port").get<std::uint16_t>()));
    const auto run = d22pub::finalized(*store_, id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    expect_draft22_identity(run);
    expect_audited(run);
    const auto stored = get("/api/v1/runs/" + id).at("run");
    EXPECT_EQ(stored.at("config").at("draft"), 22);
    EXPECT_TRUE(std::any_of(stored.at("outcomes").begin(), stored.at("outcomes").end(), [](const Json& outcome) {
        return outcome.at("requirement_id") == "D22-6-3-MAY-159" && outcome.at("state") == "pass";
    }));
}

// An id draft 22 cannot execute is refused exactly as drafts 18 and 21 refuse theirs, before any run exists.
TEST_F(Draft22HttpRunLive, UnimplementedDraft22IdIsRefusedLikeDraft21) {
    const auto draft21 = api_->Post("/api/v1/runs", request(21, {"d21-no-such-scenario"}).dump(), "application/json");
    const auto draft22 = api_->Post("/api/v1/runs", request(22, {"d22-no-such-scenario"}).dump(), "application/json");
    ASSERT_TRUE(draft21);
    ASSERT_TRUE(draft22);
    EXPECT_EQ(draft22->status, draft21->status);
    EXPECT_EQ(draft22->status, 422);
    EXPECT_EQ(Json::parse(draft22->body).at("error").at("code"), Json::parse(draft21->body).at("error").at("code"));
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
}

// A selection of only FETCH scenarios from a publisher declaring fetch=false: the manager names the requested
// draft 22 id, and the API answers it as it answers draft 21 (422 scenario_requires_publisher_capability, the
// message naming the scenario and the capability), before any run exists.
TEST_F(Draft22HttpRunLive, FetchOnlySelectionWithoutFetchIsRefusedLikeDraft21) {
    const auto post = [&](unsigned draft, const std::string& id) {
        auto body = request(draft, {id});
        body["publisher_capabilities"] = {{"fetch", false}};
        const auto response = api_->Post("/api/v1/runs", body.dump(), "application/json");
        EXPECT_TRUE(response);
        return std::pair{response ? response->status : 0, response ? Json::parse(response->body) : Json{}};
    };
    const auto [status21, body21] = post(21, "d21-fetch-accepted");
    const auto [status22, body22] = post(22, "d22-fetch-accepted");
    EXPECT_EQ(status22, 422) << body22;
    EXPECT_EQ(status22, status21);
    EXPECT_EQ(body22.at("error").at("code"), "scenario_requires_publisher_capability");
    EXPECT_EQ(body22.at("error").at("code"), body21.at("error").at("code"));
    const auto message = body22.at("error").at("message").get<std::string>();
    EXPECT_EQ(message, "Scenario d22-fetch-accepted requires the publisher capability fetch, which this run "
                       "declares the publisher does not implement.");
    // The draft 21 message, with the draft 22 id in place of the draft 21 id.
    auto expected21 = message;
    expected21.replace(expected21.find("d22-"), 4, "d21-");
    EXPECT_EQ(body21.at("error").at("message"), expected21);
    // The manager's own answer behind the response.
    const auto started = runs_->start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"d22-fetch-accepted"}, 1000ms, app::TrackFixture{{"n"}, "t"}, {.fetch = false}});
    EXPECT_EQ(started.status, app::RunStartStatus::ScenarioRequiresCapability);
    EXPECT_EQ(started.scenario, "d22-fetch-accepted");
    EXPECT_EQ(started.capability, "fetch");
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
}

// The SETUP a native QUIC client sends on the replacement session: PATH and AUTHORITY from the URI it uses.
d22pub::Bytes client_setup(const std::string& path, const std::string& authority) {
    d22pub::Bytes options;
    const auto put = [&](unsigned delta, const std::string& value) {
        options.push_back(static_cast<std::byte>(delta));
        options.push_back(static_cast<std::byte>(value.size()));
        for (const char c : value) options.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    };
    put(1, path);
    put(4, authority);
    auto setup = d22pub::bytes({0xaf, 0, 0, static_cast<unsigned>(options.size())});
    setup.insert(setup.end(), options.begin(), options.end());
    return setup;
}

// d22-publisher-goaway-alternate-uri (a shared scenario with a replacement session): the second listener the
// runner opens for the GOAWAY's New Session URI accepts moqt-22, the run's wire draft, and refuses moqt-21, the
// draft its scenarios execute as. The publisher migrates and the draft 22 row passes.
TEST_F(Draft22HttpRunLive, ReplacementSessionListenerSpeaksMoqt22) {
    const auto created = api_->Post("/api/v1/runs", request(22, {"d22-publisher-goaway-alternate-uri"}).dump(),
                                    "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto body = Json::parse(created->body);
    EXPECT_EQ(body.at("publisher_endpoint").at("alpn"), "moqt-22");
    const auto id = body.at("run").at("id").get<std::string>();
    auto first = d22pub::Client::create(
        {.port = body.at("publisher_endpoint").at("port").get<std::uint16_t>(), .alpn = d22pub::alpn22()});
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(d22pub::pump_until(*first, [&] { return first->established(); }));
    ASSERT_TRUE(first->send_stream(2, d22pub::setup(), false));
    // The runner's SETUP (4 bytes), then the GOAWAY naming the replacement listener.
    ASSERT_TRUE(d22pub::pump_until(*first, [&] {
        const auto control = first->stream(3);
        return control && control->data.size() > 4;
    }));
    const auto control = first->stream(3);
    std::string text;
    for (const auto byte : control->data) text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    const auto marker = text.find("moqt://127.0.0.1:");
    ASSERT_NE(marker, std::string::npos) << "no New Session URI in the GOAWAY";
    const auto port = static_cast<std::uint16_t>(std::stoul(text.substr(marker + 17)));
    ASSERT_NE(port, 0);
    ASSERT_NE(text.find("/moq-next", marker), std::string::npos);

    // A draft 21 client cannot open the replacement session of a draft 22 run.
    {
        auto wrong = d22pub::Client::create({.port = port, .alpn = d22pub::bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(wrong, nullptr);
        EXPECT_FALSE(d22pub::pump_until(*wrong, [&] { first->pump(); return wrong->established(); }, 500ms))
            << "the replacement listener accepted moqt-21";
    }
    auto second = d22pub::Client::create({.port = port, .alpn = d22pub::alpn22()});
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(d22pub::pump_until(*second, [&] { first->pump(); return second->established(); }))
        << "the replacement listener refused moqt-22";
    ASSERT_TRUE(second->send_stream(2, client_setup("/moq-next", "127.0.0.1:" + std::to_string(port)), false));
    ASSERT_TRUE(d22pub::pump_until(*first, [&] {
        second->pump();
        return store_->load(id).state == storage::RunState::Finalized;
    }, 6s));
    const auto run = store_->load(id);
    EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(),
                             [](const auto& event) { return event.kind == "harness_error"; }));
    // The replacement session's events are stored under the draft 22 id.
    const auto replacement = std::find_if(run.events.begin(), run.events.end(), [](const auto& event) {
        return event.kind == "raw_probe_replacement_event";
    });
    ASSERT_NE(replacement, run.events.end());
    EXPECT_EQ(replacement->scenario_id, "d22-publisher-goaway-alternate-uri");
    expect_draft22_identity(run);
    expect_audited(run);
    const auto stored = get("/api/v1/runs/" + id).at("run");
    EXPECT_TRUE(std::any_of(stored.at("outcomes").begin(), stored.at("outcomes").end(), [](const Json& outcome) {
        return outcome.at("requirement_id") == "D22-9-2-MUST-340" && outcome.at("state") == "pass";
    }));
}

// A driven draft 22 run through POST /api/v1/runs: the driver executable is started once per context with a
// DriverRequest carrying draft 22 and the requested draft 22 scenario id. The test driver connects with moqt-22
// only for such a request, so the draft 22 row passing shows the contract end to end.
TEST_F(Draft22HttpRunLive, DrivenRunHandsTheDriverDraft22AndTheRequestedIds) {
    const auto logs = std::filesystem::temp_directory_path() /
        ("moq-d22-http-driven-" + std::to_string(::getpid()));
    std::filesystem::remove_all(logs);
    std::filesystem::create_directories(logs);
    build(PICOQUIC_FAMILY_DRIVER_PATH, logs);
    const auto health = get("/healthz");
    EXPECT_TRUE(std::any_of(health.at("executable_profiles").begin(), health.at("executable_profiles").end(),
                            [](const Json& profile) {
                                return profile.at("draft") == 22 && profile.at("mode") == "driven" &&
                                       profile.at("configured").get<bool>();
                            }));
    const std::vector<std::string> ids = {"d22-duplicate-request-goaway", "d22-goaway-on-distinct-request-streams"};
    auto body = request(22, ids);
    body["mode"] = "driven";
    const auto created = api_->Post("/api/v1/runs", body.dump(), "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto response = Json::parse(created->body);
    EXPECT_EQ(response.at("run").at("config").at("mode"), "driven");
    EXPECT_EQ(response.at("publisher_endpoint").at("alpn"), "moqt-22");
    const auto id = response.at("run").at("id").get<std::string>();
    const auto run = d22pub::finalized(*store_, id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(),
                             [](const auto& event) { return event.kind == "harness_error"; }));
    std::vector<std::string> driven;
    for (const auto& event : run.events) {
        if (event.kind != "publisher_process") continue;
        ASSERT_TRUE(event.scenario_id);
        driven.push_back(*event.scenario_id);
        const auto result = Json::parse(event.detail);
        const auto directory =
            std::filesystem::path(result.at("stdout_log").at("path").get<std::string>()).parent_path();
        std::ifstream input(directory / "request.json");
        ASSERT_TRUE(input.good()) << directory;
        const auto driver_request = Json::parse(input);
        EXPECT_EQ(driver_request.at("draft"), 22);
        EXPECT_EQ(driver_request.at("scenario_id"), *event.scenario_id);
    }
    EXPECT_EQ(driven, ids);
    expect_draft22_identity(run);
    expect_audited(run);
    const auto stored = get("/api/v1/runs/" + id).at("run");
    EXPECT_TRUE(std::any_of(stored.at("outcomes").begin(), stored.at("outcomes").end(), [](const Json& outcome) {
        return outcome.at("requirement_id") == "D22-9-2-MUST-339" && outcome.at("state") == "pass";
    }));
    build({}, {});
    std::filesystem::remove_all(logs);
}

}  // namespace
}  // namespace moq::interop
