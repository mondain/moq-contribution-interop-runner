#include "moq/interop/app/publisher_driver.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace moq::interop::app {
namespace {

class DriverDirectory {
public:
    DriverDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("moq-interop-driver " + std::to_string(stamp));
        std::filesystem::create_directory(path_);
    }
    ~DriverDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

DriverRequest request(const DriverDirectory& directory, std::string mode) {
    DriverRequest value;
    value.executable = std::filesystem::path{MOQ_INTEROP_PROJECT_SOURCE_DIR} /
        "tests/support/fake_driver.sh";
    value.arguments = {std::move(mode)};
    value.run_id = "run-7";
    value.scenario_id = "subscribe-to-publisher-track";
    value.endpoint = "https://127.0.0.1:4443/moq";
    value.draft = DraftVersion::Draft18;
    value.transport = TransportKind::WebTransport;
    value.track = {{{std::string{"n\0s", 3}}}, std::string{"x\xff", 2}};
    value.fixture = directory.path() / "sample media.mp4";
    value.tls_ca = directory.path() / "ca cert.pem";
    value.log_dir = directory.path() / "publisher logs";
    value.scenario_timeout = std::chrono::milliseconds{2500};
    value.process_timeout = std::chrono::milliseconds{500};
    value.termination_grace = std::chrono::milliseconds{20};
    return value;
}

DriverResult wait_for(PublisherDriver& driver, DriverHandle handle) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        auto result = driver.poll(handle);
        if (result.status != DriverStatus::Running) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return driver.stop(handle);
}

TEST(PublisherDriver, WritesVersionedBinarySafeRequestAndHashesLogs) {
    DriverDirectory directory;
    PublisherDriver driver;
    const auto started = driver.start(request(directory, "success"));
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    const auto result = wait_for(driver, started.handle);
    EXPECT_EQ(result.status, DriverStatus::Exited);
    ASSERT_TRUE(result.exit_code.has_value());
    EXPECT_EQ(*result.exit_code, 0);
    EXPECT_EQ(result.stdout_log.bytes, 13u);
    EXPECT_EQ(result.stdout_log.sha256,
              "651ee044702aa550d9ce704e236d94fd1ca85571b65c965a5b111a441c1f1bb5");

    std::ifstream input(directory.path() / "publisher logs/request.json");
    ASSERT_TRUE(input.good());
    const auto json = nlohmann::json::parse(input);
    EXPECT_EQ(json.at("schema_version"), 1);
    EXPECT_EQ(json.at("run_id"), "run-7");
    EXPECT_EQ(json.at("scenario_id"), "subscribe-to-publisher-track");
    EXPECT_EQ(json.at("endpoint"), "https://127.0.0.1:4443/moq");
    EXPECT_EQ(json.at("draft"), 18);
    EXPECT_EQ(json.at("transport"), "webtransport");
    EXPECT_EQ(json.at("namespace_hex"), nlohmann::json::array({"6e0073"}));
    EXPECT_EQ(json.at("track_name_hex"), "78ff");
    EXPECT_EQ(json.at("fixture"),
              (directory.path() / "sample media.mp4").string());
    EXPECT_EQ(json.at("tls_ca"),
              (directory.path() / "ca cert.pem").string());
    EXPECT_EQ(json.at("scenario_timeout_ms"), 2500);
    EXPECT_EQ(json.at("process_timeout_ms"), 500);
}

TEST(PublisherDriver, ReportsMissingExecutableWithoutShellFallback) {
    DriverDirectory directory;
    PublisherDriver driver;
    auto value = request(directory, "success");
    value.executable = directory.path() / "missing program";
    const auto started = driver.start(value);
    EXPECT_EQ(started.status, DriverStartStatus::SpawnFailed);
    EXPECT_FALSE(started.handle.valid());
}

TEST(PublisherDriver, PreservesEarlyExitAsProcessResult) {
    DriverDirectory directory;
    PublisherDriver driver;
    const auto started = driver.start(request(directory, "early-exit"));
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    const auto result = wait_for(driver, started.handle);
    EXPECT_EQ(result.status, DriverStatus::Exited);
    EXPECT_EQ(result.exit_code, 7);
    EXPECT_FALSE(result.stderr_log.sha256.empty());
}

TEST(PublisherDriver, StopPreservesAlreadyExitedChildStatus) {
    DriverDirectory directory;
    PublisherDriver driver;
    const auto started = driver.start(request(directory, "early-exit"));
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    const auto result = driver.stop(started.handle);
    EXPECT_EQ(result.status, DriverStatus::Exited);
    EXPECT_EQ(result.exit_code, 7);
}

