#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/publisher_driver.h"
#include "moq/interop/app/scenario_registry.h"

#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/draft18.h"
#include "moq/interop/scenarios/draft18_close.h"
#include "moq/interop/scenarios/draft18_gap_a.h"
#include "moq/interop/scenarios/draft18_peer_close.h"
#include "moq/interop/scenarios/draft18_request.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/scenarios/fetch_probe.h"
#include "moq/interop/scenarios/fetch_response.h"
#include "moq/interop/scenarios/request_response.h"
#include "moq/interop/scenarios/range_filter.h"
#include "moq/interop/scenarios/subscription_cancel.h"
#include "moq/interop/scenarios/discovery_overlap.h"
#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/fetch_group_order.h"
#include "moq/interop/scenarios/draft21_gap_a.h"
#include "moq/interop/scenarios/draft21_gap_a_token.h"
#include "moq/interop/scenarios/immutable_repeat.h"
#include "moq/interop/scenarios/object_repeat.h"
#include "moq/interop/scenarios/draft18_contribution.h"
#include "moq/interop/scenarios/request_goaway.h"
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/draft21_response.h"
#include "moq/interop/scenarios/draft21_announcement.h"
#include "moq/interop/scenarios/draft21_contribution.h"
#include "moq/interop/scenarios/run_controller.h"
#include "moq/interop/transport/webtransport_listener.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace moq::interop::app {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kDuplicateSubscribeScenario =
    "subscribe-again-to-established-publisher-track";
constexpr std::string_view kFetchScenario = "fetch-publisher-track-range";
constexpr std::string_view kSubscribeNamespaceScenario =
    "subscribe-namespace-at-publisher";
constexpr std::string_view kSubscribeTracksScenario =
    "subscribe-tracks-at-publisher";
constexpr std::string_view kDraft21UnknownOptionScenario =
    "d21-setup-unknown-options";
constexpr std::string_view kDraft21DuplicateUnknownOptionScenario =
    "d21-setup-duplicate-unknown-options";
constexpr std::string_view kDraft21ServerAuthorityScenario =
    "d21-server-sends-authority";
constexpr std::string_view kDraft21ServerPathScenario =
    "d21-server-sends-path";

