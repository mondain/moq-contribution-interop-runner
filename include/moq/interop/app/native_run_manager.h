#pragma once

#include "moq/interop/app/types.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/scenarios/draft21_contribution.h"
#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/storage/run_store.h"
#include "moq/interop/transport/native_quic_listener.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    std::optional<std::uint64_t> unknown_auth_token_alias_compatibility_code{};
    // Credential (token type 0, Section 8.9) the publisher's authorization
    // policy is configured to refuse. Discovery authorization probes score a
    // pass or failure only when it is set; the operator controls the policy.
    std::optional<std::string> denied_authorization_token{};
    // Credentials, for a Token Type the publisher under test is configured to
    // understand, used by the draft-21 token rows D21-8-9-MUST-270 (invalid) and
    // D21-8-9-MUST-273 (expired) and their draft-18 twins D18-10-2-2-MUST-008 and
    // D18-10-2-2-MUST-010. The runner cannot create these itself.
    std::optional<scenarios::Draft21TokenCredential> invalid_auth_token{};
    std::optional<scenarios::Draft21TokenCredential> expired_auth_token{};
};

enum class RunStartStatus {
    Started,
    Unsupported,
    InvalidConfig,
    PortExhausted,
    ListenerError,
    // Every selected scenario needs a capability the publisher declared absent; nothing
    // was started. RunStartResult::scenario and ::capability name the first such scenario.
    ScenarioRequiresCapability,
};

struct RunStartResult {
    RunStartStatus status{RunStartStatus::ListenerError};
    RunId id;
    transport::BoundEndpoint endpoint;
    std::string url{};
    std::string path{};
    std::string protocol{};
    std::string scenario{};
    std::string capability{};
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
    // The raw-probe definition a run would execute for `id`, or nothing when `id`
    // is not a raw probe. Used by tests that inspect what a scenario sends.
    static std::optional<scenarios::RawProbeDefinition> resolve_probe(
        const NativeRunManagerConfig& manager_config, const RunConfig& run_config,
        std::string_view id);
    [[nodiscard]] bool supports(DraftVersion draft) const noexcept;
    [[nodiscard]] bool supports_driven() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::app
