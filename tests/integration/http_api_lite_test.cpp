// moq-lite-06 runs through the HTTP API (L1e Task 2), wired as src/app/main.cpp wires them: one lite catalog object
// shared by the run manager (scoring) and the server (listing, results). The in-process conforming publisher of the
// lite live tests (tests/support/lite_run_live.h) plays the publisher over the loopback, dialing the session URL the
// API returned; the tests read the run back through every results route.
#include "json.h"
#include "moq/interop/app/lite_run.h"
#include "moq/interop/app/lite_scenarios.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/http/server.h"
#include "moq/interop/requirements/execution_audit.h"
#include "moq/interop/requirements/lite_evaluators.h"
#include "support/lite_run_live.h"

#include <gtest/gtest.h>
#include <unistd.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop {
namespace {

using namespace std::chrono_literals;
using namespace lite_live;
using Json = nlohmann::json;

constexpr std::string_view kUnreviewedPrefix = "Unreviewed:";

class LiteHttpApi : public ::testing::Test {
protected:
    void SetUp() override { build(true, true); }

    // (Re)builds the manager and the server. `server_lite`: the server holds the lite catalog; `manager_lite`: the
    // run manager does.
    void build(bool server_lite, bool manager_lite, bool driver = false) {
        api_.reset();
        server_.reset();
        runs_.reset();
        store_ = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        auto draft18 = catalog(18);
        auto draft21 = catalog(21);
        auto draft22 = catalog(22);
        lite_ = catalog(106);
        auto config = manager_config();
        if (driver) {
            config.driver_executable = "/bin/false";
            config.driver_log_root = std::filesystem::temp_directory_path();
        }
        runs_ = std::make_shared<app::NativeRunManager>(draft18, draft21, store_, config, draft22,
                                                        manager_lite ? lite_ : nullptr);
        http::ServerConfig server;
        server.port = 0;
        server.draft22_catalog = draft22;
        if (server_lite) server.moqlite06_catalog = lite_;
        server_ = std::make_unique<http::HttpServer>(draft18, draft21, store_, app::BuildInfo{"test", "test", {}},
                                                     server, runs_);
        ASSERT_TRUE(server_->start());
        api_ = std::make_unique<httplib::Client>("127.0.0.1", server_->port());
        api_->set_connection_timeout(2s);
        api_->set_read_timeout(10s);
    }
    void TearDown() override {
        api_.reset();
        server_.reset();
        runs_.reset();
    }

    Json get(const std::string& path, int status = 200) {
        const auto response = api_->Get(path);
        EXPECT_TRUE(response) << path;
        if (!response) return {};
        EXPECT_EQ(response->status, status) << path << ": " << response->body;
        return Json::parse(response->body);
    }

    std::string get_text(const std::string& path) {
        const auto response = api_->Get(path);
        EXPECT_TRUE(response) << path;
        if (!response) return {};
        EXPECT_EQ(response->status, 200) << path << ": " << response->body;
        return response->body;
    }

    std::pair<int, Json> post(const Json& body) {
        const auto response = api_->Post("/api/v1/runs", body.dump(), "application/json");
        EXPECT_TRUE(response);
        if (!response) return {0, {}};
        return {response->status, Json::parse(response->body)};
    }

    static Json lite_request(std::vector<std::string> ids, std::string transport = "native-quic",
                             std::string mode = "observed", bool track = false) {
        Json body{{"draft", "moq-lite-06"}, {"transport", std::move(transport)}, {"mode", std::move(mode)},
                  {"scenarios", std::move(ids)}, {"timeout_ms", 4000}};
        // broadcast "demo/live", track "video" (the live publisher's fixture)
        if (track) body["track"] = {{"namespace_hex", Json::array({"64656d6f", "6c697665"})}, {"name_hex", "766964656f"}};
        return body;
    }

