#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/scenarios/draft18_contribution.h"
#include "moq/interop/storage/run_store.h"
#include "support/picoquic_client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

// Runs draft-18 contribution probes through the real run manager against a
// picoquic client that plays the publisher.
namespace moq::interop {
namespace {
using Bytes = std::vector<std::byte>;
using Client = transport::test::PicoquicTestClient;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
Bytes text(const std::string& value) {
    Bytes result;
    for (const auto c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

template <class Predicate>
bool pump_until(Client& client, Predicate predicate, std::chrono::milliseconds limit = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

template <class Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds limit = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool context_ready(const storage::RunRecord& run, std::string_view scenario, unsigned ordinal) {
    return std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
        return event.kind == "context_ready" && event.scenario_id == scenario &&
               event.detail.find("ordinal=" + std::to_string(ordinal)) != std::string::npos;
    });
}

requirements::OutcomeState outcome_of(const storage::RunRecord& run, std::string_view requirement) {
    const auto row = std::find_if(run.outcomes.begin(), run.outcomes.end(),
        [&](const auto& outcome) { return outcome.requirement_id == requirement; });
    EXPECT_NE(row, run.outcomes.end()) << requirement;
    return row == run.outcomes.end() ? requirements::OutcomeState::NotRun : row->state;
}

std::shared_ptr<const requirements::RequirementCatalog> load_catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / (draft == 18 ? "requirements/draft18.json" : "requirements/draft21.json")));
}

TEST(Draft18ContributionLive, RunsSetupUriAndSurvivalContextsThroughTheRunManager) {
    const auto draft18 = load_catalog(18);
    const auto draft21 = load_catalog(21);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(draft18, draft21, store,
        {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
         .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
         .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
         .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"});
    const std::vector<std::string> ids{"receive-setup-with-duplicate-unknown-options",
                                       "observe-publisher-setup-options",
                                       "connect-publisher-to-native-uri-with-query"};
    const auto started = manager.start({app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
        app::RunMode::Observed, ids, std::chrono::milliseconds(1500), std::nullopt});
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    const auto port = started.endpoint.port;
    const auto alpn = text("moqt-18");
    for (std::size_t index = 0; index < ids.size(); ++index) {
        SCOPED_TRACE(ids[index]);
        ASSERT_TRUE(wait_for([&] { return context_ready(store->load(started.id), ids[index],
                                                         static_cast<unsigned>(index + 1)); }));
        auto client = Client::create({.port = port, .alpn = alpn});
        ASSERT_NE(client, nullptr);
        // The runner's own SETUP arrives first on its control stream.
        ASSERT_TRUE(pump_until(*client, [&] { const auto s = client->stream(3); return s && s->data.size() >= 4; }));
        if (index == 0) {
            ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
            // Its discovery request follows; REQUEST_OK shows the runner's SETUP was handled.
            ASSERT_TRUE(pump_until(*client, [&] { const auto s = client->stream(1); return s && !s->data.empty(); }));
            const auto request = client->stream(1)->data;
            EXPECT_EQ(request.front(), std::byte{0x50});  // SUBSCRIBE_NAMESPACE
            ASSERT_TRUE(client->send_stream(1, bytes({7, 0, 1, 0}), false));
        } else if (index == 1) {
            ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        } else {
            const std::string authority = "127.0.0.1:" + std::to_string(port);
            const std::string path = "/moq?interop=1";
            Bytes payload = bytes({1, static_cast<unsigned>(path.size())});
            const auto path_bytes = text(path);
            payload.insert(payload.end(), path_bytes.begin(), path_bytes.end());
            payload.insert(payload.end(), {std::byte{4}, static_cast<std::byte>(authority.size())});
            const auto authority_bytes = text(authority);
            payload.insert(payload.end(), authority_bytes.begin(), authority_bytes.end());
            Bytes setup = bytes({0xaf, 0, 0, static_cast<unsigned>(payload.size())});
            setup.insert(setup.end(), payload.begin(), payload.end());
            ASSERT_TRUE(client->send_stream(2, setup, false));
        }
        ASSERT_TRUE(wait_for([&] {
            client->pump();
            const auto run = store->load(started.id);
            return index + 1 == ids.size() ? run.state == storage::RunState::Finalized
                                           : context_ready(run, ids[index + 1], static_cast<unsigned>(index + 2));
        }));
    }
    const auto run = store->load(started.id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(outcome_of(run, "D18-10-3-MUST-003"), requirements::OutcomeState::Pass);
    EXPECT_EQ(outcome_of(run, "D18-10-3-MUST-NOT-001"), requirements::OutcomeState::Pass);
    EXPECT_EQ(outcome_of(run, "D18-10-3-1-2-MUST-005"), requirements::OutcomeState::Pass);
    // Rows whose scenarios were not selected remain unscored.
    EXPECT_EQ(outcome_of(run, "D18-10-3-1-2-MUST-004"), requirements::OutcomeState::NotRun);
    // The URI the runner named for the query context is retained as evidence.
    EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
        return event.kind == "raw_probe_connection_uri" &&
               event.scenario_id == "connect-publisher-to-native-uri-with-query" &&
               event.detail.find("connection_uri=moqt://127.0.0.1:" + std::to_string(port) + "/moq?interop=1") !=
                   std::string::npos;
    }));
}

}  // namespace
}  // namespace moq::interop