scenarios::Draft21SetupProbe draft21_setup_probe(std::string_view scenario) {
    if (scenario == kDraft21UnknownOptionScenario) {
        return scenarios::Draft21SetupProbe::UnknownOption;
    }
    if (scenario == kDraft21DuplicateUnknownOptionScenario) {
        return scenarios::Draft21SetupProbe::DuplicateUnknownOption;
    }
    if (scenario == kDraft21ServerAuthorityScenario ||
        scenario == "d21-webtransport-server-sends-authority") {
        return scenarios::Draft21SetupProbe::ServerAuthority;
    }
    if (scenario == kDraft21ServerPathScenario ||
        scenario == "d21-webtransport-server-sends-path") {
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

std::string hex_bytes(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
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
    case session::EvidenceKind::SetupOptionDuplicate:
        return "setup_option_duplicate";
    case session::EvidenceKind::PeerStreamClassified:
        return "peer_stream_classified";
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
    if (const auto* setup =
            std::get_if<session::SetupEvidence>(&source.data)) {
        result.detail = "draft-18 peer SETUP option types: ";
        if (setup->setup.options.empty()) {
            result.detail += "none";
        } else {
            for (std::size_t index = 0; index < setup->setup.options.size();
                 ++index) {
                if (index != 0) result.detail += ",";
                result.detail += std::to_string(setup->setup.options[index].type);
            }
        }
        result.stream_id = std::to_string(setup->stream_id);
    } else if (const auto* duplicate =
                   std::get_if<session::SetupOptionDuplicateEvidence>(
                       &source.data)) {
        result.detail = "draft-18 duplicate SETUP option type " +
                        std::to_string(duplicate->option_type);
        result.stream_id = std::to_string(duplicate->stream_id);
    } else if (const auto* stream =
                   std::get_if<session::StreamEvidence>(&source.data)) {
        result.stream_id = std::to_string(stream->stream_id);
    } else if (const auto* violation =
                   std::get_if<session::ProtocolViolationEvidence>(
                       &source.data)) {
        if (violation->stream_id) {
            result.stream_id = std::to_string(*violation->stream_id);
        }
        if (violation->opener_message_type) {
            result.detail = "draft-18 protocol close with peer request opener type " +
                            std::to_string(*violation->opener_message_type);
        }
    }
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
    case scenarios::Draft21AnnouncementEventKind::NamespaceObserved:
        result.kind = "namespace_observed"; break;
    case scenarios::Draft21AnnouncementEventKind::NamespaceResponseDelivered:
        result.kind = "namespace_response_delivered"; break;
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
    case scenarios::Draft21AnnouncementEventKind::MalformedPublisherMessage:
        result.kind = "protocol_violation"; break;
    }
    result.detail = source.application_close_code
        ? "draft-21 peer application close code " +
              std::to_string(*source.application_close_code)
        : "draft-21 announcement evidence";
    if (source.kind == scenarios::Draft21AnnouncementEventKind::PeerSetupReceived) {
        result.detail = "draft-21 peer SETUP option types: ";
        if (source.setup_option_types.empty()) {
            result.detail += "none";
        } else {
            for (std::size_t index = 0; index < source.setup_option_types.size();
                 ++index) {
                if (index != 0) result.detail += ",";
                result.detail += std::to_string(source.setup_option_types[index]);
            }
        }
    }
    // Slice A: keep the decoded values the evaluators relied on.
    if (source.kind == scenarios::Draft21AnnouncementEventKind::PeerSetupReceived &&
        !source.detail.empty()) {
        result.detail += "; option values (type=hex or integer): " + source.detail;
    }
    if (source.kind == scenarios::Draft21AnnouncementEventKind::MalformedPublisherMessage) {
        result.detail = "draft-21 malformed publisher message: " + source.detail;
    }
    if ((source.kind == scenarios::Draft21AnnouncementEventKind::PublishObserved ||
         source.kind == scenarios::Draft21AnnouncementEventKind::NamespaceObserved) &&
        !source.track_namespace.empty()) {
        result.detail = "draft-21 publisher request namespace fields (hex): ";
        for (std::size_t index = 0; index < source.track_namespace.size(); ++index) {
            if (index != 0) result.detail += "/";
            result.detail += hex_bytes(source.track_namespace[index]);
        }
    }
    result.scenario_id = scenario_id;
    if (source.stream_id) {
        result.stream_id = std::to_string(*source.stream_id);
    }
    if (source.request_id) {
        result.request_id = std::to_string(*source.request_id);
    }
    return result;
}

storage::EvidenceEvent driver_evidence(const DriverResult& result,
                                       std::string_view scenario_id) {
    storage::EvidenceEvent event;
    event.wall_time_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    event.kind = "publisher_process";
    event.detail = serialize_driver_result(result);
    event.scenario_id = std::string(scenario_id);
    return event;
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
        scenarios::RawProbeClock::time_point started{scenarios::RawProbeClock::now()};
        std::size_t context_ordinal{0};
        std::string connection_id;
        std::vector<scenarios::RawProbeTranscript> transcripts;
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
            (config.port_start == 0) != (config.port_end == 0) ||
            (config.driver_executable.empty() != config.driver_log_root.empty()) ||
            (!config.driver_executable.empty() &&
             !config.driver_executable.is_absolute())) {
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

    struct ListenerResult {
        std::unique_ptr<transport::SessionTransport> listener;
        transport::BoundEndpoint endpoint;
        std::optional<transport::NativeQuicListenerError> error;
    };

    // Path of the replacement session's URI, distinct from the primary "/moq".
    static constexpr std::string_view kReplacementPath = "/moq-next";

    ListenerResult create_listener(const RunConfig& run_config, std::uint16_t port,
                                   std::optional<std::uint64_t> peer_bidi_streams = std::nullopt,
                                   std::string_view path = {}) const {
        transport::NativeQuicListenerConfig quic;
        quic.bind_address = config.bind_address;
        quic.bind_port = port;
        // A stream-credit scenario starts the peer with only this many
        // bidirectional streams; WebTransport spends one on its CONNECT.
        if (peer_bidi_streams)
            quic.initial_max_streams_bidi = *peer_bidi_streams +
                (run_config.transport == TransportKind::WebTransport ? 1u : 0u);
        quic.certificate_path = config.certificate_path;
        quic.private_key_path = config.private_key_path;
        const std::string protocol = run_config.draft == DraftVersion::Draft21 ? "moqt-21" : "moqt-18";
        if (run_config.transport == TransportKind::WebTransport) {
            transport::WebTransportListenerConfig settings;
            settings.quic = std::move(quic);
            settings.advertised_host = config.advertised_address;
            settings.allowed_origins = config.webtransport_allowed_origins;
            settings.require_origin = config.webtransport_require_origin;
            settings.application_protocol = protocol;
            if (!path.empty()) settings.path = std::string(path);
            auto created = transport::WebTransportListener::create(std::move(settings));
            const auto endpoint = created.listener ? created.listener->bound_endpoint() : transport::BoundEndpoint{};
            return {std::move(created.listener), endpoint, created.error};
        }
        quic.expected_alpn = bytes_of(protocol);
        auto created = transport::NativeQuicListener::create(std::move(quic));
        const auto endpoint = created.listener ? created.listener->bound_endpoint() : transport::BoundEndpoint{};
        return {std::move(created.listener), endpoint, created.error};
    }

    std::optional<scenarios::RawProbeDefinition> resolve_raw_probe(
        const RunConfig& run_config, std::string_view id) const {
        if (!raw_probe_scenario(static_cast<unsigned>(run_config.draft), id)) return std::nullopt;
        if (run_config.draft == DraftVersion::Draft18 && scenarios::draft18_gap_a_scenario(id)) {
            std::vector<std::vector<std::byte>> name_space;
            std::vector<std::byte> name{std::byte{'x'}};
            if (run_config.track_fixture) {
                for (const auto& field : run_config.track_fixture->namespace_fields)
                    name_space.push_back(bytes_of(field));
                name = bytes_of(run_config.track_fixture->track_name);
            } else if (scenarios::draft18_gap_a_requires_track(id)) {
                throw std::invalid_argument("gap-A probe requires a track fixture");
            }
            auto profiles = scenarios::draft18_gap_a_probes(run_config.timeout, name_space, name);
            const auto found = std::find_if(profiles.begin(), profiles.end(),
                [id](const auto& profile) { return profile.definition.id == id; });
            if (found == profiles.end()) return std::nullopt;
            return std::move(found->definition);
        }
        if (auto definition = resolve_track_probe(run_config, id)) return definition;
        const auto find = [id](auto profiles) -> std::optional<scenarios::RawProbeDefinition> {
            const auto found = std::find_if(profiles.begin(), profiles.end(),
                [id](const auto& profile) { return profile.definition.id == id; });
            if (found == profiles.end()) return std::nullopt;
            return std::move(found->definition);
        };
        if (run_config.draft == DraftVersion::Draft18) {
            if (auto value = find(scenarios::draft18_response_probes(run_config.timeout))) return value;
            if (auto value = find(scenarios::draft18_peer_close_probes(run_config.timeout))) return value;
            if (auto value = find(scenarios::draft18_request_profiles(run_config.timeout))) return value;
            const auto profiles = scenarios::draft18_close_profiles();
            if (std::none_of(profiles.begin(), profiles.end(), [id](const auto& profile) {
                    return profile.scenario_id == id;
                })) return std::nullopt;
            return scenarios::draft18_close_probe(id, run_config.timeout);
        }
        if (auto value = find(scenarios::draft21_response_probes(run_config.timeout))) return value;
        if (auto value = find(scenarios::draft21_peer_close_probes(run_config.timeout))) return value;
        if (auto value = find(scenarios::draft21_request_profiles(run_config.timeout))) return value;
        return find(scenarios::draft21_close_probes(run_config.timeout));
    }

    static bool driver_failed(const DriverResult& result) {
        return result.status == DriverStatus::Error || result.status == DriverStatus::TimedOut ||
               result.status == DriverStatus::Signaled ||
               (result.status == DriverStatus::Stopped && result.term_signal == SIGKILL) ||
               (result.status == DriverStatus::Exited && result.exit_code.value_or(1) != 0);
    }

    static std::string authority_of(const Worker* worker) {
        const auto host = worker->endpoint.address.find(':') != std::string::npos
            ? "[" + worker->endpoint.address + "]" : worker->endpoint.address;
        return host + ":" + std::to_string(worker->endpoint.port);
    }

    std::string endpoint_uri(const Worker* worker, const RunConfig& run_config,
                             std::string_view scenario = {}) const {
        const auto tail = run_config.transport == TransportKind::NativeQuic &&
                run_config.draft == DraftVersion::Draft21 &&
                announcement_gap_scenario(21, scenario)
            ? std::string(gap_native_uri_path_and_query(scenario)) : std::string("/moq");
        auto uri = (run_config.transport == TransportKind::WebTransport ? "https://" : "moqt://") +
                   authority_of(worker) + tail;
        // Some contribution scenarios check how the publisher reports a URI query.
        if (run_config.draft == DraftVersion::Draft18 && run_config.transport == TransportKind::NativeQuic) {
            const auto query = scenarios::draft18_contribution_connection_query(scenario);
            if (!query.empty()) uri += "?" + std::string(query);
        }
        return uri;
    }

    void stamp_context_event(Worker* worker, storage::EvidenceEvent& event) const {
        event.monotonic_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            scenarios::RawProbeClock::now() - worker->started).count();
        event.wall_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        event.connection_id = worker->connection_id;
        if (event.kind != "publisher_process")
            event.detail += " ordinal=" + std::to_string(worker->context_ordinal);
    }

    void append_context_event(Worker* worker, std::string_view id,
                              std::string_view kind, std::string detail) {
        storage::EvidenceEvent event;
        event.scenario_id = id;
        event.kind = kind;
        event.detail = std::move(detail);
        stamp_context_event(worker, event);
        store->append_events(worker->id, std::span(&event, 1));
    }

    DriverHandle start_context_driver(Worker* worker, const RunConfig& run_config,
                                     std::string_view id, PublisherDriver& driver) {
        DriverRequest request;
        request.executable = config.driver_executable;
        request.arguments = config.driver_arguments;
        request.run_id = worker->id;
        request.scenario_id = id;
        request.endpoint = endpoint_uri(worker, run_config, id);
        request.draft = run_config.draft;
        request.transport = run_config.transport;
        request.track = *run_config.track_fixture;
        request.fixture = config.driver_fixture;
        request.tls_ca = config.driver_tls_ca.empty() ? config.certificate_path : config.driver_tls_ca;
        request.log_dir = config.driver_log_root / worker->id /
                          (std::to_string(worker->context_ordinal) + "-" + std::string(id));
        request.scenario_timeout = run_config.timeout;
        request.process_timeout = run_config.timeout + 1000ms;
        const auto started = driver.start(request);
        if (started.status == DriverStartStatus::Started) return started.handle;
        DriverResult failure;
        failure.error = started.error;
        auto event = driver_evidence(failure, id);
        stamp_context_event(worker, event);
        store->append_events(worker->id, std::span(&event, 1));
        throw std::runtime_error("publisher driver start failed: " + started.error);
    }

    void finalize_raw_family(Worker* worker, const RunConfig& run_config, bool operational_error) {
        std::vector<requirements::Outcome> outcomes;
        if (run_config.draft == DraftVersion::Draft21) {
            outcomes = requirements::evaluate_draft21_raw_probes(*draft21, worker->transcripts);
        } else {
            std::vector<requirements::ScenarioContext> contexts;
            contexts.reserve(worker->transcripts.size());
            for (const auto& transcript : worker->transcripts) {
                requirements::ScenarioContext context;
                context.scenario_id = transcript.scenario_id;
                context.complete = transcript.complete;
                context.stimulus_delivered = transcript.stimulus_delivered;
                context.webtransport = run_config.transport == TransportKind::WebTransport;
                context.raw_probe = transcript;
                contexts.push_back(std::move(context));
            }
            outcomes = requirements::evaluate_draft18(*draft18, contexts);
        }
        const auto& catalog = run_config.draft == DraftVersion::Draft21 ? *draft21 : *draft18;
        auto summary = requirements::score(catalog, outcomes);
        if (operational_error || worker->stop_requested) summary.verdict = requirements::RunVerdict::Error;
        store->finalize(worker->id, summary, outcomes);
    }

    void run_raw_family(Worker* worker, std::unique_ptr<transport::SessionTransport>& listener,
                        const RunConfig& run_config,
                        std::vector<scenarios::RawProbeDefinition> definitions) {
        PublisherDriver driver;
        DriverHandle handle;
        bool operational_error = false;
        std::string current_id;
        const auto retire_driver = [&] {
            if (!handle.valid()) return false;
            const auto result = driver.stop(handle);
            handle = {};
            auto event = driver_evidence(result, current_id);
            stamp_context_event(worker, event);
            store->append_events(worker->id, std::span(&event, 1));
            return driver_failed(result);
        };
        try {
            worker->transcripts.reserve(definitions.size());
            for (std::size_t index = 0; index < definitions.size(); ++index) {
                if (worker->stop_requested) break;
                current_id = definitions[index].id;
                worker->context_ordinal = index + 1;
                worker->connection_id.clear();
                const auto tuned = definitions[index].initial_peer_bidi_streams;
                // The first context's listener already carries its tuning (start()).
                if (index != 0) {
                    // Cleanup events belong outside the frozen proof of the preceding context.
                    listener->close(0, {});
                    const auto cleanup_deadline = scenarios::RawProbeClock::now() + 20ms;
                    do {
                        listener->poll(256);
                        if (worker->stop_requested) break;
                        std::this_thread::sleep_for(1ms);
                    } while (scenarios::RawProbeClock::now() < cleanup_deadline);
                    listener.reset();
                    if (worker->stop_requested) break;
                    auto replacement = create_listener(run_config, worker->endpoint.port, tuned);
                    if (!replacement.listener || replacement.endpoint.port != worker->endpoint.port)
                        throw std::runtime_error("raw context listener could not rebind reserved run port");
                    listener = std::move(replacement.listener);
                }
                if (worker->stop_requested) break;
                append_context_event(worker, current_id, "context_ready",
                    "endpoint=" + endpoint_uri(worker, run_config, current_id) +
                    " reconnect=fresh-session publisher_identity=unverified");
                if (worker->stop_requested) break;
                if (run_config.mode == RunMode::Driven)
                    handle = start_context_driver(worker, run_config, current_id, driver);
                auto transcript = collect_raw_probe(worker, *listener, run_config,
                    std::move(definitions[index]), &driver, handle);
                const bool process_error = retire_driver();
                if (process_error) {
                    transcript.complete = false;
                    transcript.harness_failed = true;
                    append_context_event(worker, current_id, "harness_error", "publisher process failed");
                }
                if (worker->stop_requested) transcript.complete = false;
                operational_error = operational_error || transcript.harness_failed;
                worker->transcripts.push_back(std::move(transcript));
                const auto& completed = worker->transcripts.back();
                append_context_event(worker, current_id,
                    completed.complete && !completed.harness_failed ? "context_complete" : "context_end",
                    "complete=" + std::string(completed.complete ? "true" : "false") +
                    " timed_out=" + (completed.timed_out ? "true" : "false") +
                    " cancelled=" + (worker->stop_requested ? "true" : "false"));
                if (operational_error || worker->stop_requested) break;
            }
        } catch (const std::exception& error) {
            operational_error = true;
            try {
                retire_driver();
                append_context_event(worker, current_id, "harness_error", error.what());
            } catch (const std::exception& cleanup_error) {
                std::cerr << "raw context cleanup failed: " << cleanup_error.what() << '\n';
            }
        }
        listener.reset();
        finalize_raw_family(worker, run_config, operational_error);
    }

    void run(Worker* worker,
             std::unique_ptr<transport::SessionTransport> listener,
             RunConfig run_config,
             std::vector<scenarios::RawProbeDefinition> definitions) {
        PublisherDriver driver;
        DriverHandle handle;
        auto record_driver = [&] {
            if (!handle.valid()) return;
            const auto result = driver.stop(handle);
            handle = {};
            const auto event = driver_evidence(result, run_config.scenario_ids.front());
            store->append_events(worker->id, std::span(&event, 1));
        };
        try {
            if (!definitions.empty()) {
                run_raw_family(worker, listener, run_config, std::move(definitions));
            } else {
                if (run_config.mode == RunMode::Driven) {
                    const std::string endpoint = endpoint_uri(
                        worker, run_config, run_config.scenario_ids.front());
                    DriverRequest request;
                    request.executable = config.driver_executable;
                    request.arguments = config.driver_arguments;
                    request.run_id = worker->id;
                    request.scenario_id = run_config.scenario_ids.front();
                    request.endpoint = endpoint;
                    request.draft = run_config.draft;
                    request.transport = run_config.transport;
                    request.track = *run_config.track_fixture;
                    request.fixture = config.driver_fixture;
                    request.tls_ca = config.driver_tls_ca.empty()
                        ? config.certificate_path : config.driver_tls_ca;
                    request.log_dir = config.driver_log_root / worker->id;
                    request.scenario_timeout = run_config.timeout;
                    request.process_timeout = run_config.timeout + 1000ms;
                    const auto started = driver.start(request);
                    if (started.status != DriverStartStatus::Started) {
                        DriverResult failure;
                        failure.error = started.error;
                        const auto event = driver_evidence(
                            failure, run_config.scenario_ids.front());
                        store->append_events(worker->id, std::span(&event, 1));
                        throw std::runtime_error("publisher driver start failed: " + started.error);
                    }
                    handle = started.handle;
                }
                if (run_config.draft == DraftVersion::Draft21) {
                    run_draft21(worker, *listener, run_config, &driver, handle, record_driver);
                } else {
                    run_draft18(worker, *listener, run_config, &driver, handle, record_driver);
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "publisher run " << worker->id << " failed: "
                      << error.what() << '\n';
            try {
                record_driver();
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
        listener.reset();
        {
            std::lock_guard lock(mutex);
            reserved_ports.erase(worker->endpoint.port);
        }
        worker->finished = true;
    }

    scenarios::RawProbeTranscript collect_raw_probe(Worker* worker, transport::SessionTransport& listener,
                       const RunConfig& run_config, scenarios::RawProbeDefinition definition,
                       PublisherDriver* driver, DriverHandle handle) {
        const auto started = scenarios::RawProbeClock::now();
        const auto deadline = started + run_config.timeout;
        // A replacement-session definition gets a second listener at its own
        // port and path, observed alongside the first session.
        std::unique_ptr<transport::SessionTransport> replacement;
        std::string replacement_uri;
        std::optional<std::uint16_t> replacement_port;
        if (definition.offer_replacement_session) {
            std::uint16_t port = 0;
            if (config.port_start != 0) {
                std::lock_guard lock(mutex);
                for (auto candidate = config.port_start; candidate <= config.port_end; ++candidate) {
                    if (candidate == worker->endpoint.port || reserved_ports.contains(candidate)) continue;
                    port = candidate;
                    break;
                }
                if (port == 0) throw std::runtime_error("no free port for the replacement session listener");
                reserved_ports.insert(port);
                replacement_port = port;
            }
            auto created = create_listener(run_config, port, std::nullopt, kReplacementPath);
            if (!created.listener) {
                if (replacement_port) { std::lock_guard lock(mutex); reserved_ports.erase(*replacement_port); }
                throw std::runtime_error("replacement session listener could not be created");
            }
            replacement = std::move(created.listener);
            const auto host = worker->endpoint.address.find(':') != std::string::npos
                ? "[" + worker->endpoint.address + "]" : worker->endpoint.address;
            replacement_uri = (run_config.transport == TransportKind::WebTransport ? "https://" : "moqt://") +
                host + ":" + std::to_string(created.endpoint.port) + std::string(kReplacementPath);
        }
        struct ReplacementGuard {
            std::function<void()> release;
            ~ReplacementGuard() { release(); }
        } guard{[&] {
            // Runs after the controller (declared next) is gone.
            replacement.reset();
            if (replacement_port) {
                std::lock_guard lock(mutex);
                reserved_ports.erase(*replacement_port);
            }
        }};
        scenarios::RawProbeController controller(listener, std::move(definition), replacement.get(),
                                                 std::move(replacement_uri));
        std::size_t recorded = 0;
        std::size_t replacement_recorded = 0;
        std::string operational_error;
        try {
            while (!worker->stop_requested) {
                const auto now = scenarios::RawProbeClock::now();
                const auto& transcript = controller.poll(now);
                std::vector<storage::EvidenceEvent> batch;
                for (; recorded < transcript.events.size(); ++recorded) {
                    storage::EvidenceEvent event;
                    event.scenario_id = transcript.scenario_id;
                    event.monotonic_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now-worker->started).count();
                    event.wall_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto& source = transcript.events[recorded];
                    if (const auto* close = std::get_if<transport::PeerCloseEvent>(&source)) {
                        event.kind = "peer_close";
                        event.detail = std::string(close->error_space == transport::CloseErrorSpace::Application ? "application" : "transport") +
                            " close code=" + std::to_string(close->error_code) +
                            " transport_event_index=" + std::to_string(recorded);
                    } else if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&source)) {
                        worker->connection_id = hex_bytes(established->local_connection_id);
                        event.kind = "transport_established";
                        event.detail = "local_connection_id=" + worker->connection_id +
                            " peer_connection_id=" + hex_bytes(established->peer_connection_id) +
                            " alpn=" + hex_bytes(established->alpn) +
                            " max_datagram_payload=" + std::to_string(established->max_datagram_payload) +
                            " transport_event_index=" + std::to_string(recorded);
                    } else if (std::holds_alternative<transport::EventQueueOverflowEvent>(source) ||
                               std::holds_alternative<transport::TransportErrorEvent>(source)) {
                        event.kind = "harness_limit";
                        event.detail = "raw probe transport evidence unavailable";
                    } else {
                        event.kind = "raw_probe_transport_event";
                        event.detail = "transport event variant=" + std::to_string(source.index());
                        if (const auto* data = std::get_if<transport::StreamDataEvent>(&source)) {
                            event.stream_id = std::to_string(data->stream_id);
                            event.detail += " fin=" + std::string(data->fin ? "true" : "false") +
                                " bytes=" + hex_bytes(data->data);
                        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&source)) {
                            event.stream_id = std::to_string(reset->stream_id);
                            event.detail += " operation=peer-reset application_error=" + (reset->application_error ? std::to_string(*reset->application_error) : "unavailable");
                        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&source)) {
                            event.stream_id = std::to_string(stop->stream_id);
                            event.detail += " operation=peer-stop-sending application_error=" + (stop->application_error ? std::to_string(*stop->application_error) : "unavailable");
                        }
                        event.detail += " transport_event_index=" + std::to_string(recorded);
                    }
                    event.connection_id = worker->connection_id;
                    event.detail += " ordinal=" + std::to_string(worker->context_ordinal);
                    batch.push_back(std::move(event));
                }
                for (; replacement_recorded < transcript.replacement_events.size(); ++replacement_recorded) {
                    storage::EvidenceEvent event;
                    event.scenario_id = transcript.scenario_id;
                    event.monotonic_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now-worker->started).count();
                    event.wall_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto& source = transcript.replacement_events[replacement_recorded];
                    event.kind = "raw_probe_replacement_event";
                    event.detail = "replacement session event variant=" + std::to_string(source.index());
                    if (const auto* data = std::get_if<transport::StreamDataEvent>(&source)) {
                        event.stream_id = std::to_string(data->stream_id);
                        event.detail += " fin=" + std::string(data->fin ? "true" : "false") + " bytes=" + hex_bytes(data->data);
                    }
                    event.detail += " replacement_event_index=" + std::to_string(replacement_recorded) +
                        " ordinal=" + std::to_string(worker->context_ordinal);
                    batch.push_back(std::move(event));
                }
                if (!batch.empty()) store->append_events(worker->id,batch);
                if (transcript.complete || transcript.harness_failed || transcript.timed_out || now >= deadline) break;
                if (handle.valid()) {
                    const auto process = driver->poll(handle);
                    if (driver_failed(process) ||
                        (process.status != DriverStatus::Running && !transcript.transport_established))
                        throw std::runtime_error("publisher process failed during raw context");
                }
                std::this_thread::sleep_for(1ms);
            }
        } catch (const std::exception& error) {
            operational_error = error.what();
        }
        auto transcript = controller.transcript();
        if (!operational_error.empty()) {
            transcript.complete = false;
            transcript.harness_failed = true;
            append_context_event(worker, transcript.scenario_id, "harness_error", operational_error);
        }
        if (worker->stop_requested) transcript.complete = false;
        if (!transcript.complete && !transcript.harness_failed && !worker->stop_requested &&
            scenarios::RawProbeClock::now() >= deadline)
            transcript.timed_out = true;
        transcript.unknown_auth_token_alias_compatibility_code = config.unknown_auth_token_alias_compatibility_code;
        transcript.connection_uri = endpoint_uri(worker, run_config, transcript.scenario_id);
        storage::EvidenceEvent stimulus;
        stimulus.scenario_id = transcript.scenario_id;
        stimulus.connection_id = worker->connection_id;
        stimulus.kind = transcript.stimulus_delivered ? "raw_probe_stimulus" : "raw_probe_partial_stimulus";
        stimulus.monotonic_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(scenarios::RawProbeClock::now()-worker->started).count();
        stimulus.wall_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        stimulus.detail = "ordinal=" + std::to_string(worker->context_ordinal) + " delivery_event_count=" + (transcript.delivery_event_count ? std::to_string(*transcript.delivery_event_count) : "none");
        const auto append_write = [&](const scenarios::RawProbeAcceptedWrite& write) {
            stimulus.detail += " stream=" + (write.stream_id ? std::to_string(*write.stream_id) : "none") +
                " channel=" + std::to_string(static_cast<unsigned>(write.write.channel)) +
                " operation=" + std::string(write.write.operation == scenarios::RawProbeOperation::StopSending ? "stop-sending" : "write") +
                " application_error=" + std::to_string(write.write.application_error) +
                " operation_accepted=" + (write.operation_accepted ? "true" : "false") +
                " accepted=" + std::to_string(write.accepted) + " fin=" + (write.fin_accepted ? "true" : "false") + " bytes=";
            stimulus.detail += hex_bytes(write.write.bytes);
            stimulus.detail += " accepted_event_count=" + (write.delivery_event_count
                ? std::to_string(*write.delivery_event_count) : "none");
            stimulus.detail += " prepared_event_count=" + (write.prepared_event_count
                ? std::to_string(*write.prepared_event_count) : "none");
        };
        append_write(transcript.setup);
        for (const auto& write : transcript.writes) append_write(write);
        store->append_events(worker->id,std::span(&stimulus,1));
        if (run_config.draft == DraftVersion::Draft18 &&
            scenarios::draft18_contribution_scenario(transcript.scenario_id)) {
            storage::EvidenceEvent uri = stimulus;
            uri.kind = "raw_probe_connection_uri";
            uri.detail = "connection_uri=" + *transcript.connection_uri +
                         " ordinal=" + std::to_string(worker->context_ordinal);
            store->append_events(worker->id,std::span(&uri,1));
        }
        const auto request_profiles = run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_request_profiles() : scenarios::draft21_request_profiles();
        const auto request_profile = std::find_if(request_profiles.begin(), request_profiles.end(),
            [&](const auto& profile) { return profile.definition.id == transcript.scenario_id; });
        if (request_profile != request_profiles.end() && request_profile->compatibility_error) {
            storage::EvidenceEvent mapping = stimulus;
            mapping.kind = config.unknown_auth_token_alias_compatibility_code
                ? "compatibility_error_mapping" : "unresolved_error_mapping";
            mapping.requirement_id = request_profile->requirement_id;
            mapping.detail = "UNKNOWN_AUTH_TOKEN_ALIAS REQUEST_ERROR profile=compatibility code=" +
                (config.unknown_auth_token_alias_compatibility_code
                    ? std::to_string(*config.unknown_auth_token_alias_compatibility_code) : "unconfigured; result=NOT_RUN") +
                "; request code is unassigned in the checked-in draft";
            store->append_events(worker->id, std::span(&mapping, 1));
        }
        return transcript;
    }

    std::optional<scenarios::RawProbeDefinition> resolve_track_probe(
        const RunConfig& run_config, std::string_view id) const {
        if (run_config.draft == DraftVersion::Draft18 && scenarios::draft18_contribution_scenario(id)) {
            std::vector<std::vector<std::byte>> contribution_namespace;
            std::vector<std::byte> contribution_name{std::byte{'x'}};
            if (run_config.track_fixture) {
                for (const auto& field : run_config.track_fixture->namespace_fields)
                    contribution_namespace.push_back(bytes_of(field));
                contribution_name = bytes_of(run_config.track_fixture->track_name);
            }
            auto contributions = scenarios::draft18_contribution_probes(
                run_config.timeout, contribution_namespace, contribution_name);
            const auto found = std::find_if(contributions.begin(), contributions.end(),
                [&](const auto& profile) { return profile.definition.id == id; });
            if (found == contributions.end()) throw std::invalid_argument("unknown contribution probe");
            return std::move(found->definition);
        }
        if (request_goaway_scenario(static_cast<unsigned>(run_config.draft),id)) {
            auto profiles = run_config.draft == DraftVersion::Draft18
                ? scenarios::draft18_request_goaway_probes(run_config.timeout)
                : scenarios::draft21_request_goaway_probes(run_config.timeout);
            const auto found = std::find_if(profiles.begin(),profiles.end(),
                [&](const auto& profile) { return profile.definition.id == id; });
            if (found == profiles.end()) throw std::invalid_argument("unknown request GOAWAY probe");
            return std::move(found->definition);
        }
        if (draft21_contribution_scenario(static_cast<unsigned>(run_config.draft), id)) {
            if (!run_config.track_fixture) throw std::invalid_argument("track probe requires a track fixture");
            std::vector<std::vector<std::byte>> contribution_namespace;
            for (const auto& field : run_config.track_fixture->namespace_fields)
                contribution_namespace.push_back(bytes_of(field));
            auto profiles = scenarios::draft21_contribution_probes(run_config.timeout,
                std::move(contribution_namespace), bytes_of(run_config.track_fixture->track_name));
            const auto found = std::find_if(profiles.begin(), profiles.end(),
                [&](const auto& profile) { return profile.definition.id == id; });
            if (found == profiles.end()) throw std::invalid_argument("unknown contribution probe");
            return std::move(found->definition);
        }
        const bool fetch = id == "cancel-fetch-request-with-open-data-stream" ||
            id == "reject-request-update-for-open-fetch" ||
            id == "d21-cancel-fetch-with-open-request-and-data-streams" ||
            id == "d21-failed-fetch-update-data-reset";
        const bool subscription = id == "cancel-subscribe-with-multiple-open-subgroups" ||
                                  id == "d21-cancel-subscribe-with-open-streams";
        const bool fetch_response = id == "d21-fetch-accepted" || id == "d21-fetch-rejected";
        const bool request_response = id == "d21-subscribe-accepted" || id == "d21-subscribe-rejected" ||
            id == "d21-subscribe-namespace-accepted" || id == "d21-subscribe-namespace-rejected" ||
            id == "d21-subscribe-tracks-accepted" || id == "d21-subscribe-tracks-rejected";
        const bool range_filter = id == "d21-duplicate-range-filter-key-in-update" ||
            id == "d21-range-filter-total-exceeds-negotiated-limit" ||
            id == "d21-range-filter-with-zero-negotiated-limit" ||
            id == "d21-range-filter-total-limit" ||
            id == "d21-range-filter-default-zero-limit" ||
            id == "d21-range-filter-update-total-limit";
        const bool discovery_overlap = discovery_overlap_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool first_fetch = fetch_first_object_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool immutable_repeat = immutable_repeat_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool object_repeat = object_repeat_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool group_order = fetch_group_order_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool notify_fetch = run_config.draft == DraftVersion::Draft21 && id == "d21-publish-state-notify-on-fetch";
        const bool notify_direction = subscriber_notify_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool gap_a = gap_raw_scenario(static_cast<unsigned>(run_config.draft), id);
        if (!fetch && !subscription && !fetch_response && !request_response && !range_filter && !discovery_overlap && !first_fetch && !group_order && !immutable_repeat && !object_repeat && !notify_fetch && !notify_direction && !gap_a) return std::nullopt;
        if (!run_config.track_fixture) throw std::invalid_argument("track probe requires a track fixture");
        std::vector<std::vector<std::byte>> name_space;
        for (const auto& field : run_config.track_fixture->namespace_fields)
            name_space.push_back(bytes_of(field));
        const auto execute = [&](auto profiles) -> std::optional<scenarios::RawProbeDefinition> {
            const auto found = std::find_if(profiles.begin(), profiles.end(),
                [&](const auto& profile) { return profile.definition.id == id; });
            if (found == profiles.end()) throw std::invalid_argument("unknown track probe");
            return std::move(found->definition);
        };
        if (gap_a) {
            const auto name = bytes_of(run_config.track_fixture->track_name);
            auto probes = scenarios::draft21_gap_a_probes(run_config.timeout, name_space, name);
            const auto found = std::find_if(probes.begin(), probes.end(),
                [&](const auto& profile) { return profile.definition.id == id; });
            if (found != probes.end()) return std::move(found->definition);
            return execute(scenarios::draft21_gap_a_token_probes(run_config.timeout, name_space, name));
        }
        if (immutable_repeat) return execute(run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_immutable_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name))
            : scenarios::draft21_immutable_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)));
        if (object_repeat) return execute(run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_object_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name))
            : scenarios::draft21_object_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)));
        if (group_order) return execute(run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_fetch_group_order_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name))
            : scenarios::draft21_fetch_group_order_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)));
        if (notify_fetch || notify_direction) return execute(scenarios::draft21_close_probes(run_config.timeout, name_space,
            bytes_of(run_config.track_fixture->track_name)));
        if (first_fetch) return execute(run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_fetch_first_object_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name))
            : scenarios::draft21_fetch_first_object_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)));
        if (discovery_overlap) return execute(run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_discovery_overlap_probes(run_config.timeout, name_space)
            : scenarios::draft21_discovery_overlap_probes(run_config.timeout, name_space));
        if (range_filter) return execute(scenarios::draft21_range_filter_probes(
            run_config.timeout, name_space, bytes_of(run_config.track_fixture->track_name)));
        if (request_response) return execute(scenarios::draft21_request_response_probes(
            run_config.timeout, name_space, bytes_of(run_config.track_fixture->track_name)));
        if (fetch_response) return execute(scenarios::draft21_fetch_response_probes(
            run_config.timeout, name_space, bytes_of(run_config.track_fixture->track_name)));
        if (fetch) return execute(run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_fetch_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name))
            : scenarios::draft21_fetch_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)));
        return execute(run_config.draft == DraftVersion::Draft18
            ? scenarios::draft18_subscription_cancel_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name))
            : scenarios::draft21_subscription_cancel_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)));
    }

    void run_draft18(Worker* worker,
                     transport::SessionTransport& listener,
                     const RunConfig& run_config, PublisherDriver* driver,
                     DriverHandle handle, const std::function<void()>& record_driver) {
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
            listener, std::move(definition),
            run_config.transport == TransportKind::WebTransport);
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
            const bool connected = std::any_of(evidence.begin(), evidence.end(),
                [](const auto& event) {
                    return event.kind == session::EvidenceKind::TransportEstablished;
                });
            if (handle.valid() && driver->poll(handle).status != DriverStatus::Running &&
                !connected)
                throw std::runtime_error("publisher exited before connecting");
            std::this_thread::sleep_for(1ms);
        }
        record_driver();
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
                     const RunConfig& run_config, PublisherDriver* driver,
                     DriverHandle handle, const std::function<void()>& record_driver) {
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
            draft21_setup_probe(run_config.scenario_ids.front()),
            run_config.transport == TransportKind::WebTransport);
        {
            // Slice A: the controller needs the scenario and, for a driven
            // native publisher, the exact URI it was handed.
            std::optional<scenarios::Draft21ExpectedConnectionUri> uri;
            const auto& scenario = run_config.scenario_ids.front();
            if (run_config.mode == RunMode::Driven &&
                run_config.transport == TransportKind::NativeQuic &&
                announcement_gap_scenario(21, scenario) &&
                gap_native_only_scenario(scenario)) {
                uri = scenarios::Draft21ExpectedConnectionUri{
                    authority_of(worker),
                    std::string(gap_native_uri_path_and_query(scenario))};
            }
            controller.configure_scenario(scenario, std::move(uri));
        }
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
            const bool connected = std::any_of(evidence.begin(), evidence.end(),
                [](const auto& event) {
                    return event.kind == scenarios::Draft21AnnouncementEventKind::TransportEstablished;
                });
            if (handle.valid() && driver->poll(handle).status != DriverStatus::Running &&
                !connected)
                throw std::runtime_error("publisher exited before connecting");
            std::this_thread::sleep_for(1ms);
        }
        record_driver();
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
    std::set<std::uint16_t> reserved_ports;
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
    if (!supports(config.draft) ||
        (config.mode == RunMode::Driven && !supports_driven()))
        return {RunStartStatus::Unsupported, {}, {}};
    if (config.scenario_ids.empty() || config.scenario_ids.size() > 100 ||
        config.timeout < 2ms || config.timeout > 3600000ms ||
        (config.track_fixture && !valid_fixture(*config.track_fixture)))
        return {RunStartStatus::InvalidConfig, {}, {}};
    std::set<std::string> selected;
    std::vector<scenarios::RawProbeDefinition> definitions;
    const auto draft = static_cast<unsigned>(config.draft);
    for (const auto& id : config.scenario_ids) {
        if (id.empty() || !selected.insert(id).second)
            return {RunStartStatus::InvalidConfig, {}, {}};
        if (!executable_scenario(draft, id) ||
            (config.scenario_ids.size() > 1 && !raw_probe_scenario(draft, id)))
            return {RunStartStatus::Unsupported, {}, {}};
        if ((scenario_requires_track(draft, id) || config.mode == RunMode::Driven) &&
            !config.track_fixture)
            return {RunStartStatus::InvalidConfig, {}, {}};
        if (config.draft == DraftVersion::Draft18 && id == kDuplicateSubscribeScenario &&
            config.timeout < 3ms)
            return {RunStartStatus::InvalidConfig, {}, {}};
        // Draft-21 gap slice A: transport-specific announcement scenarios.
        if (draft == 21 && gap_webtransport_only_scenario(id) &&
            config.transport != TransportKind::WebTransport)
            return {RunStartStatus::Unsupported, {}, {}};
        if (draft == 21 && announcement_gap_scenario(21, id) && config.track_fixture &&
            !gap_fixture_valid(id, config.track_fixture->namespace_fields))
            return {RunStartStatus::InvalidConfig, {}, {}};
        if (gap_raw_scenario(draft, id) && config.track_fixture) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : config.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::fetch_first_object_fixture_valid(fields, bytes_of(config.track_fixture->track_name)))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (draft == 21 && gap_native_only_scenario(id) &&
            config.transport != TransportKind::NativeQuic)
            return {RunStartStatus::Unsupported, {}, {}};
        if (immutable_repeat_scenario(draft, id) || object_repeat_scenario(draft, id) ||
            fetch_first_object_scenario(draft, id) || fetch_group_order_scenario(draft, id) ||
            (config.draft == DraftVersion::Draft21 && id == "d21-publish-state-notify-on-fetch") ||
            subscriber_notify_scenario(draft, id)) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : config.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::fetch_first_object_fixture_valid(fields, bytes_of(config.track_fixture->track_name)))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (config.draft == DraftVersion::Draft18 && scenarios::draft18_contribution_scenario(id) &&
            config.track_fixture) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : config.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::draft18_contribution_fixture_valid(fields, bytes_of(config.track_fixture->track_name)))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (discovery_overlap_scenario(draft, id)) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : config.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::discovery_overlap_namespace_valid(fields))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (config.draft == DraftVersion::Draft18 && scenarios::draft18_gap_a_native_only(id) &&
            config.transport != TransportKind::NativeQuic)
            return {RunStartStatus::Unsupported, {}, {}};
        if (config.draft == DraftVersion::Draft18) {
            const auto profiles = scenarios::draft18_close_profiles();
            const auto found = std::find_if(profiles.begin(), profiles.end(), [&id](const auto& profile) {
                return profile.scenario_id == id;
            });
            if (found != profiles.end() &&
                ((found->webtransport_only && config.transport != TransportKind::WebTransport) ||
                 (found->native_only && config.transport != TransportKind::NativeQuic)))
                return {RunStartStatus::Unsupported, {}, {}};
        }
        if (raw_probe_scenario(draft, id)) {
            try {
                auto definition = impl_->resolve_raw_probe(config, id);
                if (!definition || definition->id != id)
                    return {RunStartStatus::Unsupported, {}, {}};
                definitions.push_back(std::move(*definition));
            } catch (const std::invalid_argument&) {
                return {RunStartStatus::InvalidConfig, {}, {}};
            }
        }
    }
    // A replacement session needs a second port beside the run's own.
    if (impl_->config.port_start != 0 && impl_->config.port_end == impl_->config.port_start &&
        std::any_of(definitions.begin(), definitions.end(),
                    [](const auto& definition) { return definition.offer_replacement_session; }))
        return {RunStartStatus::PortExhausted, {}, {}};
    std::lock_guard lock(impl_->mutex);
    impl_->reap_finished();
    if (impl_->reserved_ports.size() >= impl_->config.maximum_active_runs) {
        return {RunStartStatus::PortExhausted, {}, {}};
    }
    std::unique_ptr<transport::SessionTransport> listener;
    transport::BoundEndpoint endpoint;
    std::string url;
    std::string path;
    std::string protocol;
    const bool ephemeral = impl_->config.port_start == 0;
    const auto attempts = ephemeral ? impl_->config.maximum_active_runs + 1 :
        static_cast<std::size_t>(impl_->config.port_end - impl_->config.port_start) + 1;
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        const auto port = ephemeral ? std::uint16_t{0} :
            static_cast<std::uint16_t>(impl_->config.port_start + attempt);
        if (port != 0 && impl_->reserved_ports.contains(port)) continue;
        auto created = impl_->create_listener(config, port,
            definitions.empty() ? std::nullopt : definitions.front().initial_peer_bidi_streams);
        if (created.listener) {
            if (impl_->reserved_ports.contains(created.endpoint.port)) continue;
            endpoint = created.endpoint;
            if (!impl_->config.advertised_address.empty()) endpoint.address = impl_->config.advertised_address;
            listener = std::move(created.listener);
            if (config.transport == TransportKind::WebTransport) {
                path = "/moq";
                protocol = config.draft == DraftVersion::Draft21 ? "moqt-21" : "moqt-18";
                const auto host = endpoint.address.find(':') != std::string::npos
                    ? "[" + endpoint.address + "]" : endpoint.address;
                url = "https://" + host + ":" + std::to_string(endpoint.port) + path;
            }
            break;
        }
        if (created.error != transport::NativeQuicListenerError::BindFailed)
            return {RunStartStatus::ListenerError, {}, {}};
    }
    if (!listener) return {RunStartStatus::PortExhausted, {}, {}};

    const auto id = impl_->store->create_run(config);
    auto worker = std::make_unique<Impl::Worker>();
    worker->id = id;
    worker->endpoint = endpoint;
    auto* worker_ptr = worker.get();
    impl_->workers.push_back(std::move(worker));
    impl_->reserved_ports.insert(endpoint.port);
    try {
        worker_ptr->thread = std::thread(
            [this, worker_ptr, listener = std::move(listener), config, definitions = std::move(definitions)] () mutable {
                impl_->run(worker_ptr, std::move(listener), config, std::move(definitions));
            });
    } catch (...) {
        impl_->workers.pop_back();
        impl_->reserved_ports.erase(endpoint.port);
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

bool NativeRunManager::supports_driven() const noexcept {
    return !impl_->config.driver_executable.empty();
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