    std::shared_ptr<storage::SqliteRunStore> store_;
    std::shared_ptr<const requirements::RequirementCatalog> lite_;
    std::shared_ptr<app::NativeRunManager> runs_;
    std::unique_ptr<http::HttpServer> server_;
    std::unique_ptr<httplib::Client> api_;
};

std::unique_ptr<LiteQuicPublisher> conforming(std::uint16_t port, unsigned) {
    return std::make_unique<LiteQuicPublisher>(port, publisher_config());
}

// ---- listing ------------------------------------------------------------------------------------------------------

TEST_F(LiteHttpApi, TheDraftsListingNamesMoqLiteAsRunnableAndStaged) {
    const auto drafts = get("/api/v1/drafts").at("drafts");
    ASSERT_EQ(drafts.size(), 4u);
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_TRUE(drafts.at(index).at("draft").is_number_integer());
        EXPECT_FALSE(drafts.at(index).contains("staged")) << drafts.at(index).dump();
        EXPECT_FALSE(drafts.at(index).contains("staged_note"));
    }
    const auto& lite = drafts.at(3);
    EXPECT_EQ(lite.at("draft"), "moq-lite-06");
    EXPECT_TRUE(lite.at("runnable"));
    EXPECT_FALSE(lite.at("complete"));
    EXPECT_TRUE(lite.at("staged"));
    EXPECT_NE(lite.at("staged_note").get<std::string>().find("never pass"), std::string::npos);
    EXPECT_EQ(lite.at("requirement_count"), 212);
    EXPECT_EQ(lite.at("source_sha256"), lite_->source_sha256);
    EXPECT_EQ(lite.dump().find("\"draft\":106"), std::string::npos) << lite.dump();
}

TEST_F(LiteHttpApi, TheRequirementsRouteServesTheLiteCatalog) {
    const auto page = get("/api/v1/requirements?draft=moq-lite-06&limit=100");
    EXPECT_EQ(page.at("draft"), "moq-lite-06");
    EXPECT_EQ(page.at("pagination").at("total"), 212);
    EXPECT_EQ(page.at("items").size(), 100u);
    EXPECT_TRUE(page.at("items").at(0).at("id").get<std::string>().starts_with("L06-"));
    const auto last = get("/api/v1/requirements?draft=moq-lite-06&offset=200&limit=100");
    EXPECT_EQ(last.at("items").size(), 12u);
    EXPECT_TRUE(last.at("pagination").at("next_offset").is_null());
    // The integer form is still refused, and the message lists the drafts this server serves.
    const auto refused = get("/api/v1/requirements?draft=106", 400);
    EXPECT_EQ(refused.at("error").at("code"), "unsupported_draft");
    EXPECT_EQ(refused.at("error").at("message"), "draft must be 18, 21, 22 or moq-lite-06.");
    EXPECT_EQ(get("/api/v1/requirements?draft=moq-lite-07", 400).at("error"), refused.at("error"));
}

TEST_F(LiteHttpApi, HealthListsMoqLiteApartFromTheMoqTransportDraftsAndProfiles) {
    const auto health = get("/healthz");
    // supported_drafts and executable_profiles stay MoQ Transport only (integer drafts), byte-identical to a server
    // without the lite catalog; moq-lite-06 is listed in the two additive fields.
    EXPECT_EQ(health.at("supported_drafts"), Json::array({18, 21, 22}));
    EXPECT_EQ(health.at("supported_lite_drafts"), Json::array({"moq-lite-06"}));
    for (const auto& profile : health.at("executable_profiles"))
        EXPECT_TRUE(profile.at("draft").is_number_integer()) << profile.dump();
    std::set<std::string> observed;
    const auto& lite_profiles = health.at("lite_executable_profiles");
    for (std::size_t index = 0; index < lite_profiles.size(); ++index) {
        const auto& profile = lite_profiles.at(index);
        EXPECT_EQ(profile.at("draft"), "moq-lite-06");
        EXPECT_FALSE(profile.at("requires_fetch"));
        // Observed first, then the same profiles driven.
        EXPECT_EQ(profile.at("mode"), index < lite_profiles.size() / 2 ? "observed" : "driven") << index;
        const auto id = profile.at("scenario").get<std::string>();
        EXPECT_TRUE(app::lite_executable_scenario(id).has_value()) << id;
        // Observed lite profiles run on this manager; driven ones need --driver-executable.
        EXPECT_EQ(profile.at("configured").get<bool>(), profile.at("mode") == "observed") << profile.dump();
        if (profile.at("mode") == "observed") observed.insert(id + "@" + profile.at("transport").get<std::string>());
    }
    // 19 scenarios on both transports, observed and driven.
    EXPECT_EQ(observed.size(), 2 * app::kLiteExecutableScenarios.size());
    EXPECT_EQ(lite_profiles.size(), 4 * app::kLiteExecutableScenarios.size());
    const auto with_lite = health.at("executable_profiles").dump();
    build(false, true);
    const auto without = get("/healthz");
    EXPECT_EQ(without.at("executable_profiles").dump(), with_lite);
    EXPECT_EQ(without.at("supported_drafts"), health.at("supported_drafts"));
    EXPECT_FALSE(without.contains("supported_lite_drafts"));
    EXPECT_FALSE(without.contains("lite_executable_profiles"));
}

