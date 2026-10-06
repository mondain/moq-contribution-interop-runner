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

#include <algorithm>
#include <filesystem>
#include <memory>
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
    void SetUp() override {
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
                .private_key_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "key.pem"},
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

}  // namespace
}  // namespace moq::interop
