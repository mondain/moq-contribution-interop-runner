#include "moq/interop/app/publisher_driver.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace moq::interop::app {
namespace {

std::string hex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

const char* status_name(DriverStatus status) {
    switch (status) {
        case DriverStatus::Running: return "running";
        case DriverStatus::Exited: return "exited";
        case DriverStatus::Signaled: return "signaled";
        case DriverStatus::TimedOut: return "timed_out";
        case DriverStatus::Stopped: return "stopped";
        case DriverStatus::Error: return "error";
    }
    return "error";
}

DriverLogMetadata metadata(const std::filesystem::path& path) {
    DriverLogMetadata result;
    result.path = path;
    std::ifstream input(path, std::ios::binary);
    if (!input) return result;
    EVP_MD_CTX* digest = EVP_MD_CTX_new();
    if (!digest) return result;
    if (EVP_DigestInit_ex(digest, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(digest);
        return result;
    }
    char buffer[8192];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() != 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        result.bytes += count;
        if (EVP_DigestUpdate(digest, buffer, count) != 1) {
            EVP_MD_CTX_free(digest);
            return result;
        }
    }
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(digest, hash, &length) == 1)
        result.sha256 = hex(std::string(reinterpret_cast<char*>(hash), length));
    EVP_MD_CTX_free(digest);
    return result;
}

bool create_file(const std::filesystem::path& path, const std::string& data,
                 std::string& error) {
    const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = path.string() + ": " + std::strerror(errno);
        return false;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            error = path.string() + ": " + std::strerror(errno);
            close(fd);
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (close(fd) == 0) return true;
    error = path.string() + ": " + std::strerror(errno);
    return false;
}

int reap(pid_t pid, int flags, int& status) {
    int result;
    do { result = waitpid(pid, &status, flags); } while (result < 0 && errno == EINTR);
    return result;
}

}  // namespace

std::string serialize_driver_request(const DriverRequest& request) {
    nlohmann::json names = nlohmann::json::array();
    for (const auto& field : request.track.namespace_fields) names.push_back(hex(field));
    return nlohmann::json{
        {"schema_version", 1}, {"run_id", request.run_id},
        {"scenario_id", request.scenario_id}, {"endpoint", request.endpoint},
        {"draft", static_cast<unsigned>(request.draft)},
        {"transport", request.transport == TransportKind::WebTransport
                          ? "webtransport" : "native_quic"},
        {"namespace_hex", names}, {"track_name_hex", hex(request.track.track_name)},
        {"fixture", request.fixture.string()}, {"tls_ca", request.tls_ca.string()},
        {"log_dir", request.log_dir.string()},
        {"scenario_timeout_ms", request.scenario_timeout.count()},
        {"process_timeout_ms", request.process_timeout.count()}
    }.dump();
}

std::string serialize_driver_result(const DriverResult& result) {
    auto log_json = [](const DriverLogMetadata& log) {
        return nlohmann::json{{"path", log.path.string()}, {"bytes", log.bytes},
                              {"sha256", log.sha256}};
    };
    return nlohmann::json{
        {"status", status_name(result.status)}, {"exit_code", result.exit_code},
        {"term_signal", result.term_signal}, {"stdout_log", log_json(result.stdout_log)},
        {"stderr_log", log_json(result.stderr_log)}, {"error", result.error}
    }.dump();
}

struct PublisherDriver::Impl {
    struct Process {
        pid_t pid;
        std::filesystem::path stdout_path, stderr_path;
        std::chrono::steady_clock::time_point started;
        std::chrono::milliseconds timeout, grace;
        std::optional<DriverResult> final;
    };
    std::mutex mutex;
    std::map<std::uint64_t, Process> processes;
    std::uint64_t next_handle{1};

    DriverResult finish(Process& process, DriverStatus status, int child_status,
                        std::optional<int> sent_signal = {}) {
        DriverResult result;
        result.status = status;
        if (WIFEXITED(child_status)) result.exit_code = WEXITSTATUS(child_status);
        if (WIFSIGNALED(child_status)) result.term_signal = WTERMSIG(child_status);
        if (sent_signal) result.term_signal = sent_signal;
        result.stdout_log = metadata(process.stdout_path);
        result.stderr_log = metadata(process.stderr_path);
        process.final = result;
        return result;
    }

