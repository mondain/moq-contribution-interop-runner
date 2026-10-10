// The reference publisher's sessions (L2c) against the production NativeRunManager on the loopback: the conforming
// publisher must judge a scenario's rows as Pass over native QUIC AND over WebTransport, and one named defect must
// fail exactly its row.
#include "lite_ref_session.h"
#include "support/lite_run_live.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop {
namespace {

using namespace lite_live;
using requirements::OutcomeState;

std::shared_ptr<storage::SqliteRunStore> memory_store() {
    return std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
}

app::NativeRunManager manager(const std::shared_ptr<storage::SqliteRunStore>& store) {
    return app::NativeRunManager(catalog(18), catalog(21), store, manager_config(), catalog(22), catalog(106));
}

app::TrackFixture pinned_fixture() { return app::TrackFixture{{"interop.hang"}, "0.m4s"}; }

std::string endpoint(app::TransportKind transport, std::uint16_t port) {
    return std::string(transport == app::TransportKind::WebTransport ? "https" : "moql") + "://127.0.0.1:" +
           std::to_string(port) + "/moq?token=l1d";
}

// Runs `scenario` against a reference publisher (with `extra` flags) and returns the stored run.
storage::RunRecord run_reference(app::TransportKind transport, std::string_view scenario,
                                 std::vector<std::string_view> extra = {}) {
    auto store = memory_store();
    auto runner = manager(store);
    const auto started =
        runner.start(lite_config({std::string(scenario)}, 30000ms, transport, pinned_fixture()));
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status != app::RunStartStatus::Started) return {};
    drive_contexts(store, started.id, started.endpoint.port, 1, [&](std::uint16_t port, unsigned) {
        std::vector<std::string_view> args{"--connect"};
        const auto url = endpoint(transport, port);
        args.push_back(url);
        args.insert(args.end(), extra.begin(), extra.end());
        const auto parsed = lite_ref::parse_options(args);
        EXPECT_TRUE(parsed.options.has_value()) << parsed.error;
        std::string error;
        auto session = parsed.options ? lite_ref::RefSession::dial(*parsed.options, error) : nullptr;
        EXPECT_NE(session, nullptr) << error;
        return session;
    });
    (void)runner.stop(started.id);
    return store->load(started.id);
}

TEST(LiteRefSession, TheConformingPublisherPassesTheSubscribeRowOnBothTransports) {
    for (const auto transport : {app::TransportKind::NativeQuic, app::TransportKind::WebTransport}) {
        SCOPED_TRACE(transport == app::TransportKind::WebTransport ? "webtransport" : "native_quic");
        const auto run = run_reference(transport, "l06-subscribe-latest");
        ASSERT_EQ(run.state, storage::RunState::Finalized);
        EXPECT_EQ(state_of(run, "L06-6-3-2-MUST-093"), OutcomeState::Pass);
        for (const auto& outcome : run.outcomes) EXPECT_NE(outcome.state, OutcomeState::Fail) << outcome.requirement_id;
    }
}

}  // namespace
}  // namespace moq::interop
