// The run manager runs moq-lite only when it was constructed with the moq-lite-06 catalog (L1d Task 9): without
// it a lite start is refused as Unsupported before anything is bound or stored; with it the run starts.
#include "moq/interop/app/lite_run.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>

namespace moq::interop {
namespace {

app::NativeRunManagerConfig config() {
    return app::NativeRunManagerConfig{
        .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
        .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
        .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
        .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"};
}

TEST(MoqLiteStartGuard, WithoutALiteCatalogStartIsUnsupportedBeforeBindingOrStoringAnything) {
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(draft18, nullptr, store, config());
    EXPECT_FALSE(manager.supports(app::DraftVersion::MoqLite06));

    for (const auto transport : {app::TransportKind::NativeQuic, app::TransportKind::WebTransport}) {
        app::RunConfig run{app::DraftVersion::MoqLite06, transport, app::RunMode::Observed,
                           {"l06-setup-stream"}, std::chrono::milliseconds{1000}, std::nullopt, {}};
        EXPECT_EQ(manager.start(run).status, app::RunStartStatus::Unsupported);
    }
    EXPECT_TRUE(store->list({}).items.empty());
}

TEST(MoqLiteStartGuard, WithALiteCatalogTheRunStarts) {
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    auto lite = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{106, "test", false, {}});
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(draft18, nullptr, store, config(), nullptr, lite);
    EXPECT_TRUE(manager.supports(app::DraftVersion::MoqLite06));
    app::RunConfig run{app::DraftVersion::MoqLite06, app::TransportKind::NativeQuic, app::RunMode::Observed,
                       {"l06-setup-stream"}, std::chrono::milliseconds{1000}, std::nullopt, {}};
    const auto started = manager.start(run);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    EXPECT_TRUE(manager.stop(started.id));
    const auto stored = store->load(started.id);
    EXPECT_EQ(stored.config.draft, app::DraftVersion::MoqLite06);
    EXPECT_EQ(stored.state, storage::RunState::Finalized);
}

// L1e: the endpoint a started lite run returns. The session path /moq and query token=l1d are the fixed constants of
// src/app/lite_run.cpp, on both transports: WebTransport carries them in the URL (path is the URL path, without the
// query) and native QUIC has no URL field in the start result (the moql:// URI is in each context_ready event).
TEST(MoqLiteStartGuard, AStartedRunNamesTheFixedSessionPathAndQuery) {
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    auto lite = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{106, "test", false, {}});
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(draft18, nullptr, store, config(), nullptr, lite);
    for (const auto transport : {app::TransportKind::NativeQuic, app::TransportKind::WebTransport}) {
        const auto url = app::lite_session_url(transport);
        EXPECT_TRUE(url.has_path);
        EXPECT_EQ(url.path, "/moq");
        EXPECT_EQ(url.query, "token=l1d");
        app::RunConfig run{app::DraftVersion::MoqLite06, transport, app::RunMode::Observed,
                           {"l06-setup-stream"}, std::chrono::milliseconds{1000}, std::nullopt, {}};
        const auto started = manager.start(run);
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        const auto authority = "127.0.0.1:" + std::to_string(started.endpoint.port);
        if (transport == app::TransportKind::WebTransport) {
            EXPECT_EQ(started.url, "https://" + authority + "/moq?token=l1d");
            EXPECT_EQ(started.path, "/moq");
            EXPECT_EQ(started.protocol, "moq-lite-06");
        } else {
            EXPECT_TRUE(started.url.empty());
        }
        EXPECT_EQ(app::lite_endpoint_uri(transport, authority),
                  (transport == app::TransportKind::WebTransport ? "https://" : "moql://") + authority +
                      "/moq?token=l1d");
        EXPECT_TRUE(manager.stop(started.id));
    }
}

// The manager answers an unknown lite scenario id InvalidConfig (L1d) where the MoQ Transport drafts answer
// Unsupported; the HTTP API validates the id first (422 unsupported_run_config for every draft), so the difference
// never reaches a client (tests/integration/http_api_lite_test.cpp).
TEST(MoqLiteStartGuard, AnUnknownLiteScenarioIsInvalidConfigAtTheManager) {
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    auto lite = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{106, "test", false, {}});
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(draft18, nullptr, store, config(), nullptr, lite);
    app::RunConfig run{app::DraftVersion::MoqLite06, app::TransportKind::NativeQuic, app::RunMode::Observed,
                       {"l06-no-such-scenario"}, std::chrono::milliseconds{1000}, std::nullopt, {}};
    EXPECT_EQ(manager.start(run).status, app::RunStartStatus::InvalidConfig);
    EXPECT_TRUE(store->list({}).items.empty());
}

}  // namespace
}  // namespace moq::interop