    DriverResult terminate(Process& process, DriverStatus reason) {
        int status = 0;
        const int initial = reap(process.pid, WNOHANG, status);
        if (initial == process.pid)
            return finish(process, WIFEXITED(status) ? DriverStatus::Exited :
                          DriverStatus::Signaled, status);
        if (initial < 0)
            return finish(process, DriverStatus::Error, 0);
        kill(-process.pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + process.grace;
        while (std::chrono::steady_clock::now() < deadline) {
            if (reap(process.pid, WNOHANG, status) == process.pid)
                return finish(process, reason, status, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        kill(-process.pid, SIGKILL);
        if (reap(process.pid, 0, status) < 0)
            return finish(process, DriverStatus::Error, 0);
        return finish(process, reason, status, SIGKILL);
    }
};

PublisherDriver::PublisherDriver() : impl_(std::make_unique<Impl>()) {}
PublisherDriver::~PublisherDriver() {
    std::lock_guard lock(impl_->mutex);
    for (auto& [id, process] : impl_->processes) {
        (void)id;
        if (!process.final) impl_->terminate(process, DriverStatus::Stopped);
    }
}

DriverStartResult PublisherDriver::start(const DriverRequest& request) {
    if (request.executable.empty() || request.run_id.empty() ||
        request.scenario_id.empty() || request.endpoint.empty() || request.log_dir.empty() ||
        request.process_timeout.count() <= 0 || request.termination_grace.count() < 0 ||
        request.scenario_timeout.count() < 0)
        return {DriverStartStatus::InvalidRequest, {}, "invalid driver request"};

    std::lock_guard lock(impl_->mutex);
    std::error_code ec;
    const auto directory = std::filesystem::absolute(request.log_dir, ec);
    if (ec) return {DriverStartStatus::SpawnFailed, {}, ec.message()};
    std::filesystem::create_directories(directory, ec);
    if (ec) return {DriverStartStatus::SpawnFailed, {}, ec.message()};
    auto normalized = request;
    normalized.log_dir = directory;
    const auto request_path = directory / "request.json";
    const auto stdout_path = directory / "stdout.bin";
    const auto stderr_path = directory / "stderr.bin";
    std::string error;
    if (!create_file(request_path, serialize_driver_request(normalized), error) ||
        !create_file(stdout_path, {}, error) ||
        !create_file(stderr_path, {}, error))
        return {DriverStartStatus::SpawnFailed, {}, error};

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(), O_WRONLY, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path.c_str(), O_WRONLY, 0600);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    std::vector<std::string> arguments{request.executable.string()};
    arguments.insert(arguments.end(), request.arguments.begin(), request.arguments.end());
    std::vector<char*> argv;
    for (auto& value : arguments) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** entry = environ; *entry; ++entry) {
        std::string value(*entry);
        if (value.rfind("MOQ_INTEROP_DRIVER_CONTRACT_VERSION=", 0) != 0 &&
            value.rfind("MOQ_INTEROP_DRIVER_REQUEST_FILE=", 0) != 0)
            environment.push_back(std::move(value));
    }
    environment.emplace_back("MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1");
    environment.emplace_back("MOQ_INTEROP_DRIVER_REQUEST_FILE=" + request_path.string());
    std::vector<char*> envp;
    for (auto& value : environment) envp.push_back(value.data());
    envp.push_back(nullptr);
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, request.executable.c_str(), &actions,
                                    &attributes, argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        return {DriverStartStatus::SpawnFailed, {}, std::strerror(spawned)};
    const DriverHandle handle{impl_->next_handle++};
    impl_->processes.emplace(handle.value, Impl::Process{
        pid, stdout_path, stderr_path, std::chrono::steady_clock::now(),
        request.process_timeout, request.termination_grace, {}});
    return {DriverStartStatus::Started, handle, {}};
}

DriverResult PublisherDriver::poll(DriverHandle handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->processes.find(handle.value);
    if (found == impl_->processes.end())
        return {DriverStatus::Error, {}, {}, {}, {}, "unknown handle"};
    auto& process = found->second;
    if (process.final) return *process.final;
    int status = 0;
    const int waited = reap(process.pid, WNOHANG, status);
    if (waited == process.pid)
        return impl_->finish(process, WIFEXITED(status) ? DriverStatus::Exited :
                            DriverStatus::Signaled, status);
    if (waited < 0) return impl_->finish(process, DriverStatus::Error, 0);
    if (std::chrono::steady_clock::now() - process.started >= process.timeout)
        return impl_->terminate(process, DriverStatus::TimedOut);
    return {DriverStatus::Running};
}

DriverResult PublisherDriver::stop(DriverHandle handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->processes.find(handle.value);
    if (found == impl_->processes.end())
        return {DriverStatus::Error, {}, {}, {}, {}, "unknown handle"};
    const auto result = found->second.final ? *found->second.final :
                        impl_->terminate(found->second, DriverStatus::Stopped);
    impl_->processes.erase(found);
    return result;
}

}  // namespace moq::interop::app
