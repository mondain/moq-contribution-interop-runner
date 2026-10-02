#include "moq/interop/http/server.h"
#include "moq/interop/app/scenario_registry.h"

#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
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

class FailingRunStore final : public storage::RunStore {
public:
    app::RunId create_run(const app::RunConfig&) override { throw_failure(); }
    void append_events(const app::RunId&, std::span<const storage::EvidenceEvent>) override {
        throw_failure();
    }
    void finalize(const app::RunId&, const requirements::ScoreSummary&,
                  std::span<const requirements::Outcome>) override {
        throw_failure();
    }
    storage::RunRecord load(const app::RunId&) const override { throw_failure(); }
    storage::Page<storage::RunSummary> list(storage::RunQuery) const override {
        throw_failure();
    }
    storage::Page<storage::EvidenceEvent> list_events(const app::RunId&,
                                                       storage::RunQuery) const override {
        throw_failure();
    }

private:
    [[noreturn]] static void throw_failure() {
        throw std::runtime_error("secret database path /private/runs.sqlite3");
    }
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

TEST(HttpServerHealthTest, ReturnsSanitizedUnavailableWhenStoreReadinessFails) {
    auto server = HttpServer(catalog(18), catalog(21), std::make_shared<FailingRunStore>(),
                             test_build(), ServerConfig{.port = 0});
    ASSERT_TRUE(server.start());
    httplib::Client client("127.0.0.1", server.port());
    client.set_connection_timeout(2s);
    client.set_read_timeout(2s);

    const auto response = client.Get("/healthz");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    const auto body = Json::parse(response->body);
    EXPECT_EQ(body.at("schema_version"), 1);
    EXPECT_EQ(body.at("error").at("status"), 503);
    EXPECT_EQ(body.at("error").at("code"), "database_not_ready");
    EXPECT_EQ(response->body.find("/private/runs.sqlite3"), std::string::npos);
}

TEST_F(HttpApiTest, ReportsReadinessAndCompleteDraftInventory) {
    const auto health = get_json("/healthz");
    EXPECT_EQ(health.at("schema_version"), 1);
    EXPECT_EQ(health.at("status"), "ok");
    EXPECT_TRUE(health.at("database").at("ready"));
    EXPECT_EQ(health.at("supported_drafts"), Json::array({18, 21}));
    // Every observed profile is repeated once as a driven profile.
    const auto profile_total = health.at("executable_profiles").size();
    ASSERT_EQ(profile_total % 2, 0u);
    const auto observed_profiles = profile_total / 2;
    for (const auto& profile : health.at("executable_profiles")) {
        EXPECT_TRUE(app::executable_scenario(
            profile.at("draft").get<unsigned>(),
            profile.at("scenario").get<std::string>()));
    }
    EXPECT_EQ(health.at("executable_profiles").at(0).at("draft"), 18);
    EXPECT_EQ(health.at("executable_profiles").at(0).at("transport"), "native-quic");
    EXPECT_EQ(health.at("executable_profiles").at(0).at("scenario"),
              "subscribe-to-publisher-track");
    EXPECT_FALSE(health.at("executable_profiles").at(0).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(1).at("draft"), 18);
    EXPECT_EQ(health.at("executable_profiles").at(1).at("scenario"),
              "subscribe-again-to-established-publisher-track");
    EXPECT_FALSE(health.at("executable_profiles").at(1).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(2).at("draft"), 21);
    EXPECT_EQ(health.at("executable_profiles").at(2).at("transport"), "native-quic");
    EXPECT_EQ(health.at("executable_profiles").at(2).at("scenario"),
              "d21-publisher-request-stream-placement");
    EXPECT_FALSE(health.at("executable_profiles").at(2).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(3).at("scenario"),
              "d21-setup-unknown-options");
    EXPECT_FALSE(health.at("executable_profiles").at(3).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(4).at("scenario"),
              "d21-setup-duplicate-unknown-options");
    EXPECT_FALSE(health.at("executable_profiles").at(4).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(5).at("scenario"),
              "d21-server-sends-authority");
    EXPECT_FALSE(health.at("executable_profiles").at(5).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(6).at("scenario"),
              "d21-server-sends-path");
    EXPECT_FALSE(health.at("executable_profiles").at(6).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(7).at("scenario"),
              "fetch-publisher-track-range");
    EXPECT_FALSE(health.at("executable_profiles").at(7).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(8).at("scenario"),
              "subscribe-namespace-at-publisher");
    EXPECT_FALSE(health.at("executable_profiles").at(8).at("configured"));
    EXPECT_EQ(health.at("executable_profiles").at(9).at("scenario"),
              "subscribe-tracks-at-publisher");
    EXPECT_FALSE(health.at("executable_profiles").at(9).at("configured"));
    for (std::size_t index = 0; index < 10; ++index) {
        const auto& profile = health.at("executable_profiles").at(index + 10);
        EXPECT_EQ(profile.at("transport"), "webtransport");
        EXPECT_EQ(profile.at("draft"),
                  health.at("executable_profiles").at(index).at("draft"));
        EXPECT_FALSE(profile.at("configured"));
    }
    for (std::size_t index = 0; index < observed_profiles; ++index) {
        const auto& profile = health.at("executable_profiles").at(index + observed_profiles);
        EXPECT_EQ(profile.at("mode"), "driven");
        EXPECT_EQ(profile.at("transport"),
                  health.at("executable_profiles").at(index).at("transport"));
        EXPECT_FALSE(profile.at("configured"));
    }

    const auto drafts = get_json("/api/v1/drafts");
    ASSERT_EQ(drafts.at("drafts").size(), 2);
    EXPECT_EQ(drafts.at("drafts").at(0).at("requirement_count"), 598);
    EXPECT_EQ(drafts.at("drafts").at(1).at("requirement_count"), 638);
    EXPECT_TRUE(drafts.at("drafts").at(0).at("complete"));

    const auto report = client_->Get("/results");
    ASSERT_TRUE(report);
    EXPECT_NE(report->body.find("No runs have been created."), std::string::npos);
}

TEST_F(HttpApiTest, PublishesAuditableCompletenessByDraftAndTransport) {
    const auto document = get_json("/results/completeness.json");
    EXPECT_EQ(document.at("schema_version"), 1);
    EXPECT_EQ(document.at("source_revision"), test_build().source_revision);
    ASSERT_EQ(document.at("drafts").size(), 2);
    for (const unsigned draft : {18u, 21u}) {
        const auto current = catalog(draft);
        const auto bindings = draft == 18
            ? requirements::draft18_executable_bindings()
            : requirements::draft21_executable_bindings();
        const auto audit = requirements::audit_completeness(
            *current, bindings, app::executable_scenarios(draft));
        const auto& item = document.at("drafts").at(draft == 18 ? 0 : 1);
        EXPECT_EQ(item.at("draft"), draft);
        EXPECT_EQ(item.at("source_sha256"), current->source_sha256);
        EXPECT_EQ(item.at("catalog_rows"), current->requirements.size());
        EXPECT_EQ(item.at("required_covered"), audit.required_covered);
        EXPECT_EQ(item.at("required_total"), audit.required_total);
        EXPECT_EQ(item.at("optional_covered"), audit.optional_covered);
        EXPECT_EQ(item.at("optional_total"), audit.optional_total);
        EXPECT_EQ(item.at("evaluator_complete"), audit.complete());
        ASSERT_EQ(item.at("transports").size(), 2);
        for (const auto& transport : item.at("transports")) {
            EXPECT_TRUE(transport.at("transport") == "native-quic" ||
                        transport.at("transport") == "webtransport");
            EXPECT_EQ(transport.at("run_count"), 0);
            EXPECT_EQ(transport.at("observed_requirement_count"), 0);
        }
        for (const auto& residual : item.at("classified_residuals")) {
            EXPECT_FALSE(residual.at("reason").get<std::string>().empty());
            EXPECT_FALSE(residual.at("section").get<std::string>().empty());
            EXPECT_GT(residual.at("first_line").get<std::size_t>(), 0);
        }
    }
    const auto page = client_->Get("/results");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("/results/completeness.json"), std::string::npos);
    for (const unsigned draft : {18u, 21u}) {
        const auto current = catalog(draft);
        const auto audit = requirements::audit_completeness(
            *current,
            draft == 18 ? requirements::draft18_executable_bindings()
                        : requirements::draft21_executable_bindings(),
            app::executable_scenarios(draft));
        EXPECT_NE(page->body.find(std::to_string(audit.required_covered) + "/" +
                                  std::to_string(audit.required_total)),
                  std::string::npos);
    }
}