TEST_F(LiteHttpApi, DrivenLiteProfilesAreConfiguredWheneverADriverIsSet) {
    build(true, true, true);
    const auto health = get("/healthz");
    for (const auto& profile : health.at("lite_executable_profiles")) EXPECT_TRUE(profile.at("configured"));
}

// ---- one run through every route ----------------------------------------------------------------------------------

TEST_F(LiteHttpApi, AnObservedNativeRunIsCreatedPlayedAndReadThroughEveryRoute) {
    const auto [status, created] = post(lite_request({"l06-setup-stream", "l06-setup-client-path"}));
    ASSERT_EQ(status, 201) << created.dump();
    EXPECT_EQ(created.at("run").at("config").at("draft"), "moq-lite-06");
    EXPECT_EQ(created.at("run").at("state"), "active");
    const auto& endpoint = created.at("publisher_endpoint");
    EXPECT_EQ(endpoint.at("alpn"), "moq-lite-06");
    EXPECT_EQ(endpoint.at("address"), "127.0.0.1");
    // No new API field: native QUIC returns address, port and alpn only (the moql:// URI is in context_ready).
    EXPECT_EQ(endpoint.size(), 3u) << endpoint.dump();
    const auto id = created.at("run").at("id").get<std::string>();
    const auto port = endpoint.at("port").get<std::uint16_t>();
    drive_contexts(store_, id, port, 2, conforming);

    // The run record.
    const auto run = get("/api/v1/runs/" + id).at("run");
    EXPECT_EQ(run.at("state"), "finalized");
    EXPECT_EQ(run.at("config").at("draft"), "moq-lite-06");
    EXPECT_EQ(run.at("verdict"), "incomplete");
    EXPECT_TRUE(run.at("staged"));
    EXPECT_FALSE(run.at("staged_note").get<std::string>().empty());
    EXPECT_EQ(run.at("outcomes").size(), 212u);
    std::map<std::string, std::string> states;
    for (const auto& outcome : run.at("outcomes"))
        states[outcome.at("requirement_id").get<std::string>()] = outcome.at("state").get<std::string>();
    EXPECT_EQ(states.at("L06-3-1-MUST-014"), "pass");
    EXPECT_EQ(states.at("L06-7-3-2-MUST-120"), "pass");
    EXPECT_EQ(states.at("L06-7-3-2-SHOULD-124"), "pass");
    EXPECT_EQ(states.at("L06-7-3-2-MUST-NOT-125"), "not_run");
    // The session URL the publisher was given, in the evidence.
    const auto events = get("/api/v1/runs/" + id + "/events?limit=100").at("items");
    const auto ready = std::find_if(events.begin(), events.end(),
                                    [](const Json& event) { return event.at("kind") == "context_ready"; });
    ASSERT_NE(ready, events.end());
    EXPECT_NE(ready->at("detail").get<std::string>().find(
                  "endpoint=moql://127.0.0.1:" + std::to_string(port) + "/moq?token=l1d "),
              std::string::npos)
        << ready->dump();
    const auto list = get("/api/v1/runs");
    EXPECT_EQ(list.at("items").at(0).at("config").at("draft"), "moq-lite-06");

    // The JSON export: every row, unreviewed rows not_run with their reason.
    const auto document = get("/results/" + id + ".json");
    EXPECT_EQ(document.at("run").at("config").at("draft"), "moq-lite-06");
    EXPECT_TRUE(document.at("staged"));
    EXPECT_EQ(document.at("draft_source_sha256"), lite_->source_sha256);
    ASSERT_EQ(document.at("requirements").size(), 212u);
    std::size_t unreviewed = 0;
    for (const auto& row : document.at("requirements")) {
        EXPECT_NE(row.at("outcome"), "error") << row.at("id");
        if (!row.contains("reviewed")) continue;
        ++unreviewed;
        EXPECT_FALSE(row.at("reviewed"));
        EXPECT_EQ(row.at("outcome"), "not_run") << row.at("id");
        EXPECT_TRUE(row.at("rationale").get<std::string>().starts_with(kUnreviewedPrefix)) << row.at("id");
    }
    EXPECT_EQ(unreviewed, 75u);
    EXPECT_EQ(document.dump().find("\"draft\":106"), std::string::npos);

    // TAP.
    const auto tap = get_text("/results/" + id + ".tap");
    EXPECT_TRUE(tap.starts_with("TAP version 14\n1..2\n# staged catalog:")) << tap;
    EXPECT_NE(tap.find("\"draft\":\"moq-lite-06\""), std::string::npos) << tap;
    EXPECT_NE(tap.find(" - l06-setup-client-path"), std::string::npos) << tap;

    // The HTML report (with and without a filter) and the run list.
    const auto page = get_text("/results/" + id);
    EXPECT_NE(page.find("Draft moq-lite-06;"), std::string::npos);
    EXPECT_NE(page.find("Staged catalog:"), std::string::npos);
    EXPECT_NE(page.find("incomplete"), std::string::npos);
    const auto filtered = get_text("/results/" + id + "?outcome=not_run&section=10");
    EXPECT_NE(filtered.find("Unreviewed: classification pending."), std::string::npos);
    const auto runs_page = get_text("/results");
    EXPECT_NE(runs_page.find("<td>moq-lite-06</td>"), std::string::npos);

    // The completeness inventory: a staged lite entry whose unreviewed rows are not_run, never not_testable.
    const auto completeness = get("/results/completeness.json");
    ASSERT_EQ(completeness.at("drafts").size(), 4u);
    const auto& entry = completeness.at("drafts").at(3);
    EXPECT_EQ(entry.at("draft"), "moq-lite-06");
    EXPECT_TRUE(entry.at("staged"));
    EXPECT_FALSE(entry.at("evaluator_complete"));
    std::size_t pending = 0;
    for (const auto& residual : entry.at("classified_residuals")) {
        const auto reason = residual.at("reason").get<std::string>();
        if (!reason.starts_with(kUnreviewedPrefix)) {
            EXPECT_NE(residual.at("classification"), "not_run") << residual.dump();
            continue;
        }
        ++pending;
        EXPECT_EQ(residual.at("classification"), "not_run") << residual.dump();
    }
    EXPECT_EQ(pending, 75u);
    const auto& native = entry.at("transports").at(0);
    EXPECT_EQ(native.at("transport"), "native-quic");
    EXPECT_EQ(native.at("run_count"), 1);
    EXPECT_TRUE(native.at("execution_consistent")) << native.at("execution_findings").dump();
    EXPECT_GE(native.at("observed_requirement_count").get<std::size_t>(), 4u);
    // The MoQ Transport entries keep their shape.
    for (std::size_t index = 0; index < 3; ++index) EXPECT_FALSE(completeness.at("drafts").at(index).contains("staged"));

    // The stored run audits clean with the lite bindings.
    const auto stored = store_->load(id);
    const auto audit = requirements::audit_execution(*lite_, requirements::lite_executable_bindings(),
                                                     std::span(&stored, 1));
    EXPECT_TRUE(audit.consistent());
    for (const auto& finding : audit.findings) ADD_FAILURE() << finding.code << " " << finding.requirement_id;
}