TEST(PublisherDriver, StopRetiresCompletedHandle) {
    DriverDirectory directory;
    PublisherDriver driver;
    const auto started = driver.start(request(directory, "success"));
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    const auto completed = wait_for(driver, started.handle);
    ASSERT_EQ(completed.status, DriverStatus::Exited);
    const auto stopped = driver.stop(started.handle);
    EXPECT_EQ(stopped.status, DriverStatus::Exited);
    EXPECT_EQ(stopped.stdout_log.sha256, completed.stdout_log.sha256);
    EXPECT_EQ(driver.poll(started.handle).status, DriverStatus::Error);
}

TEST(PublisherDriver, DoesNotOverwriteExistingRequestOrLogs) {
    DriverDirectory directory;
    PublisherDriver driver;
    auto value = request(directory, "success");
    std::filesystem::create_directory(value.log_dir);
    {
        std::ofstream existing(value.log_dir / "request.json");
        existing << "original";
    }
    const auto started = driver.start(value);
    EXPECT_EQ(started.status, DriverStartStatus::SpawnFailed);
    std::ifstream existing(value.log_dir / "request.json");
    std::string content;
    existing >> content;
    EXPECT_EQ(content, "original");
}

TEST(PublisherDriver, TimesOutAndReapsAChild) {
    DriverDirectory directory;
    PublisherDriver driver;
    auto value = request(directory, "sleep");
    value.process_timeout = std::chrono::milliseconds{25};
    const auto started = driver.start(value);
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    const auto result = wait_for(driver, started.handle);
    EXPECT_EQ(result.status, DriverStatus::TimedOut);
    EXPECT_TRUE(result.term_signal == SIGTERM || result.term_signal == SIGKILL);
}

TEST(PublisherDriver, EscalatesFromIgnoredTermToKill) {
    DriverDirectory directory;
    PublisherDriver driver;
    const auto started = driver.start(request(directory, "ignore-term"));
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto result = driver.stop(started.handle);
    EXPECT_EQ(result.status, DriverStatus::Stopped);
    EXPECT_EQ(result.term_signal, SIGKILL);
}

TEST(PublisherDriver, InvalidUtf8StaysInOpaqueLogNotResultJson) {
    DriverDirectory directory;
    PublisherDriver driver;
    const auto started = driver.start(request(directory, "invalid-utf8"));
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    const auto result = wait_for(driver, started.handle);
    EXPECT_EQ(result.status, DriverStatus::Exited);
    EXPECT_EQ(result.stderr_log.bytes, 3u);
    const auto json = nlohmann::json::parse(serialize_driver_result(result));
    EXPECT_EQ(json.at("status"), "exited");
    EXPECT_EQ(json.at("stderr_log").at("bytes"), 3);
    EXPECT_FALSE(json.dump().empty());
}

// A runner socket without close-on-exec must not reach the publisher: the child would
// keep the UDP port bound after the runner closes its own descriptor, and the runner's
// next bind of the same reserved port would fail with EADDRINUSE.
TEST(PublisherDriver, ChildDoesNotInheritTheRunnersSockets) {
    const int leaked = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(leaked, 0);
    DriverDirectory directory;
    PublisherDriver driver;
    const auto started = driver.start(request(directory, "list-sockets"));
    ASSERT_EQ(started.status, DriverStartStatus::Started) << started.error;
    const auto result = wait_for(driver, started.handle);
    ::close(leaked);
    EXPECT_EQ(result.status, DriverStatus::Exited);
    std::ifstream output(directory.path() / "publisher logs/stdout.bin");
    const std::string text((std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("done"), std::string::npos) << text;
    EXPECT_EQ(text.find("inherited"), std::string::npos) << text;
}

TEST(PublisherDriver, SerializesTheDraftAsAnIntegerForMoqtAndAsTextForMoqLite) {
    DriverDirectory directory;
    auto value = request(directory, "ok");
    const std::pair<DraftVersion, nlohmann::json> expected[] = {
        {DraftVersion::Draft18, 18}, {DraftVersion::Draft21, 21}, {DraftVersion::Draft22, 22},
        {DraftVersion::MoqLite06, "moq-lite-06"}};
    for (const auto& [draft, wanted] : expected) {
        value.draft = draft;
        const auto json = nlohmann::json::parse(serialize_driver_request(value));
        EXPECT_EQ(json.at("draft"), wanted);
    }
    value.draft = DraftVersion::Draft22;
    EXPECT_NE(serialize_driver_request(value).find("\"draft\":22,"), std::string::npos);
    value.draft = DraftVersion::MoqLite06;
    EXPECT_NE(serialize_driver_request(value).find("\"draft\":\"moq-lite-06\","), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::app