TEST_F(HttpApiTest, ScoredRowWithoutEvaluatorEvidenceRemainsNotRun) {
    const auto source = catalog(18);
    const auto binding = requirements::draft18_executable_bindings().front();
    app::RunConfig config{app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
                          app::RunMode::Observed, {binding.scenario_id}, 1s};
    const auto id = store_->create_run(config);
    const std::vector<requirements::Outcome> outcomes{
        {binding.requirement_id, requirements::OutcomeState::Pass}};
    store_->finalize(id, requirements::score(*source, outcomes), outcomes);

    const auto document = get_json("/results/completeness.json");
    const auto& native = document.at("drafts").at(0).at("transports").at(0);
    const auto& webtransport = document.at("drafts").at(0).at("transports").at(1);
    EXPECT_EQ(native.at("run_count"), 1);
    EXPECT_EQ(native.at("scored_rows"), 1);
    EXPECT_EQ(native.at("observed_requirement_count"), 0);
    EXPECT_FALSE(native.at("execution_consistent"));
    EXPECT_TRUE(std::any_of(native.at("execution_findings").begin(),
                            native.at("execution_findings").end(),
                            [&](const Json& finding) {
        return finding.at("code") == "missing_evaluator_evidence" &&
               finding.at("run_id") == id &&
               finding.at("requirement_id") == binding.requirement_id;
    }));
    EXPECT_EQ(webtransport.at("run_count"), 0);
    EXPECT_EQ(webtransport.at("observed_requirement_count"), 0);
    const auto& not_run = native.at("not_run");
    EXPECT_TRUE(std::any_of(not_run.begin(), not_run.end(), [&](const Json& row) {
        return row.at("requirement_id") == binding.requirement_id &&
               !row.at("reason").get<std::string>().empty() &&
               !row.at("section").get<std::string>().empty();
    }));
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
    const auto unsupported = client_->Post("/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(unsupported);
    EXPECT_EQ(unsupported->status, 422);
    EXPECT_EQ(Json::parse(unsupported->body).at("error").at("code"), "unsupported_run_config");
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
    const app::RunConfig stored_config{
        app::DraftVersion::Draft21, app::TransportKind::WebTransport,
        app::RunMode::Driven, {"session/setup", "publisher/object"}, 12345ms};
    const auto id = store_->create_run(stored_config);

    storage::EvidenceEvent event;
    event.monotonic_time_ns = 11;
    event.wall_time_unix_ns = 22;
    event.kind = std::string{"kind<script>\0", 13};
    event.detail = "detail&\"'";
    event.scenario_id = "session/setup";
    store_->append_events(id, std::vector{event});

    const auto second_id = store_->create_run(stored_config);

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
    EXPECT_EQ(stop->status, 503);
    EXPECT_EQ(Json::parse(stop->body).at("error").at("code"),
              "publisher_listener_unavailable");
}

TEST_F(HttpApiTest, RejectsExecutableRunWhenListenerIsUnconfigured) {
    const Json request = {
        {"draft", 18}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", Json::array({"subscribe-to-publisher-track"})},
        {"timeout_ms", 1000},
        {"track", {{"namespace_hex", Json::array({"006e", "ff"})},
                   {"name_hex", "7800"}}}};
    const auto response = client_->Post(
        "/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503) << response->body;
    EXPECT_EQ(Json::parse(response->body).at("error").at("code"),
              "publisher_listener_unavailable");
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
}

TEST_F(HttpApiTest, EveryRawProbeAcceptsObservedRequestWithoutTrack) {
    const auto health = get_json("/healthz");
    for (const auto& profile : health.at("executable_profiles")) {
        const auto draft = profile.at("draft").get<unsigned>();
        const auto scenario = profile.at("scenario").get<std::string>();
        if (profile.at("mode") != "observed" || app::scenario_requires_track(draft, scenario)) continue;
        const Json request = {{"draft", draft}, {"transport", profile.at("transport")},
            {"mode", "observed"}, {"scenarios", Json::array({scenario})}, {"timeout_ms", 1000}};
        const auto response = client_->Post("/api/v1/runs", request.dump(), "application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 503) << scenario << ": " << response->body;
        EXPECT_EQ(Json::parse(response->body).at("error").at("code"), "publisher_listener_unavailable");
    }
}

TEST_F(HttpApiTest, RawContextFamiliesReachListenerAndValidateEverySelection) {
    Json request = {{"draft",21},{"transport","native-quic"},{"mode","observed"},
        {"scenarios",Json::array({"d21-duplicate-request-goaway","d21-goaway-on-distinct-request-streams"})},
        {"timeout_ms",1000}};
    const auto post = [&](int expected) {
        const auto response = client_->Post("/api/v1/runs",request.dump(),"application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status,expected) << response->body;
        EXPECT_EQ(store_->list({1,0}).total,0u);
    };
    // No fixture is needed for either GOAWAY context; both reach listener setup.
    post(503);
    request["scenarios"][1]="d21-subscriber-sends-publish-state-notify";
    post(400);
    request["track"]={{"namespace_hex",Json::array({"6e"})},{"name_hex","74"}};
    post(503);
    // Fixture constraints of a later context apply before allocating a listener.
    request["track"]["namespace_hex"]=Json::array({"2e73657373696f6e"});
    request["track"]["name_hex"]="";
    post(400);
    request["track"]={{"namespace_hex",Json::array({"6e"})},{"name_hex","74"}};
    request["scenarios"][1]="d21-publisher-request-stream-placement";
    post(422); // Typed controller combinations do not supply raw transcripts.
    request["scenarios"][1]="not-an-executable-scenario";
    post(422);
}

TEST_F(HttpApiTest, RawContextFamiliesRejectEmptyDuplicateAndExcessiveSelections) {
    for (const auto& scenarios : std::vector<Json>{Json::array(),
             Json::array({"d21-duplicate-request-goaway","d21-duplicate-request-goaway"}),
             Json(std::vector<std::string>(101,"d21-duplicate-request-goaway"))}) {
        const Json request={{"draft",21},{"transport","native-quic"},{"mode","observed"},
            {"scenarios",scenarios},{"timeout_ms",1000}};
        const auto response=client_->Post("/api/v1/runs",request.dump(),"application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status,400)<<response->body;
        EXPECT_EQ(store_->list({1,0}).total,0u);
    }
}

TEST_F(HttpApiTest, ExposesDuplicateSubscriptionScenarioAsExecutable) {
    const auto health = get_json("/healthz");
    bool listed = false;
    for (const auto& profile : health.at("executable_profiles")) {
        if (profile.at("draft") == 18 &&
            profile.at("scenario") ==
                "subscribe-again-to-established-publisher-track") {
            listed = true;
            EXPECT_FALSE(profile.at("configured").get<bool>());
        }
    }
    EXPECT_TRUE(listed);
    const Json request = {
        {"draft", 18}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", Json::array(
            {"subscribe-again-to-established-publisher-track"})},
        {"timeout_ms", 1000},
        {"track", {{"namespace_hex", Json::array({"6e"})},
                   {"name_hex", "78"}}}};
    const auto response = client_->Post(
        "/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503) << response->body;
    EXPECT_EQ(Json::parse(response->body).at("error").at("code"),
              "publisher_listener_unavailable");
}

TEST_F(HttpApiTest, ExposesPublisherFetchScenarioAsExecutable) {
    const auto health = get_json("/healthz");
    bool listed = false;
    for (const auto& profile : health.at("executable_profiles")) {
        if (profile.at("draft") == 18 &&
            profile.at("scenario") == "fetch-publisher-track-range") {
            listed = true;
            EXPECT_FALSE(profile.at("configured").get<bool>());
        }
    }
    EXPECT_TRUE(listed);
    const Json request = {
        {"draft", 18}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", Json::array({"fetch-publisher-track-range"})},
        {"timeout_ms", 1000},
        {"track", {{"namespace_hex", Json::array({"6e"})},
                   {"name_hex", "78"}}}};
    const auto response = client_->Post(
        "/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503) << response->body;
    EXPECT_EQ(Json::parse(response->body).at("error").at("code"),
              "publisher_listener_unavailable");
}

TEST_F(HttpApiTest, RejectsMalformedOrOversizedTrackFixtures) {
    const Json base = {
        {"draft", 18}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", Json::array({"subscribe-to-publisher-track"})},
        {"timeout_ms", 1000}};
    const std::vector<Json> fixtures{
        Json{{"namespace_hex", Json::array({"0"})}, {"name_hex", "78"}},
        Json{{"namespace_hex", Json::array({"zz"})}, {"name_hex", "78"}},
        Json{{"namespace_hex", Json::array({""})}, {"name_hex", "78"}},
        Json{{"namespace_hex", Json::array()},
             {"name_hex", std::string(8194, 'a')}},
        Json{{"namespace_hex", std::vector<std::string>(33, "61")},
             {"name_hex", "78"}}};
    for (const auto& fixture : fixtures) {
        auto request = base;
        request["track"] = fixture;
        const auto response = client_->Post(
            "/api/v1/runs", request.dump(), "application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 400) << response->body;
        EXPECT_EQ(Json::parse(response->body).at("error").at("code"),
                  "invalid_run_config");
    }
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
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

TEST_F(HttpApiTest, ExportsCompleteResultRowsAndScenarioTap) {
    app::RunConfig config{app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
                          app::RunMode::Observed, {"subscribe-to-publisher-track"}, 1s};
    const auto id = store_->create_run(config);
    const requirements::ScoreSummary score{requirements::RunVerdict::Error,
                                            {0, 0}, {0, 0}, {0, 0}};
    store_->finalize(id, score, {});

    const auto document = get_json("/results/" + id + ".json");
    EXPECT_EQ(document.at("schema_version"), 1);
    EXPECT_EQ(document.at("requirements").size(), 598);
    EXPECT_TRUE(document.at("requirements").at(0).at("outcome").is_null());
    EXPECT_EQ(document.at("run").at("verdict"), "error");

    const auto tap = client_->Get("/results/" + id + ".tap");
    ASSERT_TRUE(tap);
    EXPECT_EQ(tap->status, 200);
    EXPECT_EQ(tap->get_header_value("Content-Type"), "text/plain; charset=utf-8");
    EXPECT_EQ(tap->body.rfind("TAP version 14\n1..1\nnot ok 1 - ", 0), 0);
    EXPECT_NE(tap->body.find("\"result\":\"error\""), std::string::npos);
    EXPECT_EQ(get_json("/results/unknown.tap", 404).at("error").at("code"),
              "run_not_found");
}

TEST_F(HttpApiTest, RendersFilteredRequirementDetailWithEscapedEvidence) {
    const auto source = catalog(18);
    const auto found = std::find_if(
        source->requirements.begin(), source->requirements.end(),
        [](const requirements::Requirement& row) {
            return row.applicability == requirements::Applicability::Applicable &&
                   row.testability == requirements::Testability::Testable &&
                   row.strength == requirements::Strength::Must &&
                   std::find(row.scenarios.begin(), row.scenarios.end(),
                             "subscribe-to-publisher-track") != row.scenarios.end();
        });
    ASSERT_NE(found, source->requirements.end());
    app::RunConfig config{app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
                          app::RunMode::Observed, {"subscribe-to-publisher-track"}, 1s};
    const auto id = store_->create_run(config);
    storage::EvidenceEvent event;
    event.kind = "peer_setup_received";
    event.detail = "<script>alert('bad')</script>&";
    event.requirement_id = found->id;
    store_->append_events(id, std::vector{event});
    const requirements::ScoreSummary score{requirements::RunVerdict::Incomplete,
                                            {10, 20}, {10, 20}, {10, 20}};
    store_->finalize(id, score,
                     std::vector<requirements::Outcome>{
                         {found->id, requirements::OutcomeState::Pass}});

    const auto response = client_->Get("/results/" + id);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->get_header_value("Content-Type"), "text/html; charset=utf-8");
    EXPECT_NE(response->body.find("Required score"), std::string::npos);
    EXPECT_NE(response->body.find("10/20"), std::string::npos);
    EXPECT_NE(response->body.find("<th scope=\"col\">Requirement"), std::string::npos);
    EXPECT_NE(response->body.find("<details>"), std::string::npos);
    EXPECT_NE(response->body.find("background:#fff;color:#17212b"),
              std::string::npos);
    EXPECT_NE(response->body.find("summary:focus"), std::string::npos);
    EXPECT_NE(response->body.find("&lt;script&gt;"), std::string::npos);
    EXPECT_EQ(response->body.find("<script>"), std::string::npos);
    EXPECT_NE(response->body.find("/results/" + id + ".json"), std::string::npos);
    EXPECT_NE(response->body.find("/results/" + id + ".tap"), std::string::npos);

    const auto filtered = client_->Get("/results/" + id + "?outcome=pass");
    ASSERT_TRUE(filtered);
    EXPECT_EQ(filtered->status, 200);
    EXPECT_NE(filtered->body.find(found->id), std::string::npos);
    EXPECT_NE(filtered->body.find("Rows shown: 1"), std::string::npos);
    const auto section = client_->Get("/results/" + id +
                                       "?outcome=pass&section=5.1&strength=MUST");
    ASSERT_TRUE(section);
    EXPECT_EQ(section->status, 200);
    EXPECT_NE(section->body.find("Rows shown: 1"), std::string::npos);
    const auto scenario = client_->Get(
        "/results/" + id +
        "?outcome=pass&scenario=subscribe-to-publisher-track");
    ASSERT_TRUE(scenario);
    EXPECT_EQ(scenario->status, 200);
    EXPECT_NE(scenario->body.find("Rows shown: 1"), std::string::npos);
    const auto invalid = get_json("/results/" + id + "?outcome=unknown", 400);
    EXPECT_EQ(invalid.at("error").at("code"), "invalid_report_filter");
    EXPECT_EQ(get_json("/results/" + id + "?strength=MUST%0Ainjected", 400)
                  .at("error").at("code"),
              "invalid_report_filter");
    const auto list = client_->Get("/results");
    ASSERT_TRUE(list);
    EXPECT_NE(list->body.find("href=\"/results/" + id + "\""),
              std::string::npos);
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

TEST_F(HttpApiTest, DiscoveryOverlapRejectsUnconstructibleFixturesBeforeListenerAllocation) {
    for(const auto& scenario:std::vector<std::pair<unsigned,std::string>>{
        {18,"receive-overlapping-subscribe-namespace-in-same-session"},
        {21,"d21-discovery-update-independent-overlap-spaces"}}) {
        for(const auto& fields:std::vector<std::vector<std::string>>{std::vector<std::string>(32,"61"),{std::string(8190,'6')},{"2e"}}) {
            const Json request={{"draft",scenario.first},{"transport","native-quic"},{"mode","observed"},
                {"scenarios",Json::array({scenario.second})},{"timeout_ms",1000},
                {"track",{{"namespace_hex",fields},{"name_hex",""}}}};
            const auto response=client_->Post("/api/v1/runs",request.dump(),"application/json");
            ASSERT_TRUE(response);
            EXPECT_EQ(response->status,400)<<response->body;
        }
        const auto health=get_json("/healthz");
        std::set<std::pair<std::string,std::string>> combinations;
        for(const auto& profile:health.at("executable_profiles"))if(profile.at("draft")==scenario.first && profile.at("scenario")==scenario.second) {
            EXPECT_TRUE(combinations.emplace(profile.at("transport").get<std::string>(),profile.at("mode").get<std::string>()).second);
        }
        EXPECT_EQ(combinations.size(),4u);
        const Json valid={{"draft",scenario.first},{"transport","native-quic"},{"mode","observed"},
            {"scenarios",Json::array({scenario.second})},{"timeout_ms",1000},
            {"track",{{"namespace_hex",Json::array()},{"name_hex",""}}}};
        const auto response=client_->Post("/api/v1/runs",valid.dump(),"application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status,503)<<response->body;
    }
}

TEST_F(HttpApiTest, FirstFetchProfilesAdvertiseUniqueContextsAndPreflightFixtures) {
    for (const auto& scenario : std::vector<std::pair<unsigned,std::string>>{
        {18,"fetch-known-first-object-with-nonzero-group-and-object-ids"},{21,"d21-fetch-first-object-flags"},
        {18,"fetch-multiple-published-groups-in-each-explicit-order"},
        {21,"d21-fetch-ascending-groups"},{21,"d21-fetch-descending-groups"},
        {21,"d21-fetch-default-group-order"},{21,"d21-publish-state-notify-on-fetch"},
        {21,"d21-subscriber-sends-publish-state-notify"},
        {21,"d21-publish-established-subscriber-sends-publish-state-notify"},
        {18,"publish-and-retrieve-same-object-and-track-immutable-properties"},
        {18,"repeat-immutable-property-with-alternative-varint-encodings-available"},
        {18,"publish-object-with-immutable-properties"},
        {21,"d21-immutable-property-repeat"},{21,"d21-repeat-object-retrieval"},
        {21,"d21-object-immutable-property-singleton"}}) {
        const auto health = get_json("/healthz");
        std::set<std::pair<std::string,std::string>> combinations;
        for (const auto& profile : health.at("executable_profiles")) {
            if (profile.at("draft")==scenario.first && profile.at("scenario")==scenario.second) {
                EXPECT_TRUE(combinations.emplace(profile.at("transport").get<std::string>(),profile.at("mode").get<std::string>()).second);
            }
        }
        EXPECT_EQ(combinations.size(),4u);
        const Json missing = {{"draft",scenario.first},{"transport","native-quic"},{"mode","observed"},
            {"scenarios",Json::array({scenario.second})},{"timeout_ms",1000}};
        const auto missing_response = client_->Post("/api/v1/runs",missing.dump(),"application/json");
        ASSERT_TRUE(missing_response);
        EXPECT_EQ(missing_response->status,400)<<missing_response->body;
        const auto request = [&](Json fields,const char* name) {
            return Json{{"draft",scenario.first},{"transport","native-quic"},{"mode","observed"},
                {"scenarios",Json::array({scenario.second})},{"timeout_ms",1000},
                {"track",{{"namespace_hex",std::move(fields)},{"name_hex",name}}}};
        };
        for (const auto& invalid : std::vector<Json>{request(Json::array({"2e"}),"78"),request(Json::array({"2e73657373696f6e"}),"")}) {
            const auto response = client_->Post("/api/v1/runs",invalid.dump(),"application/json");
            ASSERT_TRUE(response);
            EXPECT_EQ(response->status,400)<<response->body;
        }
        // Valid input reaches the unconfigured listener, proving parser acceptance.
        const auto valid = request(Json::array({"6e"}),"74");
        const auto response = client_->Post("/api/v1/runs",valid.dump(),"application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status,503)<<response->body;
    }
}

}  // namespace moq::interop::http