TEST_F(LiteHttpApi, AWebTransportRunReturnsTheSessionUrlAndJudgesRow125) {
    const auto [status, created] = post(lite_request({"l06-setup-client-path"}, "webtransport"));
    ASSERT_EQ(status, 201) << created.dump();
    const auto& endpoint = created.at("publisher_endpoint");
    const auto port = endpoint.at("port").get<std::uint16_t>();
    EXPECT_EQ(endpoint.at("alpn"), "h3");
    EXPECT_EQ(endpoint.at("protocol"), "moq-lite-06");
    EXPECT_EQ(endpoint.at("path"), "/moq");
    EXPECT_EQ(endpoint.at("url"), "https://127.0.0.1:" + std::to_string(port) + "/moq?token=l1d");
    const auto id = created.at("run").at("id").get<std::string>();
    const auto setup = test::lite::setup_stream(wire::moqlite06::SetupMessage{publisher_config().setup_parameters});
    drive_contexts(store_, id, port, 1, [&](std::uint16_t value, unsigned) {
        return std::make_unique<LiteWebTransportPublisher>(value, setup);
    });
    const auto run = get("/api/v1/runs/" + id).at("run");
    EXPECT_EQ(run.at("verdict"), "incomplete");
    std::map<std::string, std::string> states;
    for (const auto& outcome : run.at("outcomes"))
        states[outcome.at("requirement_id").get<std::string>()] = outcome.at("state").get<std::string>();
    EXPECT_EQ(states.at("L06-7-3-2-MUST-NOT-125"), "pass");
    EXPECT_EQ(states.at("L06-7-3-2-MUST-120"), "not_run");
    (void)get_text("/results/" + id);
    (void)get("/results/" + id + ".json");
    (void)get_text("/results/" + id + ".tap");
    const auto completeness = get("/results/completeness.json");
    EXPECT_EQ(completeness.at("drafts").at(3).at("transports").at(1).at("run_count"), 1);
}

