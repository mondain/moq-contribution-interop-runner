#pragma once

#include "moq/interop/app/types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace moq::interop::app {

struct DriverRequest {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    RunId run_id;
    // The scenario as it was selected for the run (a d22- id for a draft 22 run, never the draft 21
    // implementation id the runner executes it with), the same id the run's stored events carry.
    std::string scenario_id;
    std::string endpoint;
    // The draft the run is on the wire (the ALPN the runner accepts), not the family it executes on.
    DraftVersion draft{DraftVersion::Draft18};
    TransportKind transport{TransportKind::NativeQuic};
    TrackFixture track;
    std::filesystem::path fixture;
    std::filesystem::path tls_ca;
    std::filesystem::path log_dir;
    std::chrono::milliseconds scenario_timeout{0};
    std::chrono::milliseconds process_timeout{0};
    std::chrono::milliseconds termination_grace{100};
};

struct DriverHandle {
    std::uint64_t value{0};
    [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

enum class DriverStartStatus { Started, InvalidRequest, SpawnFailed };
enum class DriverStatus { Running, Exited, Signaled, TimedOut, Stopped, Error };

struct DriverLogMetadata {
    std::filesystem::path path;
    std::uint64_t bytes{0};
    std::string sha256;
};

struct DriverResult {
    DriverStatus status{DriverStatus::Error};
    std::optional<int> exit_code;
    std::optional<int> term_signal;
    DriverLogMetadata stdout_log;
    DriverLogMetadata stderr_log;
    std::string error;
};

struct DriverStartResult {
    DriverStartStatus status{DriverStartStatus::InvalidRequest};
    DriverHandle handle;
    std::string error;
};

// The request file handed to the driver: `scenario_id` is the requested scenario id and `draft` the wire draft
// (see DriverRequest), so a draft 22 run's driver sees draft 22 and d22- ids only.
std::string serialize_driver_request(const DriverRequest& request);
std::string serialize_driver_result(const DriverResult& result);

class PublisherDriver {
public:
    PublisherDriver();
    ~PublisherDriver();
    PublisherDriver(const PublisherDriver&) = delete;
    PublisherDriver& operator=(const PublisherDriver&) = delete;

    DriverStartResult start(const DriverRequest& request);
    DriverResult poll(DriverHandle handle);
    // Returns the final result and retires the handle; callers must stop even
    // after poll reports completion so long-running services release state.
    DriverResult stop(DriverHandle handle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::app
