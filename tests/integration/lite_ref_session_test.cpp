// The reference publisher's sessions (L2c) against the production NativeRunManager on the loopback: the conforming
// publisher must judge a scenario's rows as Pass over native QUIC AND over WebTransport, and one named defect must
// fail exactly its row.
#include "lite_ref_session.h"
#include "support/lite_run_live.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

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

// The runner's STOP_SENDING or RESET_STREAM with a code the publisher does not know is answered by the publisher with
// a reset of its own stream (draft 4.4); on WebTransport that reset follows the peer's STOP_SENDING, which the session
// must not treat as the end of the stream's send side.
TEST(LiteRefSession, AWebTransportPublisherAnswersAStopSendingWithAReset) {
    for (const auto transport : {app::TransportKind::NativeQuic, app::TransportKind::WebTransport}) {
        SCOPED_TRACE(transport == app::TransportKind::WebTransport ? "webtransport" : "native_quic");
        const auto run = run_reference(transport, "l06-errors-unknown-reset-code");
        ASSERT_EQ(run.state, storage::RunState::Finalized);
        EXPECT_EQ(state_of(run, "L06-4-4-MUST-030"), OutcomeState::Pass);
        EXPECT_EQ(state_of(run, "L06-4-4-MUST-NOT-032"), OutcomeState::Pass);
    }
}

// A reference publisher run as the shipped binary: a child process whose standard output is read when it ends.
class ChildPublisher {
public:
    ChildPublisher(const std::string& binary, const std::string& url) {
        int fds[2];
        if (::pipe(fds) != 0) return;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        const char* argv[] = {binary.c_str(), "--connect", url.c_str(), nullptr};
        if (posix_spawn(&pid_, binary.c_str(), &actions, nullptr, const_cast<char* const*>(argv), environ) != 0) pid_ = 0;
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]);
        out_ = fds[0];
        ::fcntl(out_, F_SETFL, ::fcntl(out_, F_GETFL, 0) | O_NONBLOCK);
    }
    ~ChildPublisher() {
        if (pid_ > 0 && !exited_) {
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, &status_, 0);
        }
        if (out_ >= 0) ::close(out_);
    }
    // Called by drive_contexts in its loop.
    bool step() {
        char buffer[512];
        for (;;) {
            const auto n = ::read(out_, buffer, sizeof(buffer));
            if (n <= 0) break;
            output_.append(buffer, static_cast<std::size_t>(n));
        }
        if (pid_ > 0 && !exited_ && ::waitpid(pid_, &status_, WNOHANG) == pid_) exited_ = true;
        return !exited_;
    }
    bool wait_for_exit(std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (step() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        return exited_;
    }
    [[nodiscard]] int exit_code() const { return WIFEXITED(status_) ? WEXITSTATUS(status_) : -1; }
    [[nodiscard]] const std::string& output() const { return output_; }

private:
    pid_t pid_{0};
    int out_{-1};
    int status_{0};
    bool exited_{false};
    std::string output_;
};

TEST(LiteRefSession, TheShippedBinaryRunsAContextAndEndsWithOneJsonLine) {
    for (const auto transport : {app::TransportKind::NativeQuic, app::TransportKind::WebTransport}) {
        SCOPED_TRACE(transport == app::TransportKind::WebTransport ? "webtransport" : "native_quic");
        auto store = memory_store();
        auto runner = manager(store);
        const auto started = runner.start(lite_config({"l06-setup-stream"}, 8000ms, transport, pinned_fixture()));
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        std::shared_ptr<ChildPublisher> child;
        drive_contexts(store, started.id, started.endpoint.port, 1, [&](std::uint16_t port, unsigned) {
            child = std::make_shared<ChildPublisher>(MOQ_INTEROP_LITE_REF_PUBLISHER, endpoint(transport, port));
            return child;
        });
        (void)runner.stop(started.id);
        ASSERT_NE(child, nullptr);
        ASSERT_TRUE(child->wait_for_exit(15000ms)) << child->output();
        EXPECT_EQ(child->exit_code(), 0);
        const auto line = nlohmann::json::parse(child->output());
        EXPECT_EQ(line.at("defect"), "none");
        EXPECT_EQ(line.at("transport"), transport == app::TransportKind::WebTransport ? "webtransport" : "native_quic");
        EXPECT_EQ(state_of(store->load(started.id), "L06-3-1-MUST-014"), OutcomeState::Pass);
    }
}

}  // namespace
}  // namespace moq::interop
