#include "moq/interop/app/native_run_manager.h"

#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/draft18.h"
#include "moq/interop/scenarios/draft21_announcement.h"
#include "moq/interop/scenarios/run_controller.h"
#include "moq/interop/transport/webtransport_listener.h"

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
constexpr std::string_view kDuplicateSubscribeScenario =
    "subscribe-again-to-established-publisher-track";
constexpr std::string_view kFetchScenario = "fetch-publisher-track-range";
constexpr std::string_view kSubscribeNamespaceScenario =
    "subscribe-namespace-at-publisher";
constexpr std::string_view kSubscribeTracksScenario =
    "subscribe-tracks-at-publisher";
constexpr std::string_view kDraft21AnnouncementScenario =
    "d21-publisher-request-stream-placement";
constexpr std::string_view kDraft21UnknownOptionScenario =
    "d21-setup-unknown-options";
constexpr std::string_view kDraft21DuplicateUnknownOptionScenario =
    "d21-setup-duplicate-unknown-options";
constexpr std::string_view kDraft21ServerAuthorityScenario =
    "d21-server-sends-authority";
constexpr std::string_view kDraft21ServerPathScenario =
    "d21-server-sends-path";

bool draft21_scenario_id(std::string_view scenario) {
    return scenario == kDraft21AnnouncementScenario ||
           scenario == kDraft21UnknownOptionScenario ||
           scenario == kDraft21DuplicateUnknownOptionScenario ||
           scenario == kDraft21ServerAuthorityScenario ||
           scenario == kDraft21ServerPathScenario;
}

scenarios::Draft21SetupProbe draft21_setup_probe(std::string_view scenario) {
    if (scenario == kDraft21UnknownOptionScenario) {
        return scenarios::Draft21SetupProbe::UnknownOption;
    }
    if (scenario == kDraft21DuplicateUnknownOptionScenario) {
        return scenarios::Draft21SetupProbe::DuplicateUnknownOption;
    }
    if (scenario == kDraft21ServerAuthorityScenario) {
        return scenarios::Draft21SetupProbe::ServerAuthority;
    }
    if (scenario == kDraft21ServerPathScenario) {
        return scenarios::Draft21SetupProbe::ServerPath;
    }
    return scenarios::Draft21SetupProbe::None;
}

std::vector<std::byte> bytes_of(const std::string& value) {
    std::vector<std::byte> result;
    result.reserve(value.size());
    for (const char byte : value) {
        result.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(byte)));
    }
    return result;
}

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

