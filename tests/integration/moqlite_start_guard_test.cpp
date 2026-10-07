// The run manager runs moq-lite only when it was constructed with the moq-lite-06 catalog (L1d Task 9): without
// it a lite start is refused as Unsupported before anything is bound or stored; with it the run starts.
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

}  // namespace
}  // namespace moq::interop
