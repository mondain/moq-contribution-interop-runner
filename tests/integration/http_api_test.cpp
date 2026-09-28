#include "moq/interop/http/server.h"

#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace moq::interop::http {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

class TemporaryDatabase {
public:
    TemporaryDatabase()
        : directory_(std::filesystem::temp_directory_path() /
                     ("moq-interop-http-" + std::to_string(next_++))),
          path_(directory_ / "runs.sqlite3") {
        std::filesystem::create_directories(directory_);
    }
    ~TemporaryDatabase() { std::filesystem::remove_all(directory_); }
    const std::filesystem::path& path() const { return path_; }

private:
    inline static unsigned long long next_ = 0;
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(
        draft, root / "docs", root / "requirements" / "draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog::load(
            source, root / "requirements" / ("draft" + std::to_string(draft) + ".json")));
}

app::BuildInfo test_build() {
    return {std::string{"0.1<script>\0", 13}, "rev&\"'", {{"dep<script>", "one&two"}}};
}

class HttpApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_shared<storage::SqliteRunStore>(database_.path(), test_build());
        server_ = std::make_unique<HttpServer>(catalog(18), catalog(21), store_, test_build(),
                                               ServerConfig{.port = 0});
        ASSERT_TRUE(server_->start());
        ASSERT_GT(server_->port(), 0);
        client_ = std::make_unique<httplib::Client>("127.0.0.1", server_->port());
        client_->set_connection_timeout(2s);
        client_->set_read_timeout(2s);
    }

    void TearDown() override {
        client_.reset();
        server_.reset();
    }

    Json get_json(const std::string& path, int expected_status = 200) {
        const auto response = client_->Get(path);
        EXPECT_TRUE(response) << path;
        if (!response) return {};
        EXPECT_EQ(response->status, expected_status) << response->body;
        EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
        return Json::parse(response->body);
    }

    TemporaryDatabase database_;
    std::shared_ptr<storage::SqliteRunStore> store_;
    std::unique_ptr<HttpServer> server_;
    std::unique_ptr<httplib::Client> client_;
};

TEST(HttpServerConfigTest, DefaultsToLocalhostPort8080) {
    const ServerConfig config;
    EXPECT_EQ(config.bind_address, "127.0.0.1");
    EXPECT_EQ(config.port, 8080);
}

TEST_F(HttpApiTest, ReportsReadinessAndCompleteDraftInventory) {
    const auto health = get_json("/healthz");
    EXPECT_EQ(health.at("schema_version"), 1);
    EXPECT_EQ(health.at("status"), "ok");
    EXPECT_TRUE(health.at("database").at("ready"));
    EXPECT_EQ(health.at("supported_drafts"), Json::array({18, 21}));

    const auto drafts = get_json("/api/v1/drafts");
    ASSERT_EQ(drafts.at("drafts").size(), 2);
    EXPECT_EQ(drafts.at("drafts").at(0).at("requirement_count"), 598);
    EXPECT_EQ(drafts.at("drafts").at(1).at("requirement_count"), 638);
    EXPECT_TRUE(drafts.at("drafts").at(0).at("complete"));

    const auto report = client_->Get("/results");
    ASSERT_TRUE(report);
    EXPECT_NE(report->body.find("No runs have been created."), std::string::npos);
}

TEST_F(HttpApiTest, ValidatesDraftAndPaginatesRequirements) {
    auto page = get_json("/api/v1/requirements?draft=18&limit=2&offset=1");
    EXPECT_EQ(page.at("pagination").at("limit"), 2);
    EXPECT_EQ(page.at("pagination").at("offset"), 1);
    EXPECT_EQ(page.at("pagination").at("total"), 598);
    EXPECT_EQ(page.at("items").size(), 2);
    EXPECT_TRUE(page.at("items").at(0).contains("source"));
    EXPECT_TRUE(page.at("items").at(0).contains("rationale"));

    for (const auto& path : {"/api/v1/requirements", "/api/v1/requirements?draft=19",
                             "/api/v1/requirements?draft=18&limit=0",
                             "/api/v1/requirements?draft=18&offset=no"}) {
        const auto error = get_json(path, 400);
        EXPECT_EQ(error.at("schema_version"), 1);
        EXPECT_EQ(error.at("error").at("status"), 400);
        EXPECT_FALSE(error.at("error").at("code").get<std::string>().empty());
        EXPECT_FALSE(error.at("error").at("message").get<std::string>().empty());
    }
}