TEST_F(LiteHttpApi, SeveralLiteScenariosAreAcceptedInOneRun) {
    // moq-lite scenarios are not MoQ Transport raw probes, yet each runs as its own context: the "typed scenario"
    // single-selection rule does not apply.
    const auto [status, created] = post(lite_request({"l06-setup-stream", "l06-setup-server-role"}));
    ASSERT_EQ(status, 201) << created.dump();
    EXPECT_TRUE(runs_->stop(created.at("run").at("id").get<std::string>()));
}

// ---- refusals -----------------------------------------------------------------------------------------------------

TEST_F(LiteHttpApi, AnUnknownScenarioIsRefusedLikeMoqTransport) {
    const auto [lite_status, lite] = post(lite_request({"l06-no-such-scenario"}));
    EXPECT_EQ(lite_status, 422);
    EXPECT_EQ(lite.at("error").at("code"), "unsupported_run_config");
    EXPECT_EQ(lite.at("error").at("message"),
              "Scenario 'l06-no-such-scenario' is not an executable scenario for draft moq-lite-06.");
    // The same status and code a MoQ Transport draft gives (the manager's InvalidConfig is never reached).
    auto moqt = lite_request({"l06-no-such-scenario"});
    moqt["draft"] = 18;
    const auto [moqt_status, moqt_error] = post(moqt);
    EXPECT_EQ(moqt_status, lite_status);
    EXPECT_EQ(moqt_error.at("error").at("code"), lite.at("error").at("code"));
    // A MoQ Transport id in a lite run, and a lite id in a MoQ Transport run.
    EXPECT_EQ(post(lite_request({"subscribe-to-publisher-track"})).first, 422);
    auto crossed = lite_request({"l06-setup-stream"});
    crossed["draft"] = 22;
    EXPECT_EQ(post(crossed).first, 422);
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
}

TEST_F(LiteHttpApi, ATrackScenarioWithoutTheFixtureIsRefusedLikeDraft22OwnScenarios) {
    const auto [lite_status, lite] = post(lite_request({"l06-subscribe-latest"}));
    EXPECT_EQ(lite_status, 400);
    EXPECT_EQ(lite.at("error").at("code"), "invalid_run_config");
    std::optional<std::string> own;
    for (const auto id : app::executable_scenarios(22)) {
        const auto traits = app::own_scenario_22(id);
        if (traits && traits->requires_track) {
            own = std::string(id);
            break;
        }
    }
    ASSERT_TRUE(own.has_value());
    auto d22 = lite_request({*own});
    d22["draft"] = 22;
    const auto [d22_status, d22_error] = post(d22);
    EXPECT_EQ(d22_status, lite_status);
    EXPECT_EQ(d22_error.at("error"), lite.at("error"));
    // A track-free lite scenario needs no fixture; a mixed selection does.
    EXPECT_EQ(post(lite_request({"l06-setup-stream", "l06-announce-prefix"})).first, 400);
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
}

