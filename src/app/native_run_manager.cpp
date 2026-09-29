#include "moq/interop/app/native_run_manager.h"

#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/draft18.h"
#include "moq/interop/scenarios/run_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace moq::interop::app {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kSubscribeScenario =
    "subscribe-to-publisher-track";

bool valid_fixture(const TrackFixture& fixture) {
    if (fixture.namespace_fields.size() > 32) return false;
    std::size_t total = fixture.track_name.size();
    if (total > 4096) return false;
    for (const auto& field : fixture.namespace_fields) {
        if (field.empty() || field.size() > 4096 - total) return false;
        total += field.size();
    }
    return true;
}

wire::draft18::TrackNamespace track_namespace(
    const TrackFixture& fixture) {
    wire::draft18::TrackNamespace result;
    for (const auto& field : fixture.namespace_fields) {
        std::vector<std::byte> bytes;
        bytes.reserve(field.size());
        for (const char byte : field) {
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
        }
        result.fields.push_back(std::move(bytes));
    }
    return result;
}

wire::draft18::TrackName track_name(const TrackFixture& fixture) {
    wire::draft18::TrackName result;
    result.bytes.reserve(fixture.track_name.size());
    for (const char byte : fixture.track_name) {
        result.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    }
    return result;
}

const char* evidence_name(session::EvidenceKind kind) {
    switch (kind) {
    case session::EvidenceKind::TransportEstablished:
        return "transport_established";
    case session::EvidenceKind::LocalSetupObserved:
        return "local_setup_observed";
    case session::EvidenceKind::PeerSetupReceived:
        return "peer_setup_received";
    case session::EvidenceKind::RequestObserved:
        return "request_observed";
    case session::EvidenceKind::InitialResponseObserved:
        return "initial_response_observed";
    case session::EvidenceKind::ResponseViolation:
        return "response_violation";
    case session::EvidenceKind::ProtocolViolation:
        return "protocol_violation";
    case session::EvidenceKind::ObjectObserved:
        return "object_observed";
    case session::EvidenceKind::PeerClose:
        return "peer_close";
    case session::EvidenceKind::LocalClose:
        return "local_close";
    case session::EvidenceKind::HarnessLimit:
        return "harness_limit";
    default:
        return "session_evidence";
    }
}

storage::EvidenceEvent stored_evidence(
    const session::EvidenceEvent& source,
    const std::string& scenario_id,
    scenarios::Clock::time_point started) {
    storage::EvidenceEvent result;
    result.monotonic_time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            scenarios::Clock::now() - started).count();
    result.wall_time_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    result.kind = evidence_name(source.kind);
    result.detail = "typed session evidence sequence " +
                    std::to_string(source.sequence) + " kind " +
                    std::to_string(static_cast<unsigned>(source.kind));
    result.scenario_id = scenario_id;
    if (const auto* request =
            std::get_if<session::RequestObservedEvidence>(&source.data)) {
        result.stream_id = std::to_string(request->stream_id);
        result.request_id = std::to_string(request->request_id);
    } else if (const auto* response =
                   std::get_if<session::InitialResponseEvidence>(&source.data)) {
        result.stream_id = std::to_string(response->stream_id);
        result.request_id = std::to_string(response->original_request_id);
    }
    return result;
}

bool terminal(scenarios::ScenarioStatus status) {
    return status == scenarios::ScenarioStatus::Passed ||
           status == scenarios::ScenarioStatus::Failed ||
           status == scenarios::ScenarioStatus::TimedOut ||
           status == scenarios::ScenarioStatus::Stopped;
}

}  // namespace

class NativeRunManager::Impl {
public:
    struct Worker {
        RunId id;
        transport::BoundEndpoint endpoint;
        std::atomic<bool> stop_requested{false};
        std::atomic<bool> finished{false};
        std::thread thread;
    };

    Impl(std::shared_ptr<const requirements::RequirementCatalog> supplied_catalog,
         std::shared_ptr<storage::RunStore> supplied_store,
         NativeRunManagerConfig supplied_config)
        : catalog(std::move(supplied_catalog)), store(std::move(supplied_store)),
          config(std::move(supplied_config)) {
        if (!catalog || !store || catalog->draft != 18 || !catalog->complete ||
            config.maximum_active_runs == 0 ||
            config.port_start > config.port_end ||
            (config.port_start == 0) != (config.port_end == 0)) {
            throw std::invalid_argument("invalid native run manager configuration");
        }
    }

    ~Impl() {
        std::vector<std::unique_ptr<Worker>> departing;
        {
            std::lock_guard lock(mutex);
            for (auto& worker : workers) worker->stop_requested = true;
            departing = std::move(workers);
        }
        for (auto& worker : departing) {
            if (worker->thread.joinable()) worker->thread.join();
        }
    }