storage::EvidenceEvent stored_draft21_evidence(
    const scenarios::Draft21AnnouncementEvent& source,
    scenarios::Draft21Clock::time_point started,
    std::string_view scenario_id) {
    storage::EvidenceEvent result;
    result.monotonic_time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            scenarios::Draft21Clock::now() - started).count();
    result.wall_time_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    switch (source.kind) {
    case scenarios::Draft21AnnouncementEventKind::TransportEstablished:
        result.kind = "transport_established"; break;
    case scenarios::Draft21AnnouncementEventKind::LocalSetupSent:
        result.kind = "local_setup_sent"; break;
    case scenarios::Draft21AnnouncementEventKind::PeerSetupReceived:
        result.kind = "peer_setup_received"; break;
    case scenarios::Draft21AnnouncementEventKind::PublishObserved:
        result.kind = "publish_observed"; break;
    case scenarios::Draft21AnnouncementEventKind::ResponseDelivered:
        result.kind = "response_delivered"; break;
    case scenarios::Draft21AnnouncementEventKind::UnsupportedStream:
        result.kind = "unsupported_stream"; break;
    case scenarios::Draft21AnnouncementEventKind::InvalidRequestOpener:
        result.kind = "invalid_request_opener"; break;
    case scenarios::Draft21AnnouncementEventKind::ProtocolViolation:
        result.kind = "protocol_violation"; break;
    case scenarios::Draft21AnnouncementEventKind::PeerClosed:
        result.kind = "peer_closed"; break;
    case scenarios::Draft21AnnouncementEventKind::HarnessLimit:
        result.kind = "harness_limit"; break;
    }
    result.detail = source.application_close_code
        ? "draft-21 peer application close code " +
              std::to_string(*source.application_close_code)
        : "draft-21 announcement evidence";
    result.scenario_id = scenario_id;
    if (source.stream_id) {
        result.stream_id = std::to_string(*source.stream_id);
    }
    if (source.request_id) {
        result.request_id = std::to_string(*source.request_id);
    }
    return result;
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

    Impl(std::shared_ptr<const requirements::RequirementCatalog> supplied_draft18,
         std::shared_ptr<const requirements::RequirementCatalog> supplied_draft21,
         std::shared_ptr<storage::RunStore> supplied_store,
         NativeRunManagerConfig supplied_config)
        : draft18(std::move(supplied_draft18)),
          draft21(std::move(supplied_draft21)),
          store(std::move(supplied_store)),
          config(std::move(supplied_config)) {
        if (!draft18 || !store || draft18->draft != 18 || !draft18->complete ||
            (draft21 && (draft21->draft != 21 || !draft21->complete)) ||
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
             std::unique_ptr<transport::SessionTransport> listener,
             RunConfig run_config) {
        try {
            if (run_config.draft == DraftVersion::Draft21) {
                run_draft21(worker, *listener, run_config);
            } else {
                run_draft18(worker, *listener, run_config);
            }
        } catch (const std::exception& error) {
            std::cerr << "publisher run " << worker->id << " failed: "
                      << error.what() << '\n';
            try {
                const requirements::ScoreSummary failure{
                    requirements::RunVerdict::Error,
                    {0, 0}, {0, 0}, {0, 0}};
                store->finalize(worker->id, failure, {});
            } catch (const std::exception& finalization_error) {
                std::cerr << "publisher run " << worker->id
                          << " finalization failed: "
                          << finalization_error.what() << '\n';
            }
        }
        worker->finished = true;
    }

    void run_draft18(Worker* worker,
                     transport::SessionTransport& listener,
                     const RunConfig& run_config) {
            const auto started = scenarios::Clock::now();
            const auto deadline = started + run_config.timeout;
            const auto quiet = std::clamp(run_config.timeout / 4, 1ms, 50ms);
            const bool duplicate =
                run_config.scenario_ids.front() == kDuplicateSubscribeScenario;
            const bool fetch =
                run_config.scenario_ids.front() == kFetchScenario;
            const bool subscribe_namespace =
                run_config.scenario_ids.front() == kSubscribeNamespaceScenario;
            const bool subscribe_tracks =
                run_config.scenario_ids.front() == kSubscribeTracksScenario;
            scenarios::ScenarioDefinition definition;
            if (duplicate) {
                definition =
                    scenarios::subscribe_again_to_established_publisher_track(
                        track_namespace(*run_config.track_fixture),
                        track_name(*run_config.track_fixture), 1, 3,
                        (run_config.timeout - quiet) / 2, quiet);
            } else if (fetch) {
                definition = scenarios::fetch_publisher_track_range(
                    track_namespace(*run_config.track_fixture),
                    track_name(*run_config.track_fixture), 1,
                    {0, 0}, {0, 1}, run_config.timeout - quiet, quiet);
            } else if (subscribe_namespace) {
                definition = scenarios::subscribe_namespace_at_publisher(
                    track_namespace(*run_config.track_fixture), 1,
                    run_config.timeout - quiet, quiet);
            } else if (subscribe_tracks) {
                definition = scenarios::subscribe_tracks_at_publisher(
                    track_namespace(*run_config.track_fixture), 1,
                    run_config.timeout - quiet, quiet);
            } else {
                definition = scenarios::subscribe_to_publisher_track(
                    track_namespace(*run_config.track_fixture),
                    track_name(*run_config.track_fixture), 1,
                    run_config.timeout - quiet, quiet);
            }
            scenarios::Draft18RunController controller(
                listener, std::move(definition));
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
                *draft18, std::span<const requirements::ScenarioContext>(
                              &context, 1));
            auto score = requirements::score(*draft18, outcomes);
            store->finalize(worker->id, score, outcomes);
    }

    void run_draft21(Worker* worker,
                     transport::SessionTransport& listener,
                     const RunConfig& run_config) {
        const auto started = scenarios::Draft21Clock::now();
        const auto deadline = started + run_config.timeout;
        std::vector<std::vector<std::byte>> name_space;
        for (const auto& field : run_config.track_fixture->namespace_fields) {
            name_space.push_back(bytes_of(field));
        }
        scenarios::Draft21AnnouncementController controller(
            listener, std::move(name_space),
            bytes_of(run_config.track_fixture->track_name),
            run_config.timeout,
            draft21_setup_probe(run_config.scenario_ids.front()));
        std::size_t recorded = 0;
        while (!worker->stop_requested) {
            const auto now = scenarios::Draft21Clock::now();
            const auto snapshot = controller.poll(now);
            const auto& evidence = controller.context().evidence;
            if (recorded < evidence.size()) {
                std::vector<storage::EvidenceEvent> batch;
                batch.reserve(evidence.size() - recorded);
                for (; recorded < evidence.size(); ++recorded) {
                    batch.push_back(stored_draft21_evidence(
                        evidence[recorded], started,
                        run_config.scenario_ids.front()));
                }
                store->append_events(worker->id, batch);
            }
            if (snapshot.status != scenarios::Draft21AnnouncementStatus::Running ||
                snapshot.harness_failed || now >= deadline) break;
            std::this_thread::sleep_for(1ms);
        }
        auto context = controller.context();
        if (worker->stop_requested) context.complete = false;
        const auto outcomes = requirements::evaluate_draft21_announcement(
            *draft21, context);
        const auto summary = requirements::score(*draft21, outcomes);
        store->finalize(worker->id, summary, outcomes);
    }

    std::shared_ptr<const requirements::RequirementCatalog> draft18;
    std::shared_ptr<const requirements::RequirementCatalog> draft21;
    std::shared_ptr<storage::RunStore> store;
    NativeRunManagerConfig config;
    std::mutex mutex;
    std::vector<std::unique_ptr<Worker>> workers;
};