TEST_F(LiteHttpApi, DrivenWithoutADriverIsRefusedLikeMoqTransport) {
    const auto [lite_status, lite] = post(lite_request({"l06-setup-stream"}, "native-quic", "driven", true));
    EXPECT_EQ(lite_status, 422);
    EXPECT_EQ(lite.at("error").at("code"), "unsupported_run_config");
    auto moqt = lite_request({"subscribe-to-publisher-track"}, "native-quic", "driven", true);
    moqt["draft"] = 18;
    const auto [moqt_status, moqt_error] = post(moqt);
    EXPECT_EQ(moqt_status, lite_status);
    EXPECT_EQ(moqt_error.at("error"), lite.at("error"));
    // With a driver, driven mode still needs the track fixture (400, as for MoQ Transport).
    build(true, true, true);
    const auto [status, error] = post(lite_request({"l06-setup-stream"}, "native-quic", "driven", false));
    EXPECT_EQ(status, 400);
    EXPECT_EQ(error.at("error").at("code"), "invalid_run_config");
}

TEST_F(LiteHttpApi, BadDraftValuesAndAnUnusableFixtureAreInvalid) {
    for (const auto& draft : {Json(106), Json("moq-lite-07"), Json("MOQ-LITE-06"), Json(106.0)}) {
        auto body = lite_request({"l06-setup-stream"});
        body["draft"] = draft;
        const auto [status, error] = post(body);
        EXPECT_EQ(status, 400) << draft.dump();
        EXPECT_EQ(error.at("error").at("code"), "invalid_run_config") << draft.dump();
        // This server serves moq-lite-06, so the message names it (a server without it keeps the old message).
        EXPECT_EQ(error.at("error").at("message"), "draft must be 18, 21, 22 or moq-lite-06.") << draft.dump();
    }
    // An empty track name is no broadcast track: the manager refuses it as invalid.
    auto body = lite_request({"l06-subscribe-latest"}, "native-quic", "observed", true);
    body["track"]["name_hex"] = "";
    const auto [status, error] = post(body);
    EXPECT_EQ(status, 400);
    EXPECT_EQ(error.at("error").at("code"), "invalid_run_config");
    EXPECT_EQ(store_->list({1, 0}).total, 0u);
}

TEST_F(LiteHttpApi, AServerWithoutTheLiteCatalogKeepsTheExistingRefusals) {
    build(false, true);
    const auto [status, error] = post(lite_request({"l06-setup-stream"}));
    EXPECT_EQ(status, 422);
    EXPECT_EQ(error.at("error").at("code"), "draft_not_runnable");
    EXPECT_EQ(error.at("error").at("message"), "Draft moq-lite-06 is not runnable on this runner.");
    EXPECT_EQ(get("/api/v1/drafts").at("drafts").size(), 3u);
    EXPECT_EQ(get("/healthz").at("supported_drafts"), Json::array({18, 21, 22}));
    EXPECT_EQ(get("/api/v1/requirements?draft=moq-lite-06", 400).at("error").at("message"),
              "draft must be 18, 21 or 22.");
    auto invalid = lite_request({"l06-setup-stream"});
    invalid["draft"] = 106;
    const auto [invalid_status, invalid_error] = post(invalid);
    EXPECT_EQ(invalid_status, 400);
    EXPECT_EQ(invalid_error.at("error").at("message"), "draft must be 18, 21 or 22.");
    const auto health = get("/healthz");
    EXPECT_FALSE(health.contains("supported_lite_drafts"));
    EXPECT_FALSE(health.contains("lite_executable_profiles"));
    // A stored lite run answers 409 on the catalog-backed routes.
    const auto id = store_->create_run(app::RunConfig{app::DraftVersion::MoqLite06, app::TransportKind::NativeQuic,
                                                      app::RunMode::Observed, {"l06-setup-stream"}, 1s});
    store_->finalize(id, requirements::ScoreSummary{}, {});
    for (const auto& path : {"/results/" + id + ".json", "/results/" + id + ".tap", "/results/" + id})
        EXPECT_EQ(get(path, 409).at("error").at("code"), "draft_catalog_not_configured") << path;
}