    void reap_finished() {
        auto it = workers.begin();
        while (it != workers.end()) {
            if ((*it)->finished) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void run(Worker* worker,
             std::unique_ptr<transport::NativeQuicListener> listener,
             RunConfig run_config) {
        try {
            const auto started = scenarios::Clock::now();
            const auto deadline = started + run_config.timeout;
            const auto quiet = std::min(50ms, run_config.timeout / 4);
            scenarios::Draft18RunController controller(
                *listener,
                scenarios::subscribe_to_publisher_track(
                    track_namespace(*run_config.track_fixture),
                    track_name(*run_config.track_fixture), 1,
                    run_config.timeout - quiet, quiet));
            std::size_t recorded = 0;
            while (!worker->stop_requested) {
                const auto now = scenarios::Clock::now();
                const auto snapshot = controller.poll(now);
                const auto& evidence = controller.context().evidence;
                if (recorded < evidence.size()) {
                    std::vector<storage::EvidenceEvent> batch;
                    batch.reserve(evidence.size() - recorded);
                    for (; recorded < evidence.size(); ++recorded) {
                        batch.push_back(stored_evidence(
                            evidence[recorded],
                            controller.context().scenario_id, started));
                    }
                    store->append_events(worker->id, batch);
                }
                if (terminal(snapshot.status) || snapshot.harness_failed ||
                    now >= deadline) break;
                std::this_thread::sleep_for(1ms);
            }
            auto context = controller.context();
            if (worker->stop_requested) context.complete = false;
            const auto outcomes = requirements::evaluate_draft18(
                *catalog, std::span<const requirements::ScenarioContext>(
                              &context, 1));
            auto score = requirements::score(*catalog, outcomes);
            store->finalize(worker->id, score, outcomes);
        } catch (const std::exception& error) {
            std::cerr << "native run " << worker->id << " failed: "
                      << error.what() << '\n';
            try {
                const requirements::ScoreSummary failure{
                    requirements::RunVerdict::Error,
                    {0, 0}, {0, 0}, {0, 0}};
                store->finalize(worker->id, failure, {});
            } catch (const std::exception& finalization_error) {
                std::cerr << "native run " << worker->id
                          << " finalization failed: "
                          << finalization_error.what() << '\n';
            }
        }
        worker->finished = true;
    }

    std::shared_ptr<const requirements::RequirementCatalog> catalog;
    std::shared_ptr<storage::RunStore> store;
    NativeRunManagerConfig config;
    std::mutex mutex;
    std::vector<std::unique_ptr<Worker>> workers;
};

NativeRunManager::NativeRunManager(
    std::shared_ptr<const requirements::RequirementCatalog> catalog,
    std::shared_ptr<storage::RunStore> store,
    NativeRunManagerConfig config)
    : impl_(std::make_unique<Impl>(std::move(catalog), std::move(store),
                                    std::move(config))) {}

NativeRunManager::~NativeRunManager() = default;

RunStartResult NativeRunManager::start(const RunConfig& config) {
    if (config.draft != DraftVersion::Draft18 ||
        config.transport != TransportKind::NativeQuic ||
        config.mode != RunMode::Observed ||
        config.scenario_ids !=
            std::vector<std::string>{std::string(kSubscribeScenario)}) {
        return {RunStartStatus::Unsupported, {}, {}};
    }
    if (!config.track_fixture || !valid_fixture(*config.track_fixture) ||
        config.timeout < 2ms) {
        return {RunStartStatus::InvalidConfig, {}, {}};
    }
    std::lock_guard lock(impl_->mutex);
    impl_->reap_finished();
    if (impl_->workers.size() >= impl_->config.maximum_active_runs) {
        return {RunStartStatus::PortExhausted, {}, {}};
    }
    std::unique_ptr<transport::NativeQuicListener> listener;
    transport::BoundEndpoint endpoint;
    for (unsigned port = impl_->config.port_start;
         port <= impl_->config.port_end; ++port) {
        transport::NativeQuicListenerConfig listener_config;
        listener_config.bind_address = impl_->config.bind_address;
        listener_config.bind_port = static_cast<std::uint16_t>(port);
        listener_config.certificate_path = impl_->config.certificate_path;
        listener_config.private_key_path = impl_->config.private_key_path;
        listener_config.expected_alpn = {
            std::byte{'m'}, std::byte{'o'}, std::byte{'q'},
            std::byte{'t'}, std::byte{'-'}, std::byte{'1'}, std::byte{'8'}};
        auto created = transport::NativeQuicListener::create(
            std::move(listener_config));
        if (created.listener) {
            endpoint = created.listener->bound_endpoint();
            if (!impl_->config.advertised_address.empty()) {
                endpoint.address = impl_->config.advertised_address;
            }
            listener = std::move(created.listener);
            break;
        }
        if (created.error != transport::NativeQuicListenerError::BindFailed) {
            return {RunStartStatus::ListenerError, {}, {}};
        }
    }
    if (!listener) return {RunStartStatus::PortExhausted, {}, {}};

    const auto id = impl_->store->create_run(config);
    auto worker = std::make_unique<Impl::Worker>();
    worker->id = id;
    worker->endpoint = endpoint;
    auto* worker_ptr = worker.get();
    impl_->workers.push_back(std::move(worker));
    try {
        worker_ptr->thread = std::thread(
            [this, worker_ptr, listener = std::move(listener), config] () mutable {
                impl_->run(worker_ptr, std::move(listener), config);
            });
    } catch (...) {
        impl_->workers.pop_back();
        const requirements::ScoreSummary failure{
            requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
        impl_->store->finalize(id, failure, {});
        return {RunStartStatus::ListenerError, {}, {}};
    }
    return {RunStartStatus::Started, id, endpoint};
}

bool NativeRunManager::stop(const RunId& id) {
    std::unique_ptr<Impl::Worker> worker;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = std::find_if(
            impl_->workers.begin(), impl_->workers.end(),
            [&id](const auto& item) { return item->id == id; });
        if (found == impl_->workers.end()) return false;
        (*found)->stop_requested = true;
        worker = std::move(*found);
        impl_->workers.erase(found);
    }
    if (worker->thread.joinable()) worker->thread.join();
    return true;
}

}  // namespace moq::interop::app