NativeRunManager::NativeRunManager(
    std::shared_ptr<const requirements::RequirementCatalog> catalog,
    std::shared_ptr<storage::RunStore> store,
    NativeRunManagerConfig config)
    : NativeRunManager(std::move(catalog), nullptr, std::move(store),
                       std::move(config)) {}

NativeRunManager::NativeRunManager(
    std::shared_ptr<const requirements::RequirementCatalog> draft18,
    std::shared_ptr<const requirements::RequirementCatalog> draft21,
    std::shared_ptr<storage::RunStore> store,
    NativeRunManagerConfig config)
    : impl_(std::make_unique<Impl>(std::move(draft18), std::move(draft21),
                                    std::move(store), std::move(config))) {}

NativeRunManager::~NativeRunManager() = default;

RunStartResult NativeRunManager::start(const RunConfig& config) {
    const bool draft18_scenario =
        config.draft == DraftVersion::Draft18 &&
        config.scenario_ids.size() == 1 &&
        (config.scenario_ids.front() == kSubscribeScenario ||
         config.scenario_ids.front() == kDuplicateSubscribeScenario ||
         config.scenario_ids.front() == kFetchScenario ||
         config.scenario_ids.front() == kSubscribeNamespaceScenario ||
         config.scenario_ids.front() == kSubscribeTracksScenario);
    const bool draft21_scenario =
        config.draft == DraftVersion::Draft21 && impl_->draft21 &&
        config.scenario_ids.size() == 1 &&
        draft21_scenario_id(config.scenario_ids.front());
    if ((!draft18_scenario && !draft21_scenario) ||
        config.mode != RunMode::Observed ||
        !supports(config.draft)) {
        return {RunStartStatus::Unsupported, {}, {}};
    }
    if (!config.track_fixture || !valid_fixture(*config.track_fixture) ||
        config.timeout < 2ms ||
        (config.draft == DraftVersion::Draft18 &&
         config.scenario_ids.size() == 1 &&
         config.scenario_ids.front() == kDuplicateSubscribeScenario &&
         config.timeout < 3ms)) {
        return {RunStartStatus::InvalidConfig, {}, {}};
    }
    std::lock_guard lock(impl_->mutex);
    impl_->reap_finished();
    if (impl_->workers.size() >= impl_->config.maximum_active_runs) {
        return {RunStartStatus::PortExhausted, {}, {}};
    }
    std::unique_ptr<transport::SessionTransport> listener;
    transport::BoundEndpoint endpoint;
    std::string url;
    std::string path;
    std::string protocol;
    for (unsigned port = impl_->config.port_start;
         port <= impl_->config.port_end; ++port) {
        if (config.transport == TransportKind::WebTransport) {
            transport::WebTransportListenerConfig listener_config;
            listener_config.quic.bind_address = impl_->config.bind_address;
            listener_config.quic.bind_port = static_cast<std::uint16_t>(port);
            listener_config.quic.certificate_path = impl_->config.certificate_path;
            listener_config.quic.private_key_path = impl_->config.private_key_path;
            listener_config.advertised_host = impl_->config.advertised_address;
            listener_config.allowed_origins = impl_->config.webtransport_allowed_origins;
            listener_config.require_origin = impl_->config.webtransport_require_origin;
            listener_config.application_protocol = draft21_scenario ? "moqt-21" : "moqt-18";
            auto created = transport::WebTransportListener::create(
                std::move(listener_config));
            if (created.listener) {
                endpoint = created.listener->bound_endpoint();
                if (!impl_->config.advertised_address.empty())
                    endpoint.address = impl_->config.advertised_address;
                path = "/moq";
                protocol = draft21_scenario ? "moqt-21" : "moqt-18";
                const auto host = endpoint.address.find(':') != std::string::npos
                    ? "[" + endpoint.address + "]" : endpoint.address;
                url = "https://" + host + ":" + std::to_string(endpoint.port) + path;
                listener = std::move(created.listener);
                break;
            }
            if (created.error != transport::NativeQuicListenerError::BindFailed)
                return {RunStartStatus::ListenerError, {}, {}};
            continue;
        }
        transport::NativeQuicListenerConfig listener_config;
        listener_config.bind_address = impl_->config.bind_address;
        listener_config.bind_port = static_cast<std::uint16_t>(port);
        listener_config.certificate_path = impl_->config.certificate_path;
        listener_config.private_key_path = impl_->config.private_key_path;
        listener_config.expected_alpn =
            draft21_scenario
                ? std::vector<std::byte>{
                      std::byte{'m'}, std::byte{'o'}, std::byte{'q'},
                      std::byte{'t'}, std::byte{'-'}, std::byte{'2'},
                      std::byte{'1'}}
                : std::vector<std::byte>{
                      std::byte{'m'}, std::byte{'o'}, std::byte{'q'},
                      std::byte{'t'}, std::byte{'-'}, std::byte{'1'},
                      std::byte{'8'}};
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
    return {RunStartStatus::Started, id, endpoint, url, path, protocol};
}

bool NativeRunManager::supports(DraftVersion draft) const noexcept {
    return draft == DraftVersion::Draft18 ||
           (draft == DraftVersion::Draft21 && impl_->draft21 != nullptr);
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
