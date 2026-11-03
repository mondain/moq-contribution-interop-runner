#include "moq/interop/app/draft23_run.h"
#include "moq/interop/app/publisher_driver.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/session/draft23_session.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <stdexcept>

namespace moq::interop::app {
namespace {
std::vector<std::byte> bytes(const std::string& s) {
    const auto view = std::as_bytes(std::span(s));
    return {view.begin(), view.end()};
}
}
bool valid_draft23_run(const RunConfig& config) {
    if (config.draft != DraftVersion::Draft23 || config.scenario_ids.size() != 1 ||
        !executable_scenario(23, config.scenario_ids.front()) || !config.track_fixture ||
        config.timeout < std::chrono::milliseconds(100) || config.timeout > std::chrono::hours(1)) return false;
    const auto& fixture = *config.track_fixture;
    if (fixture.namespace_fields.empty() || fixture.namespace_fields.size() > 32 || fixture.track_name.empty()) return false;
    std::size_t size = fixture.track_name.size();
    for (const auto& field : fixture.namespace_fields) { if (field.empty()) return false; size += field.size(); }
    return size <= 4096;
}
void run_draft23(const RunId& id, std::string authority, storage::RunStore& store,
                 const requirements::RequirementCatalog& catalog,
                 const NativeRunManagerConfig& manager, const std::atomic<bool>& stopped,
                 std::unique_ptr<transport::SessionTransport> listener, const RunConfig& config) {
    PublisherDriver driver;
    DriverHandle handle;
    bool operational_error = false;
    auto event = [&](std::string kind, std::string detail) {
        storage::EvidenceEvent e;
        e.kind = std::move(kind); e.detail = std::move(detail); e.scenario_id = config.scenario_ids.front();
        store.append_events(id, std::span(&e, 1));
    };
    try {
        session::Draft23SessionConfig settings;
        for (const auto& field : config.track_fixture->namespace_fields) settings.track_namespace.push_back(bytes(field));
        settings.track_name = bytes(config.track_fixture->track_name);
        settings.webtransport = config.transport == TransportKind::WebTransport;
        settings.subscribe_immediately = config.scenario_ids.front() == "d23-direct-subscribe-smoke";
        session::Draft23Session session(*listener, std::move(settings));
        const auto endpoint = std::string(config.transport == TransportKind::WebTransport ? "https://" : "moqt://") + authority + "/moq";
        event("context_start", "draft=23 protocol=moqt-23 endpoint=" + endpoint + " scoring=unscored");
        if (config.mode == RunMode::Driven) {
            DriverRequest request;
            request.executable = manager.driver_executable; request.arguments = manager.driver_arguments;
            request.run_id = id; request.scenario_id = config.scenario_ids.front(); request.endpoint = endpoint;
            request.draft = DraftVersion::Draft23; request.transport = config.transport;
            request.track = *config.track_fixture; request.fixture = manager.driver_fixture;
            request.tls_ca = manager.driver_tls_ca.empty() ? manager.certificate_path : manager.driver_tls_ca;
            request.log_dir = manager.driver_log_root / id / request.scenario_id;
            request.scenario_timeout = config.timeout; request.process_timeout = 2 * config.timeout + std::chrono::seconds(1);
            auto started = driver.start(request);
            if (started.status != DriverStartStatus::Started) throw std::runtime_error("publisher driver start failed: " + started.error);
            handle = started.handle;
        }
        auto deadline = std::chrono::steady_clock::now() + config.timeout;
        bool connected = false;
        while (!stopped && std::chrono::steady_clock::now() < deadline && !session.error()) {
            for (const auto& e : listener->poll(256)) session.on_event(e);
            session.flush();
            if (!connected && session.stats().connected) {
                connected = true;
                deadline = std::chrono::steady_clock::now() + config.timeout;
            }
            if (session.stats().setup_received && session.stats().subscription_established && session.stats().progressing()) break;
            if (handle.valid()) {
                auto result = driver.poll(handle);
                if (result.status != DriverStatus::Running) {
                    event("publisher_process", serialize_driver_result(result));
                    // A publisher that exits before the smoke goal has not demonstrated delivery.
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto& stats = session.stats();
        nlohmann::json summary{{"setup_received",stats.setup_received},{"namespace_announced",stats.namespace_announced},
            {"subscription_established",stats.subscription_established},{"objects",stats.objects},
            {"distinct_objects",stats.distinct_objects},{"payload_bytes",stats.payload_bytes},
            {"progressing",stats.progressing()},{"conformance_scored",false}};
        const bool delivered = stats.setup_received && stats.subscription_established && stats.progressing() && !session.error() && !stopped;
        summary["status"] = delivered ? "PASS" : "NOT_RUN";
        if (session.error()) summary["error"] = *session.error();
        event("draft23_smoke",summary.dump());
        if (!stats.connected) { operational_error = true; event("harness_error", "draft-23 publisher did not connect before deadline"); }
        if (stopped) { operational_error = true; event("run_stopped", "operator stopped draft-23 smoke"); }
    } catch (const std::exception& error) {
        operational_error = true;
        try { event("harness_error",error.what()); } catch (...) {}
    }
    if (handle.valid()) {
        const auto result = driver.stop(handle);
        try { event("publisher_process",serialize_driver_result(result)); } catch (...) { operational_error = true; }
        if (result.status == DriverStatus::Error || result.status == DriverStatus::TimedOut || result.status == DriverStatus::Signaled ||
            (result.status == DriverStatus::Exited && result.exit_code.value_or(1) != 0)) operational_error = true;
    }
    (void)listener->close(0, {});
    try {
        std::vector<requirements::Outcome> outcomes;
        for (const auto& row : catalog.requirements) outcomes.push_back({row.id, requirements::OutcomeState::NotRun});
        auto score = requirements::score_staged(catalog, outcomes);
        if (operational_error) score.verdict = requirements::RunVerdict::Error;
        store.finalize(id,score,outcomes);
    } catch (...) {
        try { store.finalize(id,{requirements::RunVerdict::Error,{0,0},{0,0},{0,0}},{}); } catch (...) {}
    }
}
}