TEST_F(LiteHttpApi, AManagerWithoutTheLiteCatalogIsRefusedAsUnsupportedByTheListener) {
    build(true, false);
    const auto [status, error] = post(lite_request({"l06-setup-stream"}));
    EXPECT_EQ(status, 422);
    EXPECT_EQ(error.at("error").at("code"), "unsupported_run_config");
    EXPECT_EQ(error.at("error").at("message"),
              "Draft moq-lite-06 is not supported by this runner's publisher listener.");
}

// ---- startup (src/app/main.cpp) -------------------------------------------------------------------------------------

class TemporaryDirectory {
public:
    TemporaryDirectory() : path_(std::filesystem::temp_directory_path() / ("moq-lite-startup-" + std::to_string(::getpid()) +
                                                                          "-" + std::to_string(next_++))) {
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    inline static unsigned next_ = 0;
    std::filesystem::path path_;
};

struct StartupTree {
    TemporaryDirectory root;
    std::filesystem::path docs = root.path() / "docs";
    std::filesystem::path requirements = root.path() / "requirements";
    StartupTree() {
        std::filesystem::create_directories(docs);
        std::filesystem::create_directories(requirements);
        std::filesystem::copy_file(kRoot / "docs/draft-lcurley-moq-lite-06.txt", docs / "draft-lcurley-moq-lite-06.txt");
        std::filesystem::copy_file(kRoot / "requirements/draft-digests.json", requirements / "draft-digests.json");
        std::filesystem::copy_file(kRoot / "requirements/moq-lite-06.json", requirements / "moq-lite-06.json");
    }
    std::shared_ptr<const requirements::RequirementCatalog> load(std::string& log) const {
        std::ostringstream out;
        auto loaded = app::load_lite_catalog_if_available(docs, requirements, requirements / "draft-digests.json", out);
        log = out.str();
        return loaded;
    }
};

TEST(LiteStartup, TheCheckedInCatalogLoads) {
    const StartupTree tree;
    std::string log;
    const auto loaded = tree.load(log);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->draft, 106u);
    EXPECT_FALSE(loaded->complete);
    EXPECT_EQ(loaded->requirements.size(), 212u);
    EXPECT_TRUE(log.empty()) << log;
}

TEST(LiteStartup, AMissingCatalogOrDraftTextLeavesLiteUnavailable) {
    {
        const StartupTree tree;
        std::filesystem::remove(tree.requirements / "moq-lite-06.json");
        std::string log;
        EXPECT_FALSE(tree.load(log));
        EXPECT_NE(log.find("moq-lite-06 is unavailable"), std::string::npos) << log;
    }
    {
        const StartupTree tree;
        std::filesystem::remove(tree.docs / "draft-lcurley-moq-lite-06.txt");
        std::string log;
        EXPECT_FALSE(tree.load(log));
        EXPECT_NE(log.find("moq-lite-06 is unavailable"), std::string::npos) << log;
    }
    {
        // A draft text that fails its digest.
        const StartupTree tree;
        std::ofstream(tree.docs / "draft-lcurley-moq-lite-06.txt", std::ios::app) << "tampered\n";
        std::string log;
        EXPECT_FALSE(tree.load(log));
        EXPECT_NE(log.find("SHA-256 mismatch"), std::string::npos) << log;
    }
}

TEST(LiteStartup, APresentButInvalidCatalogIsAStartupError) {
    const StartupTree tree;
    std::ofstream(tree.requirements / "moq-lite-06.json", std::ios::trunc) << "{\"draft\": 106}";
    std::string log;
    EXPECT_ANY_THROW((void)tree.load(log));
}

}  // namespace
}  // namespace moq::interop
