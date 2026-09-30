#pragma once

#include "moq/interop/app/types.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/storage/run_store.h"
#include "moq/interop/transport/native_quic_listener.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace moq::interop::app {

struct NativeRunManagerConfig {
    std::string bind_address{"127.0.0.1"};
    std::string advertised_address;
    std::uint16_t port_start{0};
    std::uint16_t port_end{0};
    std::size_t maximum_active_runs{1};
    std::filesystem::path certificate_path;
    std::filesystem::path private_key_path;
    std::vector<std::string> webtransport_allowed_origins{};
    bool webtransport_require_origin{false};
    std::filesystem::path driver_executable{};
    std::vector<std::string> driver_arguments{};
    std::filesystem::path driver_fixture{};
    std::filesystem::path driver_tls_ca{};
    std::filesystem::path driver_log_root{};
};

enum class RunStartStatus {
    Started,
    Unsupported,
    InvalidConfig,
    PortExhausted,
    ListenerError,
};

struct RunStartResult {
    RunStartStatus status{RunStartStatus::ListenerError};
    RunId id;
    transport::BoundEndpoint endpoint;
    std::string url{};
    std::string path{};
    std::string protocol{};
};

class NativeRunManager {
public:
    NativeRunManager(
        std::shared_ptr<const requirements::RequirementCatalog> catalog,
        std::shared_ptr<storage::RunStore> store,
        NativeRunManagerConfig config);
    NativeRunManager(
        std::shared_ptr<const requirements::RequirementCatalog> draft18,
        std::shared_ptr<const requirements::RequirementCatalog> draft21,
        std::shared_ptr<storage::RunStore> store,
        NativeRunManagerConfig config);
    ~NativeRunManager();

    NativeRunManager(const NativeRunManager&) = delete;
    NativeRunManager& operator=(const NativeRunManager&) = delete;

    RunStartResult start(const RunConfig& config);
    bool stop(const RunId& id);
    [[nodiscard]] bool supports(DraftVersion draft) const noexcept;
    [[nodiscard]] bool supports_driven() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::app