TEST_F(HttpApiTest, CreatesListsAndLoadsRunsWithEvents) {
    const auto malformed = client_->Post("/api/v1/runs", "{", "application/json");
    ASSERT_TRUE(malformed);
    EXPECT_EQ(malformed->status, 400);
    EXPECT_EQ(Json::parse(malformed->body).at("error").at("code"), "invalid_json");

    const auto invalid = client_->Post(
        "/api/v1/runs",
        Json{{"draft", 19}, {"transport", "tcp"}, {"mode", "unknown"},
             {"scenarios", Json::array()}, {"timeout_ms", 0}}.dump(),
        "application/json");
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->status, 400);
    EXPECT_EQ(Json::parse(invalid->body).at("error").at("code"), "invalid_run_config");

    const Json request = {{"draft", 21},
                          {"transport", "webtransport"},
                          {"mode", "driven"},
                          {"scenarios", Json::array({"session/setup", "publisher/object"})},
                          {"timeout_ms", 12345}};
    const auto created = client_->Post("/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto created_json = Json::parse(created->body);
    EXPECT_EQ(created_json.at("schema_version"), 1);
    EXPECT_EQ(created_json.at("run").at("state"), "active");
    const auto id = created_json.at("run").at("id").get<std::string>();

    storage::EvidenceEvent event;
    event.monotonic_time_ns = 11;
    event.wall_time_unix_ns = 22;
    event.kind = std::string{"kind<script>\0", 13};
    event.detail = "detail&\"'";
    event.scenario_id = "session/setup";
    store_->append_events(id, std::vector{event});

    const auto second = client_->Post("/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(second);
    ASSERT_EQ(second->status, 201);
    const auto second_id = Json::parse(second->body).at("run").at("id").get<std::string>();

    const auto list = get_json("/api/v1/runs?limit=1&offset=0");
    EXPECT_EQ(list.at("pagination").at("total"), 2);
    EXPECT_EQ(list.at("items").at(0).at("id"), second_id);
    EXPECT_EQ(list.at("pagination").at("next_offset"), 1);

    const auto loaded = get_json("/api/v1/runs/" + id);
    EXPECT_EQ(loaded.at("run").at("config").at("timeout_ms"), 12345);
    EXPECT_EQ(loaded.at("run").at("events").at("total"), 1);
    EXPECT_EQ(get_json("/api/v1/runs/" + id + "/events?limit=1&offset=0")
                  .at("items").at(0).at("detail"),
              "detail&\"'");
    EXPECT_EQ(get_json("/results/" + id + ".json").at("run").at("id"), id);
    EXPECT_EQ(get_json("/api/v1/runs/not-a-run", 404).at("error").at("code"),
              "run_not_found");
    const auto stop = client_->Post("/api/v1/runs/" + id + "/stop", "", "application/json");
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->status, 501);
    EXPECT_EQ(Json::parse(stop->body).at("error").at("code"), "not_implemented");
}

TEST_F(HttpApiTest, RendersEscapedAccessibleZeroScoreReport) {
    std::string special = "scenario<script>&\"'";
    special.push_back('\0');
    special += "tail";
    app::RunConfig config{app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
                          app::RunMode::Observed, {special}, 1s};
    const auto id = store_->create_run(config);
    storage::EvidenceEvent event;
    event.monotonic_time_ns = 1;
    event.wall_time_unix_ns = 2;
    event.kind = special;
    event.detail = special;
    store_->append_events(id, std::vector{event});
    const requirements::ScoreSummary score{requirements::RunVerdict::Incomplete,
                                            {0, 0}, {0, 0}, {0, 0}};
    store_->finalize(id, score, {});

    const auto json = get_json("/api/v1/runs/" + id);
    EXPECT_EQ(json.at("run").at("config").at("scenarios").at(0), special);
    EXPECT_EQ(json.at("run").at("build").at("version"), test_build().version);
    EXPECT_EQ(get_json("/api/v1/runs/" + id + "/events").at("items").at(0).at("detail"),
              special);

    const auto response = client_->Get("/results");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->get_header_value("Content-Type"), "text/html; charset=utf-8");
    EXPECT_EQ(response->get_header_value("X-Content-Type-Options"), "nosniff");
    EXPECT_FALSE(response->get_header_value("Content-Security-Policy").empty());
    EXPECT_EQ(response->body.find("<script>"), std::string::npos);
    EXPECT_NE(response->body.find("scenario&lt;script&gt;&amp;&quot;&#39;"), std::string::npos);
    EXPECT_NE(response->body.find("FINALIZED"), std::string::npos);
    EXPECT_NE(response->body.find("INCOMPLETE"), std::string::npos);
    EXPECT_NE(response->body.find("0/0"), std::string::npos);
}

TEST_F(HttpApiTest, StopsAndRestartsCleanlyInProcess) {
    server_->stop();
    EXPECT_FALSE(server_->running());
    EXPECT_TRUE(server_->start());
    client_ = std::make_unique<httplib::Client>("127.0.0.1", server_->port());
    const auto response = client_->Get("/healthz");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
}

}  // namespace
}  // namespace moq::interop::http
