// The run manager must never start a listener or store a run for a moq-lite run in this sub-project.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace moq::interop {
namespace {

TEST(MoqLiteStartGuard, StartThrowsBeforeBindingOrStoringAnything) {
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(
        draft18, nullptr, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
            .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
            .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"});
    EXPECT_FALSE(manager.supports(app::DraftVersion::MoqLite06));

    for (const auto transport : {app::TransportKind::NativeQuic, app::TransportKind::WebTransport}) {
        app::RunConfig config{app::DraftVersion::MoqLite06, transport, app::RunMode::Observed,
                              {"any"}, std::chrono::milliseconds{1000}, std::nullopt, {}};
        EXPECT_THROW((void)manager.start(config), std::logic_error);
    }
    EXPECT_TRUE(store->list({}).items.empty());
}

}  // namespace
}  // namespace moq::interop
