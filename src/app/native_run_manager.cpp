#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/draft_traits.h"
#include "moq/interop/app/lineage_run.h"
#include "moq/interop/app/lite_run.h"
#include "moq/interop/app/own_scenario_dispatch_22.h"
#include "moq/interop/app/publisher_capabilities.h"
#include "moq/interop/app/publisher_driver.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/app/unscored_probe_event_22.h"

#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/lineage_translate.h"
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
#include "moq/interop/scenarios/raw_probe_liveness.h"
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/draft21_response.h"
#include "moq/interop/scenarios/draft21_announcement.h"
#include "moq/interop/scenarios/draft21_contribution.h"
#include "moq/interop/scenarios/run_controller.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/transport/webtransport_listener.h"

#include <cstdio>
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

// lineage_run for every selected id except the implemented own draft 22 ones, which lineage_run refuses:
// those keep their draft 22 id in the execution list, in the requested order, and are dispatched natively
// on the draft 22 wire.
std::optional<LineageRun> plan_run(const RunConfig& requested) {
    if (requested.draft != DraftVersion::Draft22) return lineage_run(requested);
    // Each id is classified once, so the shared subset and the rebuilt list cannot disagree.
    std::vector<bool> own;
    RunConfig shared = requested;
    shared.scenario_ids.clear();
    for (const auto& id : requested.scenario_ids) {
        own.push_back(own_scenario_22(id).has_value());
        if (!own.back()) shared.scenario_ids.push_back(id);
    }
    auto plan = lineage_run(shared);
    if (!plan) return std::nullopt;
    auto implementations = std::move(plan->execution.scenario_ids);
    plan->execution.scenario_ids.clear();
    std::size_t next_shared = 0;
    for (std::size_t index = 0; index < requested.scenario_ids.size(); ++index)
        plan->execution.scenario_ids.push_back(own[index] ? requested.scenario_ids[index]
                                                          : std::move(implementations[next_shared++]));
    return plan;
}

// The identity rule, in one place. A run has two drafts that differ only for a draft 22 run:
// - its IDENTITY draft (the requested, wire draft): the ALPN its listeners offer, its stored row, the catalog it
//   is scored against, the scenario and requirement ids it stores, DriverRequest::draft, API answers;
// - its BEHAVIOR draft (the execution draft, `plan.execution.draft`, the draft 21 family for a draft 22 run):
//   which probes, profiles, controllers and evaluators run. On every run path, each by_draft(...) and draft
//   test in this file reads either the plan through these accessors or a `run_config` that is
//   `plan.execution`. The exception is the public static NativeRunManager::resolve_probe (used by tests),
//   which takes its caller's RunConfig as the execution config: a draft 22 RunConfig is not one, and its raw
//   ids reach by_draft(22), which throws std::logic_error.
// Never switch a behavior site to the identity draft unless it chooses identity.
DraftVersion identity_draft(const LineageRun& plan) { return plan.wire_draft; }
DraftVersion behavior_draft(const LineageRun& plan) { return plan.execution.draft; }
// A draft 22 run executed by lineage on the draft 21 family: its outcomes are translated to the identity
// draft's rows and scored against its catalog.
bool runs_by_lineage(const LineageRun& plan) { return identity_draft(plan) != behavior_draft(plan); }

// Adapter refusals. Draft 21-family scenario code reads a draft 22 peer's PUBLISH through
// scenarios::decode_publish_for_wire, which refuses what draft 21 has no form for (an Absolute {0,0}
// LOCATION_FILTER) and records the refusal on the calling thread. Several scenario sites drop that
// DecodeError (the scenario just never sees the PUBLISH), so the manager reads the record instead: a refusal
// is a harness error of the run (verdict Error), recorded as a `harness_error` event with this detail. Every
// decode of a run happens on its worker thread (contexts run one after another on it, inside run()'s
// ScopedWireDraft), which is where the record is read. Draft 18 and 21 runs never record one.
std::string adapter_refusal_message(std::string_view detail) {
    return "the draft 22 wire adapter refused a publisher message (" + std::string(detail) +
           "): the scenario code cannot be shown it, so the run cannot judge this publisher because of the "
           "runner's own limit, not a publisher fault";
}

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

// The identity rule at store time. The scenario layer works with `plan.execution`'s ids (for a draft 22 run, a
// shared scenario's draft 21 implementation id); everything the run stores or hands the publisher driver names
// the requested scenario (its draft 22 id). Every store site and DriverRequest goes through this one function;
// for drafts 18 and 21 it is the identity.
std::string stored_scenario_id(const LineageRun& plan, std::string_view execution_id) {
    return stamp_scenario_id(identity_draft(plan), execution_id);
}

// The same rule for a requirement id an event carries (a draft 21 row id the scenario layer attaches): a draft 22
// run stores its draft 22 row. A row with no single draft 22 successor is a bug, as for scenario ids.
std::string stored_requirement_id(const LineageRun& plan, const std::string& execution_row) {
    if (identity_draft(plan) != DraftVersion::Draft22 || !execution_row.starts_with("D21-")) return execution_row;
    const requirements::Outcome source{execution_row, requirements::OutcomeState::NotRun};
    const auto translated = requirements::translate_shared_outcomes(std::span(&source, 1));
    if (translated.size() != 1)
        throw std::logic_error("draft 21 row " + execution_row + " has no single draft 22 successor");
    return translated.front().requirement_id;
}

storage::EvidenceEvent driver_evidence(const DriverResult& result, const LineageRun& plan,
                                       std::string_view execution_id) {
    storage::EvidenceEvent event;
    event.wall_time_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    event.kind = "publisher_process";
    event.detail = serialize_driver_result(result);
    event.scenario_id = stored_scenario_id(plan, execution_id);
    return event;
}

}  // namespace

class NativeRunManager::Impl {
public:
    struct Worker {
        RunId id;
        transport::BoundEndpoint endpoint;
        // Second port held for the whole run when a definition offers a
        // replacement session; claimed in start() under the same lock and
        // released with the run's own port, so it can never leak mid-run.
        std::optional<std::uint16_t> replacement_port;
        std::atomic<bool> stop_requested{false};
        std::atomic<bool> finished{false};
        std::thread thread;
        scenarios::RawProbeClock::time_point started{scenarios::RawProbeClock::now()};
        std::size_t context_ordinal{0};
        std::string connection_id;
        std::vector<scenarios::RawProbeTranscript> transcripts;
        // A raw run already recorded an adapter refusal's harness_error (see adapter_refusal_message).
        bool adapter_refused{false};
    };

    Impl(std::shared_ptr<const requirements::RequirementCatalog> supplied_draft18,
         std::shared_ptr<const requirements::RequirementCatalog> supplied_draft21,
         std::shared_ptr<storage::RunStore> supplied_store,
         NativeRunManagerConfig supplied_config,
         std::shared_ptr<const requirements::RequirementCatalog> supplied_draft22,
         std::shared_ptr<const requirements::RequirementCatalog> supplied_moqlite06)
        : draft18(std::move(supplied_draft18)),
          draft21(std::move(supplied_draft21)),
          draft22(std::move(supplied_draft22)),
          moqlite06(std::move(supplied_moqlite06)),
          store(std::move(supplied_store)),
          config(std::move(supplied_config)) {
        // Every catalog must be complete: requirements::score() refuses an incomplete one.
        if (!draft18 || !store || draft18->draft != 18 || !draft18->complete ||
            (draft21 && (draft21->draft != 21 || !draft21->complete)) ||
            (draft22 && (draft22->draft != 22 || !draft22->complete)) ||
            // The lite catalog is staged (score_staged): it need not be complete.
            (moqlite06 && moqlite06->draft != 106) ||
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

    struct Tuning {
        std::optional<std::uint64_t> peer_bidi_streams;
        std::optional<std::uint64_t> peer_uni_streams;
        std::optional<std::uint64_t> peer_uni_stream_data;
        bool hold_uni_stream_credit;
        std::string_view path;
    };
    static Tuning tuning_of(const scenarios::RawProbeDefinition& definition) {
        return {definition.initial_peer_bidi_streams, definition.initial_peer_uni_streams,
                definition.initial_peer_uni_stream_data, definition.hold_uni_stream_credit, {}};
    }

    static std::string describe_listener_failure(const ListenerResult& result, std::uint16_t wanted_port) {
        static constexpr std::array<std::string_view, 11> names{
            "InvalidConfiguration", "UnsupportedBindAddress", "CertificateLoadFailed",
            "PrivateKeyLoadFailed", "CryptoInitializationFailed", "SocketOpenFailed",
            "SocketConfigurationFailed", "BindFailed", "BoundEndpointFailed", "AfterBindFailed",
            "TransportConfigurationFailed"};
        std::string text = " (wanted port " + std::to_string(wanted_port);
        if (result.error) {
            const auto index = static_cast<std::size_t>(*result.error);
            text += ", listener error " + std::string(index < names.size() ? names[index] : "unknown");
        } else if (result.listener) {
            text += ", bound port " + std::to_string(result.endpoint.port);
        }
        return text + ")";
    }

    // Identity/wire: the transport and the run's real (wire) draft, whose ALPN the listener accepts.
    // Scenario tuning arrives separately, from the family-draft definitions.
    ListenerResult create_listener(TransportKind transport, DraftVersion wire_draft, std::uint16_t port,
                                   Tuning tuning = {}) const {
        const auto& peer_bidi_streams = tuning.peer_bidi_streams;
        const auto path = tuning.path;
        transport::NativeQuicListenerConfig quic;
        // Bytes the publisher may write on a unidirectional data stream; with held
        // credit the runner never raises it.
        if (tuning.peer_uni_stream_data) quic.initial_max_stream_data_uni = *tuning.peer_uni_stream_data;
        quic.hold_uni_stream_credit = tuning.hold_uni_stream_credit;
        quic.bind_address = config.bind_address;
        quic.bind_port = port;
        // A stream-credit scenario starts the peer with only this many
        // bidirectional streams; WebTransport spends one on its CONNECT.
        if (peer_bidi_streams)
            quic.initial_max_streams_bidi = *peer_bidi_streams +
                (transport == TransportKind::WebTransport ? 1u : 0u);
        // The MOQT control stream, plus HTTP/3 control and two QPACK streams
        // under WebTransport, come out of the unidirectional credit.
        if (tuning.peer_uni_streams)
            quic.initial_max_streams_uni = *tuning.peer_uni_streams +
                (transport == TransportKind::WebTransport ? 4u : 1u);
        quic.certificate_path = config.certificate_path;
        quic.private_key_path = config.private_key_path;
        const std::string protocol(app::alpn(wire_draft));
        if (transport == TransportKind::WebTransport) {
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
        // moq-lite needs no QUIC DATAGRAM (plan decision (c)); the MoQ Transport drafts keep requiring it.
        if (wire_draft == DraftVersion::MoqLite06) quic.require_datagram = false;
        auto created = transport::NativeQuicListener::create(std::move(quic));
        const auto endpoint = created.listener ? created.listener->bound_endpoint() : transport::BoundEndpoint{};
        return {std::move(created.listener), endpoint, created.error};
    }

    // Every raw probe answers a publisher's PUBLISH_NAMESPACE by default (see
    // scenarios::apply_default_namespace_answer for the exceptions).
    static std::optional<scenarios::RawProbeDefinition> resolve_raw_probe(
        const NativeRunManagerConfig& config, const RunConfig& run_config, std::string_view id) {
        auto definition = resolve_raw_probe_definition(config, run_config, id);
        if (definition) finish_raw_probe(*definition, run_config);
        return definition;
    }

    // An own draft 22 scenario's probe, built on the draft 22 wire and finished like a family probe
    // (`run_config` is the execution config: the draft 21 family).
    static std::optional<scenarios::RawProbeDefinition> resolve_own_probe(const RunConfig& run_config,
                                                                          std::string_view id) {
        const scenarios::ScopedWireDraft wire(22);
        auto definition = own_probe_22(id, run_config);
        if (definition) finish_raw_probe(*definition, run_config);
        return definition;
    }

    static void finish_raw_probe(scenarios::RawProbeDefinition& definition, const RunConfig& run_config) {
        // Behavior: `run_config` is the execution config, so this is the family draft's namespace answer.
        scenarios::apply_default_namespace_answer(definition, static_cast<unsigned>(run_config.draft));
        // A definition that opted into a liveness follow-up asks for the track fixture.
        if (definition.liveness && run_config.track_fixture) {
            std::vector<std::vector<std::byte>> name_space;
            for (const auto& field : run_config.track_fixture->namespace_fields)
                name_space.push_back(bytes_of(field));
            scenarios::bind_liveness_track(definition, name_space, bytes_of(run_config.track_fixture->track_name));
        }
    }

    static std::optional<scenarios::RawProbeDefinition> resolve_raw_probe_definition(
        const NativeRunManagerConfig& config, const RunConfig& run_config, std::string_view id) {
        // Behavior: `run_config` is the execution config; every draft test below picks the family's probes.
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
        if (auto definition = resolve_track_probe(config, run_config, id)) return definition;
        const auto find = [id](auto profiles) -> std::optional<scenarios::RawProbeDefinition> {
            const auto found = std::find_if(profiles.begin(), profiles.end(),
                [id](const auto& profile) { return profile.definition.id == id; });
            if (found == profiles.end()) return std::nullopt;
            return std::move(found->definition);
        };
        // Behavior: the execution draft (never 22) chooses the family's profile tables.
        return app::by_draft(run_config.draft,
            [&]() -> std::optional<scenarios::RawProbeDefinition> {
            if (auto value = find(scenarios::draft18_response_probes(run_config.timeout))) return value;
            if (auto value = find(scenarios::draft18_peer_close_probes(run_config.timeout))) return value;
            if (auto value = find(scenarios::draft18_request_profiles(run_config.timeout))) return value;
            const auto profiles = scenarios::draft18_close_profiles();
            if (std::none_of(profiles.begin(), profiles.end(), [id](const auto& profile) {
                    return profile.scenario_id == id;
                })) return std::nullopt;
            std::vector<std::vector<std::byte>> close_namespace;
            std::vector<std::byte> close_name;
            if (run_config.track_fixture) {
                for (const auto& field : run_config.track_fixture->namespace_fields)
                    close_namespace.push_back(bytes_of(field));
                close_name = bytes_of(run_config.track_fixture->track_name);
            }
            return scenarios::draft18_close_probe(id, run_config.timeout, std::move(close_namespace),
                                                  std::move(close_name));
            },
            [&]() -> std::optional<scenarios::RawProbeDefinition> {
            // The run's track reaches the probe only on the draft 22 wire (the builder ignores it on wire 21).
            std::vector<std::vector<std::byte>> response_namespace;
            std::vector<std::byte> response_name;
            if (run_config.track_fixture) {
                for (const auto& field : run_config.track_fixture->namespace_fields)
                    response_namespace.push_back(bytes_of(field));
                response_name = bytes_of(run_config.track_fixture->track_name);
            }
            if (auto value = find(scenarios::draft21_response_probes(run_config.timeout,
                    response_namespace, response_name))) return value;
            if (auto value = find(scenarios::draft21_peer_close_probes(run_config.timeout))) return value;
            // The same names reach the request profiles and the SUBSCRIBE / TRACK_STATUS close probes (on
            // wire 22 only).
            if (auto value = find(scenarios::draft21_request_profiles(run_config.timeout,
                    response_namespace, response_name))) return value;
            return find(scenarios::draft21_close_probes(run_config.timeout, {}, {std::byte{'x'}},
                                                        std::move(response_namespace), std::move(response_name)));
            });
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
        // Behavior: `run_config` is the execution config and `scenario` an execution id; the URI path and
        // query a few scenarios test are the family's.
        const auto tail = run_config.transport == TransportKind::NativeQuic &&
                run_config.draft == DraftVersion::Draft21 &&
                announcement_gap_scenario(21, scenario)
            ? std::string(gap_native_uri_path_and_query(scenario)) : std::string("/moq");
        auto uri = (run_config.transport == TransportKind::WebTransport ? "https://" : "moqt://") +
                   authority_of(worker) + tail;
        // Section 3.1.1: one scenario names a moqt URI with no host.
        if (run_config.draft == DraftVersion::Draft18 && run_config.transport == TransportKind::NativeQuic &&
            scenarios::draft18_contribution_empty_host_scenario(scenario))
            return "moqt://:" + std::to_string(worker->endpoint.port) + "/moq";
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

    // `execution_id` is the scenario layer's id; the event is stored under the requested one.
    void append_context_event(Worker* worker, const LineageRun& plan, std::string_view execution_id,
                              std::string_view kind, std::string detail) {
        storage::EvidenceEvent event;
        event.scenario_id = stored_scenario_id(plan, execution_id);
        event.kind = kind;
        event.detail = std::move(detail);
        stamp_context_event(worker, event);
        store->append_events(worker->id, std::span(&event, 1));
    }

    DriverHandle start_context_driver(Worker* worker, const LineageRun& plan,
                                     std::string_view id, PublisherDriver& driver) {
        const RunConfig& run_config = plan.execution;
        DriverRequest request;
        request.executable = config.driver_executable;
        request.arguments = config.driver_arguments;
        request.run_id = worker->id;
        // Identity: the publisher is told the run's real (wire) draft and the scenario that was selected for
        // it (the requested draft 22 id for a draft 22 run), as every stored event names it.
        request.scenario_id = stored_scenario_id(plan, id);
        // Behavior: the endpoint is built from the execution config, because the path and query of a few
        // scenarios' URIs are part of what the scenario layer tests (keyed by the implementation id).
        request.endpoint = endpoint_uri(worker, run_config, id);
        request.draft = identity_draft(plan);
        request.transport = run_config.transport;
        request.track = *run_config.track_fixture;
        request.fixture = config.driver_fixture;
        request.tls_ca = config.driver_tls_ca.empty() ? config.certificate_path : config.driver_tls_ca;
        request.log_dir = config.driver_log_root / worker->id /
                          (std::to_string(worker->context_ordinal) + "-" + request.scenario_id);
        request.scenario_timeout = run_config.timeout;
        request.process_timeout = run_config.timeout + 1000ms;
        const auto started = driver.start(request);
        if (started.status == DriverStartStatus::Started) return started.handle;
        DriverResult failure;
        failure.error = started.error;
        auto event = driver_evidence(failure, plan, id);
        stamp_context_event(worker, event);
        store->append_events(worker->id, std::span(&event, 1));
        throw std::runtime_error("publisher driver start failed: " + started.error);
    }

    // Evaluation runs on the family draft (execution); what is stored and scored is the wire draft's.
    void finalize_raw_family(Worker* worker, const LineageRun& plan, bool operational_error,
                             std::string_view last_scenario_id) {
        const RunConfig& run_config = plan.execution;
        const auto outcomes_18 = [&]() -> std::vector<requirements::Outcome> {
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
            return requirements::evaluate_draft18(*draft18, contexts);
        };
        const auto outcomes_21 = [&]() -> std::vector<requirements::Outcome> {
            return requirements::evaluate_draft21_raw_probes(*draft21, worker->transcripts);
        };
        // Behavior: the execution draft's evaluators judge the transcripts.
        std::vector<requirements::Outcome> outcomes = app::by_draft(run_config.draft, outcomes_18, outcomes_21);
        // Pointer-returning lambdas: a reference-returning by_draft trips -Wdangling-reference.
        // Behavior, and identity for drafts 18 and 21 only: the execution draft's catalog, which a lineage run
        // replaces with the identity draft's catalog below.
        const auto* catalog = app::by_draft(run_config.draft,
            [&]() -> const requirements::RequirementCatalog* { return draft18.get(); },
            [&]() -> const requirements::RequirementCatalog* { return draft21.get(); });
        if (runs_by_lineage(plan)) {
            // A draft 22 run: the draft 21 evaluators' complete outcome set, keyed by draft 22 rows, with
            // the own draft 22 evaluators' outcomes (already draft 22 rows) for the rows they reach.
            const auto own = evaluate_own_draft22(*draft22, worker->transcripts);
            outcomes = lineage_outcomes(*draft22, outcomes, own);
            catalog = draft22.get();
        }
        // A refusal met while evaluating that no context recorded (each context's own decodes, judges
        // included, were checked when it ended): it cannot be tied to one context, so it is recorded under
        // the run's last context, like a stop request.
        if (const auto refusal = scenarios::take_adapter_refusal()) {
            if (!worker->adapter_refused)
                append_context_event(worker, plan, last_scenario_id, "harness_error", adapter_refusal_message(*refusal));
            worker->adapter_refused = true;
            operational_error = true;
        }
        // Rows whose every scenario needs a capability the publisher declared absent are
        // not applicable to this run (they leave the score denominators). The draft number is
        // the catalog's: draft 22 rows name draft 22 scenarios, which the registry forwards to
        // their draft 21 implementations.
        apply_publisher_capabilities(draft_number(identity_draft(plan)), *catalog,
                                     run_config.publisher_capabilities, outcomes);
        auto summary = requirements::score(*catalog, outcomes);
        if (operational_error || worker->stop_requested) summary.verdict = requirements::RunVerdict::Error;
        // An errored run always says why: operational errors were recorded where they
        // happened, and a stop request is recorded here.
        if (worker->stop_requested && !operational_error)
            append_context_event(worker, plan, last_scenario_id, "run_stopped",
                                 "the run was stopped before every selected context finished");
        store->finalize(worker->id, summary, outcomes);
    }

    static bool publisher_exit_expected(const scenarios::RawProbeTranscript& transcript,
                                        bool exit_is_evidence) {
        // A publisher that ends the session itself typically exits with a failure status.
        // Once the probe was delivered in full and the transport recorded that close, the
        // exit status is a consequence of the observed behavior, not a separate harness fault.
        const bool closed_by_peer = transcript.complete && !transcript.harness_failed &&
            std::any_of(transcript.events.begin(), transcript.events.end(), [](const auto& event) {
                return std::holds_alternative<transport::PeerCloseEvent>(event);
            });
        // The runner ends a context whose evidence hit a recording limit, so a publisher that
        // exits because of that is a consequence of the runner, not a harness fault.
        return refused_for_missing_datagram(transcript) || closed_by_peer ||
               transcript.event_limit_reached || (exit_is_evidence && !transcript.harness_failed);
    }

    static bool refused_for_missing_datagram(const scenarios::RawProbeTranscript& transcript) {
        constexpr std::string_view reason = "QUIC DATAGRAM not negotiated";
        if (transcript.transport_established || transcript.harness_failed || transcript.events.size() != 1)
            return false;
        const auto* close = std::get_if<transport::LocalCloseEvent>(&transcript.events.front());
        return close != nullptr &&
               std::string_view(reinterpret_cast<const char*>(close->reason.data()), close->reason.size()) == reason;
    }

    void run_raw_family(Worker* worker, std::unique_ptr<transport::SessionTransport>& listener,
                        const LineageRun& plan,
                        std::vector<scenarios::RawProbeDefinition> definitions) {
        const RunConfig& run_config = plan.execution;
        PublisherDriver driver;
        DriverHandle handle;
        bool operational_error = false;
        std::string current_id;
        const auto retire_driver = [&] {
            if (!handle.valid()) return false;
            const auto result = driver.stop(handle);
            handle = {};
            auto event = driver_evidence(result, plan, current_id);
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
                    auto replacement = create_listener(run_config.transport, identity_draft(plan), worker->endpoint.port,
                                                       tuning_of(definitions[index]));
                    if (!replacement.listener || replacement.endpoint.port != worker->endpoint.port)
                        throw std::runtime_error("raw context listener could not rebind reserved run port" +
                            describe_listener_failure(replacement, worker->endpoint.port));
                    listener = std::move(replacement.listener);
                }
                if (worker->stop_requested) break;
                append_context_event(worker, plan, current_id, "context_ready",
                    "endpoint=" + endpoint_uri(worker, run_config, current_id) +
                    " reconnect=fresh-session publisher_identity=unverified");
                if (worker->stop_requested) break;
                if (run_config.mode == RunMode::Driven)
                    handle = start_context_driver(worker, plan, current_id, driver);
                const bool exit_is_evidence = definitions[index].publisher_exit_is_evidence;
                auto transcript = collect_raw_probe(worker, *listener, plan,
                    std::move(definitions[index]), &driver, handle);
                bool process_error = retire_driver() &&
                    !scenarios::draft18_contribution_empty_host_scenario(current_id);
                if (process_error && publisher_exit_expected(transcript, exit_is_evidence)) {
                    // The runner itself caused this exit: it refused a client that never offered
                    // QUIC DATAGRAM (draft 21 Section 6.2), or the scenario withholds or rejects
                    // what the publisher asked for. A publisher that gives up afterwards is the
                    // consequence of what was observed, not a harness failure, so the transport
                    // evidence stays scoreable.
                    append_context_event(worker, plan, current_id, "publisher_exit_after_refusal",
                                         "publisher process exited after the runner refused or rejected it");
                } else if (process_error) {
                    transcript.complete = false;
                    transcript.harness_failed = true;
                    if (transcript.harness_failure_reason.empty())
                        transcript.harness_failure_reason = "publisher process failed";
                    append_context_event(worker, plan, current_id, "harness_error", "publisher process failed");
                }
                // An adapter refusal during this context fails it like any harness error (the contexts after
                // it do not run). Gates and courtesy answers decode while the context runs; judges decode
                // when evaluated, so a draft 22 run evaluates this transcript now, on this thread, to meet
                // theirs here (finalize_raw_family evaluates every transcript again).
                if (runs_by_lineage(plan))
                    (void)requirements::evaluate_draft21_raw_probes(*draft21, std::span(&transcript, 1));
                if (const auto refusal = scenarios::take_adapter_refusal()) {
                    transcript.complete = false;
                    transcript.harness_failed = true;
                    transcript.harness_failure_reason = adapter_refusal_message(*refusal);
                    append_context_event(worker, plan, current_id, "harness_error", transcript.harness_failure_reason);
                    worker->adapter_refused = true;
                }
                if (worker->stop_requested) transcript.complete = false;
                operational_error = operational_error || transcript.harness_failed;
                worker->transcripts.push_back(std::move(transcript));
                const auto& completed = worker->transcripts.back();
                append_context_event(worker, plan, current_id,
                    completed.complete && !completed.harness_failed ? "context_complete" : "context_end",
                    "complete=" + std::string(completed.complete ? "true" : "false") +
                    " timed_out=" + (completed.timed_out ? "true" : "false") +
                    " event_limit=" + (completed.event_limit_reached ? "true" : "false") +
                    " cancelled=" + (worker->stop_requested ? "true" : "false"));
                // An unscored draft 22 probe: its verdict is evidence only, never a requirement outcome.
                // Identity: unscored probes exist only on the draft 22 wire.
                if (identity_draft(plan) == DraftVersion::Draft22) {
                    if (const auto unscored = evaluate_unscored_probe_22(completed)) {
                        const char* verdict = !unscored->verdict ? "not_run" : *unscored->verdict ? "pass" : "fail";
                        // The detail keeps its earlier prefix and adds the reason, in the format the HTTP layer
                        // reads back as the event's structured verdict and reason (unscored_probe_event_22.h).
                        append_context_event(worker, plan, current_id, kUnscoredProbeVerdictEvent,
                            unscored_probe_detail_22(unscored->evaluator, verdict, unscored->reason));
                    }
                }
                if (operational_error || worker->stop_requested) {
                    // Contexts that never ran are named, so the run explains its own end.
                    std::string not_run;
                    for (std::size_t later = index + 1; later < definitions.size(); ++later)
                        not_run += (not_run.empty() ? "" : ",") + stored_scenario_id(plan, definitions[later].id);
                    if (operational_error && !not_run.empty())
                        append_context_event(worker, plan, current_id, "run_aborted",
                            "the run ended after context " + std::to_string(index + 1) + " of " +
                            std::to_string(definitions.size()) + " because of a harness error (" +
                            (completed.harness_failure_reason.empty() ? "see harness_error" : completed.harness_failure_reason) +
                            "); contexts not run: " + not_run);
                    break;
                }
            }
        } catch (const std::exception& error) {
            operational_error = true;
            try {
                retire_driver();
                append_context_event(worker, plan, current_id, "harness_error", error.what());
            } catch (const std::exception& cleanup_error) {
                std::cerr << "raw context cleanup failed: " << cleanup_error.what() << '\n';
            }
        }
        listener.reset();
        finalize_raw_family(worker, plan, operational_error, current_id);
    }

    // The worker thread's body. `plan.execution` drives the scenario layer (family draft,
    // implementation ids); `plan.wire_draft` is what the peer speaks. Scenario evidence is
    // decoded and evaluated on this thread, so the wire draft is set for all of it here.
    void run(Worker* worker,
             std::unique_ptr<transport::SessionTransport> listener,
             LineageRun plan,
             std::vector<scenarios::RawProbeDefinition> definitions) {
        const scenarios::ScopedWireDraft wire(draft_number(identity_draft(plan)));
        const RunConfig& run_config = plan.execution;
        PublisherDriver driver;
        DriverHandle handle;
        auto record_driver = [&] {
            if (!handle.valid()) return;
            const auto result = driver.stop(handle);
            handle = {};
            const auto event = driver_evidence(result, plan, run_config.scenario_ids.front());
            store->append_events(worker->id, std::span(&event, 1));
        };
        try {
            if (!definitions.empty()) {
                run_raw_family(worker, listener, plan, std::move(definitions));
            } else {
                if (run_config.mode == RunMode::Driven) {
                    // Behavior: the endpoint follows the execution config (see start_context_driver).
                    const std::string endpoint = endpoint_uri(
                        worker, run_config, run_config.scenario_ids.front());
                    DriverRequest request;
                    request.executable = config.driver_executable;
                    request.arguments = config.driver_arguments;
                    request.run_id = worker->id;
                    // Identity: the requested scenario id and the wire draft.
                    request.scenario_id = stored_scenario_id(plan, run_config.scenario_ids.front());
                    request.endpoint = endpoint;
                    request.draft = identity_draft(plan);
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
                            failure, plan, run_config.scenario_ids.front());
                        store->append_events(worker->id, std::span(&event, 1));
                        throw std::runtime_error("publisher driver start failed: " + started.error);
                    }
                    handle = started.handle;
                }
                // Behavior: the execution draft picks the typed controller (draft 21's for a draft 22 run).
                app::by_draft(run_config.draft,
                    [&] { run_draft18(worker, *listener, run_config, &driver, handle, record_driver); },
                    [&] { run_draft21(worker, *listener, plan, &driver, handle, record_driver); });
            }
        } catch (const std::exception& error) {
            std::cerr << "publisher run " << worker->id << " failed: "
                      << error.what() << '\n';
            try {
                record_driver();
                storage::EvidenceEvent reason;
                reason.kind = "harness_error";
                reason.detail = error.what();
                // A refusal no path took yet (another error ended the run first) is named here too.
                if (const auto refusal = scenarios::take_adapter_refusal())
                    reason.detail += "; " + adapter_refusal_message(*refusal);
                reason.scenario_id = run_config.scenario_ids.empty()
                    ? std::string{} : stored_scenario_id(plan, run_config.scenario_ids.front());
                reason.wall_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                try { store->append_events(worker->id, std::span(&reason, 1)); } catch (...) {}
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
            if (worker->replacement_port) reserved_ports.erase(*worker->replacement_port);
        }
        worker->finished = true;
    }

    scenarios::RawProbeTranscript collect_raw_probe(Worker* worker, transport::SessionTransport& listener,
                       const LineageRun& plan, scenarios::RawProbeDefinition definition,
                       PublisherDriver* driver, DriverHandle handle) {
        const RunConfig& run_config = plan.execution;
        // Every event of this context is stored under the requested scenario id; the transcript (and so the
        // evaluators) keeps the scenario layer's id.
        const std::string stored_id = stored_scenario_id(plan, definition.id);
        const auto started = scenarios::RawProbeClock::now();
        const auto deadline = started + run_config.timeout;
        // A replacement-session definition gets a second listener at its own
        // port and path, observed alongside the first session.
        std::unique_ptr<transport::SessionTransport> replacement;
        std::string replacement_uri;
        if (definition.offer_replacement_session) {
            // The port was reserved by start(); ephemeral mode binds port 0.
            const std::uint16_t port = worker->replacement_port.value_or(0);
            auto created = create_listener(run_config.transport, identity_draft(plan), port,
                Tuning{std::nullopt, std::nullopt, std::nullopt, false, kReplacementPath});
            if (!created.listener) {
                throw std::runtime_error("replacement session listener could not be created");
            }
            replacement = std::move(created.listener);
            const auto host = worker->endpoint.address.find(':') != std::string::npos
                ? "[" + worker->endpoint.address + "]" : worker->endpoint.address;
            replacement_uri = (run_config.transport == TransportKind::WebTransport ? "https://" : "moqt://") +
                host + ":" + std::to_string(created.endpoint.port) + std::string(kReplacementPath);
            if (definition.bind_alternate_uri) definition.bind_alternate_uri(definition, replacement_uri);
        }
        struct ReplacementGuard {
            std::function<void()> release;
            ~ReplacementGuard() { release(); }
        } guard{[&] {
            // Runs after the controller (declared next) is gone. The port
            // reservation itself is released with the run.
            replacement.reset();
        }};
        scenarios::RawProbeController controller(listener, std::move(definition), replacement.get(),
                                                 std::move(replacement_uri));
        std::size_t recorded = 0;
        std::size_t replacement_recorded = 0;
        // Stores the recorded events before `limit` (and any replacement-session events) that
        // are not stored yet.
        const auto persist = [&](const scenarios::RawProbeTranscript& transcript, std::size_t limit,
                                 scenarios::RawProbeClock::time_point now) {
            std::vector<storage::EvidenceEvent> batch;
            for (; recorded < limit; ++recorded) {
                storage::EvidenceEvent event;
                event.scenario_id = stored_id;
                // An event may be stored after it arrived (a growing tail is held back), so it
                // carries the time it arrived, not the time it was stored.
                const auto arrived = recorded < transcript.event_times.size() ? transcript.event_times[recorded] : now;
                event.monotonic_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(arrived-worker->started).count();
                event.wall_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch() - (now - arrived)).count();
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
                event.scenario_id = stored_id;
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
        };
        std::string operational_error;
        try {
            while (!worker->stop_requested) {
                const auto now = scenarios::RawProbeClock::now();
                const auto& transcript = controller.poll(now);
                const bool finished = transcript.complete || transcript.harness_failed ||
                    transcript.event_limit_reached || transcript.timed_out || now >= deadline;
                // The last event may still grow by merging (RawProbeController::coalesce); it is
                // stored once settled or when the context ends, so stored rows equal the transcript.
                persist(transcript, finished ? transcript.events.size() : controller.settled_event_count(), now);
                if (finished) break;
                if (handle.valid()) {
                    const auto process = driver->poll(handle);
                    // A publisher may decline the host-less URI of the section 3.1.1 scenario.
                    if (process.status != DriverStatus::Running && !transcript.transport_established &&
                        scenarios::draft18_contribution_empty_host_scenario(transcript.scenario_id)) break;
                    if (driver_failed(process) ||
                        (process.status != DriverStatus::Running && !transcript.transport_established))
                        throw std::runtime_error("publisher process failed during raw context");
                }
                std::this_thread::sleep_for(1ms);
            }
        } catch (const std::exception& error) {
            operational_error = error.what();
        }
        // However the loop ended, every event the transcript holds is stored in full.
        try {
            persist(controller.transcript(), controller.transcript().events.size(), scenarios::RawProbeClock::now());
        } catch (const std::exception& error) {
            if (operational_error.empty()) operational_error = error.what();
        }
        auto transcript = controller.transcript();
        if (!operational_error.empty()) {
            transcript.complete = false;
            transcript.harness_failed = true;
            transcript.harness_failure_reason = operational_error;
            append_context_event(worker, plan, transcript.scenario_id, "harness_error", operational_error);
        } else if (transcript.harness_failed) {
            append_context_event(worker, plan, transcript.scenario_id, "harness_error",
                transcript.harness_failure_reason.empty() ? "harness failure without a recorded reason"
                                                          : transcript.harness_failure_reason);
        }
        // Truncated evidence is its own, scored-as-nothing, condition of this context only.
        if (transcript.event_limit_reached)
            append_context_event(worker, plan, transcript.scenario_id, "context_event_limit",
                                 transcript.event_limit_reason);
        if (worker->stop_requested) transcript.complete = false;
        if (!transcript.complete && !transcript.harness_failed && !transcript.event_limit_reached &&
            !worker->stop_requested && scenarios::RawProbeClock::now() >= deadline)
            transcript.timed_out = true;
        transcript.unknown_auth_token_alias_compatibility_code = config.unknown_auth_token_alias_compatibility_code;
        transcript.denied_authorization_token = config.denied_authorization_token;
        transcript.connection_uri = endpoint_uri(worker, run_config, transcript.scenario_id);
        storage::EvidenceEvent stimulus;
        stimulus.scenario_id = stored_id;
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
        if (transcript.liveness) {
            // The follow-up is recorded beside the stimulus, never as part of it.
            storage::EvidenceEvent follow_up = stimulus;
            follow_up.kind = "raw_probe_liveness_followup";
            follow_up.detail = "ordinal=" + std::to_string(worker->context_ordinal) +
                " stream=" + (transcript.liveness->write.stream_id ? std::to_string(*transcript.liveness->write.stream_id) : "none") +
                " accepted=" + std::to_string(transcript.liveness->write.accepted) +
                " accepted_event_count=" + (transcript.liveness->write.delivery_event_count
                    ? std::to_string(*transcript.liveness->write.delivery_event_count) : "none") +
                " anchor_event_count=" + std::to_string(transcript.liveness->anchor_event_count) +
                " answered=" + (transcript.liveness->answered_at ? "true" : "false") +
                " bytes=" + hex_bytes(transcript.liveness->write.write.bytes);
            store->append_events(worker->id,std::span(&follow_up,1));
        }
        std::vector<storage::EvidenceEvent> courtesy_events;
        courtesy_events.reserve(transcript.courtesy_writes.size());
        for (const auto& courtesy : transcript.courtesy_writes) {
            // Responses the runner volunteered to requests the publisher opened;
            // they are context for the transcript, not part of the stimulus proof.
            storage::EvidenceEvent event = stimulus;
            event.kind = "raw_probe_courtesy_write";
            event.stream_id = std::to_string(courtesy.stream_id);
            const char* kind = "namespace_ok";
            if (courtesy.kind == scenarios::RawProbeCourtesyKind::PublishOk) kind = "publish_ok";
            else if (courtesy.kind == scenarios::RawProbeCourtesyKind::PublishError) kind = "publish_error";
            else if (courtesy.kind == scenarios::RawProbeCourtesyKind::UpdateOk) kind = "update_ok";
            event.detail = std::string("courtesy=") + kind + " accepted_event_count=" +
                std::to_string(courtesy.event_count) + " ordinal=" + std::to_string(worker->context_ordinal);
            courtesy_events.push_back(std::move(event));
        }
        if (!courtesy_events.empty()) store->append_events(worker->id, courtesy_events);
        // Behavior: the execution draft and id (the event itself is stored under the requested id).
        if (run_config.draft == DraftVersion::Draft18 &&
            scenarios::draft18_contribution_scenario(transcript.scenario_id)) {
            storage::EvidenceEvent uri = stimulus;
            uri.kind = "raw_probe_connection_uri";
            uri.detail = "connection_uri=" + *transcript.connection_uri +
                         " ordinal=" + std::to_string(worker->context_ordinal);
            store->append_events(worker->id,std::span(&uri,1));
        }
        // Behavior: the family's request profiles; the requirement id they name is translated to identity below.
        const auto request_profiles = app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_request_profiles(); },
            [&] { return scenarios::draft21_request_profiles(); });
        const auto request_profile = std::find_if(request_profiles.begin(), request_profiles.end(),
            [&](const auto& profile) { return profile.definition.id == transcript.scenario_id; });
        if (request_profile != request_profiles.end() && request_profile->compatibility_error) {
            storage::EvidenceEvent mapping = stimulus;
            mapping.kind = config.unknown_auth_token_alias_compatibility_code
                ? "compatibility_error_mapping" : "unresolved_error_mapping";
            mapping.requirement_id = stored_requirement_id(plan, request_profile->requirement_id);
            mapping.detail = "UNKNOWN_AUTH_TOKEN_ALIAS REQUEST_ERROR profile=compatibility code=" +
                (config.unknown_auth_token_alias_compatibility_code
                    ? std::to_string(*config.unknown_auth_token_alias_compatibility_code) : "unconfigured; result=NOT_RUN") +
                "; request code is unassigned in the checked-in draft";
            store->append_events(worker->id, std::span(&mapping, 1));
        }
        return transcript;
    }

    static std::optional<scenarios::RawProbeDefinition> resolve_track_probe(
        const NativeRunManagerConfig& config, const RunConfig& run_config, std::string_view id) {
        // Behavior: `run_config` is the execution config and `id` an execution id; every draft test and
        // by_draft(...) in this function picks the family's probe tables.
        if (run_config.draft == DraftVersion::Draft18 && scenarios::draft18_contribution_scenario(id)) {
            std::vector<std::vector<std::byte>> contribution_namespace;
            std::vector<std::byte> contribution_name{std::byte{'x'}};
            if (run_config.track_fixture) {
                for (const auto& field : run_config.track_fixture->namespace_fields)
                    contribution_namespace.push_back(bytes_of(field));
                contribution_name = bytes_of(run_config.track_fixture->track_name);
            }
            // The operator's credentials for the section 10.2.2 token rows (D18-10-2-2-MUST-008, -010).
            scenarios::Draft18TokenCredentials token_credentials;
            if (config.invalid_auth_token)
                token_credentials.invalid = scenarios::Draft18TokenCredential{
                    config.invalid_auth_token->token_type, config.invalid_auth_token->value};
            if (config.expired_auth_token)
                token_credentials.expired = scenarios::Draft18TokenCredential{
                    config.expired_auth_token->token_type, config.expired_auth_token->value};
            token_credentials.denied = config.denied_authorization_token;
            auto contributions = scenarios::draft18_contribution_probes(
                run_config.timeout, contribution_namespace, contribution_name, std::move(token_credentials));
            const auto found = std::find_if(contributions.begin(), contributions.end(),
                [&](const auto& profile) { return profile.definition.id == id; });
            if (found == contributions.end()) throw std::invalid_argument("unknown contribution probe");
            return std::move(found->definition);
        }
        if (request_goaway_scenario(static_cast<unsigned>(run_config.draft),id)) {
            // The run's namespace reaches the probes only on the draft 22 wire (ignored on wire 21).
            std::vector<std::vector<std::byte>> goaway_namespace;
            if (run_config.track_fixture)
                for (const auto& field : run_config.track_fixture->namespace_fields)
                    goaway_namespace.push_back(bytes_of(field));
            auto profiles = app::by_draft(run_config.draft,
                [&] { return scenarios::draft18_request_goaway_probes(run_config.timeout); },
                [&] { return scenarios::draft21_request_goaway_probes(run_config.timeout, goaway_namespace); });
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
                std::move(contribution_namespace), bytes_of(run_config.track_fixture->track_name),
                config.denied_authorization_token.value_or(std::string{}),
                scenarios::Draft21TokenCredentials{config.invalid_auth_token, config.expired_auth_token});
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
        // Behavior: execution draft (see the top of this function).
        const bool discovery_overlap = discovery_overlap_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool first_fetch = fetch_first_object_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool immutable_repeat = immutable_repeat_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool object_repeat = object_repeat_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool group_order = fetch_group_order_scenario(static_cast<unsigned>(run_config.draft),id);
        const bool notify_fetch = run_config.draft == DraftVersion::Draft21 && id == "d21-publish-state-notify-on-fetch";
        const bool notify_direction = subscriber_notify_scenario(static_cast<unsigned>(run_config.draft),id) ||
            established_update_scenario(static_cast<unsigned>(run_config.draft),id);
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
        if (immutable_repeat) return execute(app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_immutable_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); },
            [&] { return scenarios::draft21_immutable_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); }));
        if (object_repeat) return execute(app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_object_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); },
            [&] { return scenarios::draft21_object_repeat_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); }));
        if (group_order) return execute(app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_fetch_group_order_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); },
            [&] { return scenarios::draft21_fetch_group_order_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); }));
        if (notify_fetch || notify_direction) return execute(scenarios::draft21_close_probes(run_config.timeout, name_space,
            bytes_of(run_config.track_fixture->track_name)));
        if (first_fetch) return execute(app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_fetch_first_object_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); },
            [&] { return scenarios::draft21_fetch_first_object_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); }));
        if (discovery_overlap) return execute(app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_discovery_overlap_probes(run_config.timeout, name_space); },
            [&] { return scenarios::draft21_discovery_overlap_probes(run_config.timeout, name_space); }));
        if (range_filter) return execute(scenarios::draft21_range_filter_probes(
            run_config.timeout, name_space, bytes_of(run_config.track_fixture->track_name)));
        if (request_response) return execute(scenarios::draft21_request_response_probes(
            run_config.timeout, name_space, bytes_of(run_config.track_fixture->track_name)));
        if (fetch_response) return execute(scenarios::draft21_fetch_response_probes(
            run_config.timeout, name_space, bytes_of(run_config.track_fixture->track_name)));
        if (fetch) return execute(app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_fetch_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); },
            [&] { return scenarios::draft21_fetch_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); }));
        return execute(app::by_draft(run_config.draft,
            [&] { return scenarios::draft18_subscription_cancel_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); },
            [&] { return scenarios::draft21_subscription_cancel_probes(run_config.timeout, name_space,
                bytes_of(run_config.track_fixture->track_name)); }));
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
                // Draft 18 has no lineage (its execution draft is its wire draft), so the controller's id is
                // already the requested one.
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
                     const LineageRun& plan, PublisherDriver* driver,
                     DriverHandle handle, const std::function<void()>& record_driver) {
        const RunConfig& run_config = plan.execution;
        const bool lineage = runs_by_lineage(plan);
        const auto started = scenarios::Draft21Clock::now();
        // The controller starts its own observation clock at its first poll, a
        // little after `started`. This deadline is only a backstop: if it fired
        // first, a window-judged row would be abandoned before the controller
        // recorded that the window elapsed, and a pass would degrade to not_run.
        const auto deadline = started + run_config.timeout + std::chrono::milliseconds{250};
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
        // The evidence is stored under the requested scenario id.
        const std::string stored_id = stored_scenario_id(plan, run_config.scenario_ids.front());
        std::size_t recorded = 0;
        while (!worker->stop_requested) {
            const auto now = scenarios::Draft21Clock::now();
            const auto snapshot = controller.poll(now);
            const auto& evidence = controller.context().evidence;
            if (recorded < evidence.size()) {
                std::vector<storage::EvidenceEvent> batch;
                batch.reserve(evidence.size() - recorded);
                for (; recorded < evidence.size(); ++recorded) {
                    batch.push_back(stored_draft21_evidence(evidence[recorded], started, stored_id));
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
        auto outcomes = requirements::evaluate_draft21_announcement(
            *draft21, context);
        // An adapter refusal (the controller then closed the session with PROTOCOL_VIOLATION) is a harness
        // error of this path: run() records it and finalizes the run as Error.
        if (const auto refusal = scenarios::take_adapter_refusal())
            throw std::runtime_error(adapter_refusal_message(*refusal));
        if (lineage) {
            // A draft 22 run: the same evaluation, stored and scored as draft 22 rows.
            outcomes = lineage_outcomes(*draft22, outcomes);
            store->finalize(worker->id, requirements::score(*draft22, outcomes), outcomes);
            return;
        }
        const auto summary = requirements::score(*draft21, outcomes);
        store->finalize(worker->id, summary, outcomes);
    }

    // A moq-lite-06 run (app/lite_run.h): validated, given the first context's listener and stored here; the
    // worker runs every context with run_lite. Unsupported without the lite catalog (or a driver for a Driven run).
    RunStartResult start_lite(const RunConfig& requested) {
        if (!moqlite06 || (requested.mode == RunMode::Driven && config.driver_executable.empty()))
            return {RunStartStatus::Unsupported, {}, {}};
        if (const auto refusal = lite_start_refusal(requested)) return {*refusal, {}, {}};
        std::lock_guard lock(mutex);
        reap_finished();
        if (workers.size() >= config.maximum_active_runs) return {RunStartStatus::PortExhausted, {}, {}};
        const bool ephemeral = config.port_start == 0;
        const auto attempts = ephemeral ? config.maximum_active_runs + 1 :
            static_cast<std::size_t>(config.port_end - config.port_start) + 1;
        // A WebTransport lite session is CONNECTed at the fixed session target "/moq?token=l1d" (lite_run.h).
        const auto target = lite_session_target(requested.transport);
        const Tuning tuning{{}, {}, {}, false, target};
        ListenerResult created;
        for (std::size_t attempt = 0; attempt < attempts && !created.listener; ++attempt) {
            const auto port = ephemeral ? std::uint16_t{0} : static_cast<std::uint16_t>(config.port_start + attempt);
            if (port != 0 && reserved_ports.contains(port)) continue;
            created = create_listener(requested.transport, DraftVersion::MoqLite06, port, tuning);
            if (created.listener && reserved_ports.contains(created.endpoint.port)) {
                created = {};
            } else if (!created.listener && created.error != transport::NativeQuicListenerError::BindFailed) {
                std::fprintf(stderr, "publisher listener could not start on port %u%s\n", static_cast<unsigned>(port),
                             describe_listener_failure(created, port).c_str());
                return {RunStartStatus::ListenerError, {}, {}};
            }
        }
        if (!created.listener) return {RunStartStatus::PortExhausted, {}, {}};
        auto worker = std::make_unique<Worker>();
        worker->endpoint = created.endpoint;
        if (!config.advertised_address.empty()) worker->endpoint.address = config.advertised_address;
        worker->id = store->create_run(requested);
        auto* run = worker.get();
        RunStartResult result{RunStartStatus::Started, run->id, run->endpoint};
        if (requested.transport == TransportKind::WebTransport) {
            result.path = lite_session_url(requested.transport).path;
            result.protocol = std::string(app::alpn(DraftVersion::MoqLite06));
            result.url = lite_endpoint_uri(requested.transport, authority_of(run));
        }
        workers.push_back(std::move(worker));
        reserved_ports.insert(run->endpoint.port);
        try {
            run->thread = std::thread([this, run, requested, target, listener = std::move(created.listener)]() mutable {
                const auto port = run->endpoint.port;
                LiteRunEnvironment environment{run->id, authority_of(run), *store, *moqlite06, config,
                    run->stop_requested, [this, &requested, port, target]() -> LiteRunListener {
                        const Tuning tuning{{}, {}, {}, false, target};
                        auto replacement = create_listener(requested.transport, DraftVersion::MoqLite06, port, tuning);
                        if (replacement.listener && replacement.endpoint.port == port)
                            return {std::move(replacement.listener), {}};
                        return {nullptr, describe_listener_failure(replacement, port)};
                    }, {}};
                run_lite(environment, std::move(listener), requested);
                {
                    std::lock_guard released(mutex);
                    reserved_ports.erase(port);
                }
                run->finished = true;
            });
        } catch (...) {
            const auto id = run->id;
            reserved_ports.erase(run->endpoint.port);
            workers.pop_back();
            try {
                storage::EvidenceEvent reason;
                reason.kind = "harness_error";
                reason.detail = "the run's worker thread could not be started";
                store->append_events(id, std::span(&reason, 1));
            } catch (...) {}
            const requirements::ScoreSummary failure{requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
            store->finalize(id, failure, {});
            return {RunStartStatus::ListenerError, {}, {}};
        }
        return result;
    }

    std::shared_ptr<const requirements::RequirementCatalog> draft18;
    std::shared_ptr<const requirements::RequirementCatalog> draft21;
    std::shared_ptr<const requirements::RequirementCatalog> draft22;
    std::shared_ptr<const requirements::RequirementCatalog> moqlite06;
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
    NativeRunManagerConfig config,
    std::shared_ptr<const requirements::RequirementCatalog> draft22,
    std::shared_ptr<const requirements::RequirementCatalog> moqlite06)
    : impl_(std::make_unique<Impl>(std::move(draft18), std::move(draft21),
                                    std::move(store), std::move(config), std::move(draft22),
                                    std::move(moqlite06))) {}

NativeRunManager::~NativeRunManager() = default;

// `requested` is the run as asked for: its draft is the run's identity (ALPN, stored row, scoring
// catalog, API answers). `execution` is what the scenario layer runs: the same run for drafts 18 and 21, and for
// draft 22 the draft 21 family with each shared scenario's draft 21 implementation id.
RunStartResult NativeRunManager::start(const RunConfig& requested) {
    // moq-lite runs its own family (app/lite_run.h), only when this manager holds the lite catalog.
    if (requested.draft == DraftVersion::MoqLite06) return impl_->start_lite(requested);
    // Identity: whether this runner can run the requested draft (its catalogs).
    if (!supports(requested.draft) ||
        (requested.mode == RunMode::Driven && !supports_driven()))
        return {RunStartStatus::Unsupported, {}, {}};
    if (requested.scenario_ids.empty() || requested.scenario_ids.size() > 100)
        return {RunStartStatus::InvalidConfig, {}, {}};
    // The selection's ids are judged in selection order, the first defect deciding, the same way for every
    // draft: that is the per-id loop below (an empty or repeated id is InvalidConfig, an id the draft cannot
    // run is Unsupported, and an earlier id's other defects come first). plan_run cannot plan a draft 22 id
    // the draft cannot run (it would refuse the whole selection before the loop judged the ids ahead of it),
    // so only the ids before the first such id are planned: `refused` is that id's position, found here in
    // one ordered pass with the requested draft's registry (for draft 22, exactly plan_run's criterion), and
    // the loop answers for it after judging the ids ahead of it. For drafts 18 and 21, plan_run plans every
    // id it is given (the identity on the prefix), and the answer for the refused id is the one their loop
    // gave it, so their answers are unchanged.
    std::size_t refused = requested.scenario_ids.size();
    for (std::size_t index = 0; index < requested.scenario_ids.size(); ++index) {
        if (!executable_scenario(draft_number(requested.draft), requested.scenario_ids[index])) {
            refused = index;
            break;
        }
    }
    RunConfig planned = requested;
    planned.scenario_ids.resize(refused);
    // An implemented own draft 22 scenario bypasses lineage_run and is dispatched natively below.
    auto plan = plan_run(planned);
    if (!plan) return {RunStartStatus::Unsupported, {}, {}};
    const RunConfig& execution = plan->execution;
    // Every stored event and driver request names the requested scenario (stored_scenario_id): it must lead back
    // from each execution id to the id requested at the same position, or the run would store evidence under
    // a scenario it did not select. Checked here, before anything is stored, so the worker never meets it.
    if (execution.scenario_ids.size() != planned.scenario_ids.size())
        throw std::logic_error("run plan changed the number of selected scenarios");
    for (std::size_t index = 0; index < execution.scenario_ids.size(); ++index) {
        if (stored_scenario_id(*plan, execution.scenario_ids[index]) != requested.scenario_ids[index])
            throw std::logic_error("scenario " + execution.scenario_ids[index] + " is not stored as the requested " +
                                   requested.scenario_ids[index]);
    }
    // The selection's size was checked above (the planned ids may be fewer than the selected ones).
    if (execution.timeout < 2ms || execution.timeout > 3600000ms ||
        (execution.track_fixture && !valid_fixture(*execution.track_fixture)))
        return {RunStartStatus::InvalidConfig, {}, {}};
    std::vector<scenarios::RawProbeDefinition> definitions;
    // Scenarios the publisher's declaration rules out: never given a listener context or a
    // publisher process, only a context_skipped evidence event.
    // `skipped` holds the scenario layer's ids; the stored skip events and the API answer carry the
    // requested ones.
    std::vector<std::pair<std::string, std::string>> skipped;
    std::string first_skipped_requested;
    std::string first_skipped_capability;
    // Behavior: every `draft`/`execution.draft` test in this loop is the execution draft, checked against an
    // execution id (a shared draft 22 scenario is checked as its draft 21 implementation).
    const auto draft = static_cast<unsigned>(behavior_draft(*plan));
    std::set<std::string> selected;
    for (std::size_t index = 0; index < execution.scenario_ids.size(); ++index) {
        const auto& id = execution.scenario_ids[index];
        // Distinct planned requested ids are distinct execution ids (stored_scenario_id maps each back).
        if (id.empty() || !selected.insert(id).second)
            return {RunStartStatus::InvalidConfig, {}, {}};
        // Identity: own scenarios exist only on the draft 22 wire, and keep their draft 22 id in execution.
        if (identity_draft(*plan) == DraftVersion::Draft22 && own_scenario_22(id)) {
            // An own draft 22 scenario: a raw probe on the draft 22 wire, scored by own evaluators.
            // Follow-up for Tasks 9-10: this branch skips the family transport gates (webtransport_only /
            // native_only profiles, gap_*_only_scenario); a transport-specific own scenario needs a field in
            // OwnScenarioTraits22 and a check here.
            if (auto reason = scenario_skip_reason(22, id, execution.publisher_capabilities)) {
                if (skipped.empty()) {
                    first_skipped_requested = requested.scenario_ids[index];
                    first_skipped_capability = std::string(scenario_required_capability(22, id).value_or("unknown"));
                }
                skipped.emplace_back(id, std::move(*reason));
                continue;
            }
            if ((scenario_requires_track(22, id) || execution.mode == RunMode::Driven) &&
                !execution.track_fixture)
                return {RunStartStatus::InvalidConfig, {}, {}};
            try {
                auto definition = Impl::resolve_own_probe(execution, id);
                if (!definition || definition->id != id)
                    return {RunStartStatus::Unsupported, {}, {}};
                definitions.push_back(std::move(*definition));
            } catch (const std::invalid_argument&) {
                return {RunStartStatus::InvalidConfig, {}, {}};
            }
            continue;
        }
        // The selection's size, not the planned prefix's.
        if (!executable_scenario(draft, id) ||
            (requested.scenario_ids.size() > 1 && !raw_probe_scenario(draft, id)))
            return {RunStartStatus::Unsupported, {}, {}};
        if (auto reason = scenario_skip_reason(draft, id, execution.publisher_capabilities)) {
            if (skipped.empty()) {
                // Identity: the API answer names the requested scenario and asks the identity draft's registry
                // (which forwards a shared draft 22 id to its implementation, so the capability is the same).
                first_skipped_requested = requested.scenario_ids[index];
                first_skipped_capability = std::string(scenario_required_capability(
                    draft_number(identity_draft(*plan)), first_skipped_requested).value_or("unknown"));
            }
            skipped.emplace_back(id, std::move(*reason));
            continue;
        }
        if ((scenario_requires_track(draft, id) || execution.mode == RunMode::Driven) &&
            !execution.track_fixture)
            return {RunStartStatus::InvalidConfig, {}, {}};
        if (execution.draft == DraftVersion::Draft18 && id == kDuplicateSubscribeScenario &&
            execution.timeout < 3ms)
            return {RunStartStatus::InvalidConfig, {}, {}};
        // Draft-21 gap slice A: transport-specific announcement scenarios.
        if (draft == 21 && gap_webtransport_only_scenario(id) &&
            execution.transport != TransportKind::WebTransport)
            return {RunStartStatus::Unsupported, {}, {}};
        if (draft == 21 && announcement_gap_scenario(21, id) && execution.track_fixture &&
            !gap_fixture_valid(id, execution.track_fixture->namespace_fields))
            return {RunStartStatus::InvalidConfig, {}, {}};
        if (gap_raw_scenario(draft, id) && execution.track_fixture) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : execution.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::fetch_first_object_fixture_valid(fields, bytes_of(execution.track_fixture->track_name)))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (draft == 21 && gap_native_only_scenario(id) &&
            execution.transport != TransportKind::NativeQuic)
            return {RunStartStatus::Unsupported, {}, {}};
        if (immutable_repeat_scenario(draft, id) || object_repeat_scenario(draft, id) ||
            fetch_first_object_scenario(draft, id) || fetch_group_order_scenario(draft, id) ||
            (execution.draft == DraftVersion::Draft21 && id == "d21-publish-state-notify-on-fetch") ||
            subscriber_notify_scenario(draft, id) || established_update_scenario(draft, id)) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : execution.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::fetch_first_object_fixture_valid(fields, bytes_of(execution.track_fixture->track_name)))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (execution.draft == DraftVersion::Draft18 && scenarios::draft18_contribution_scenario(id) &&
            execution.track_fixture) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : execution.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::draft18_contribution_fixture_valid(fields, bytes_of(execution.track_fixture->track_name)))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (discovery_overlap_scenario(draft, id)) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : execution.track_fixture->namespace_fields) fields.push_back(bytes_of(field));
            if (!scenarios::discovery_overlap_namespace_valid(fields))
                return {RunStartStatus::InvalidConfig, {}, {}};
        }
        if (execution.draft == DraftVersion::Draft18 && scenarios::draft18_gap_a_native_only(id) &&
            execution.transport != TransportKind::NativeQuic)
            return {RunStartStatus::Unsupported, {}, {}};
        if (execution.draft == DraftVersion::Draft18) {
            const auto profiles = scenarios::draft18_close_profiles();
            const auto found = std::find_if(profiles.begin(), profiles.end(), [&id](const auto& profile) {
                return profile.scenario_id == id;
            });
            if (found != profiles.end() &&
                ((found->webtransport_only && execution.transport != TransportKind::WebTransport) ||
                 (found->native_only && execution.transport != TransportKind::NativeQuic)))
                return {RunStartStatus::Unsupported, {}, {}};
        }
        if (raw_probe_scenario(draft, id)) {
            try {
                // start() runs on the caller's thread, outside the worker's ScopedWireDraft: build the
                // writes for the wire draft the peer speaks.
                auto definition = resolve_probe(impl_->config, execution, id, identity_draft(*plan));
                if (!definition || definition->id != id)
                    return {RunStartStatus::Unsupported, {}, {}};
                definitions.push_back(std::move(*definition));
            } catch (const std::invalid_argument&) {
                return {RunStartStatus::InvalidConfig, {}, {}};
            }
        }
    }
    if (refused != requested.scenario_ids.size()) {
        // Every id ahead of it passed: the refused id is judged as the loop judges an id the draft cannot run
        // (an empty id is InvalidConfig, anything else Unsupported). It cannot repeat an id ahead of it: those
        // are all executable and it is not, so the loop's repeat check has nothing to find here.
        if (requested.scenario_ids[refused].empty()) return {RunStartStatus::InvalidConfig, {}, {}};
        return {RunStartStatus::Unsupported, {}, {}};
    }
    if (skipped.size() == execution.scenario_ids.size()) {
        RunStartResult rejected{RunStartStatus::ScenarioRequiresCapability, {}, {}};
        rejected.scenario = first_skipped_requested;
        rejected.capability = first_skipped_capability;
        return rejected;
    }
    const bool needs_replacement = std::any_of(definitions.begin(), definitions.end(),
        [](const auto& definition) { return definition.offer_replacement_session; });
    std::lock_guard lock(impl_->mutex);
    impl_->reap_finished();
    // Capacity counts runs, not ports: a replacement run holds two ports.
    if (impl_->workers.size() >= impl_->config.maximum_active_runs) {
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
        // Identity: the listener offers the identity draft's ALPN (requested.draft == identity_draft(*plan)).
        auto created = impl_->create_listener(requested.transport, identity_draft(*plan), port,
            definitions.empty() ? Impl::Tuning{} : Impl::tuning_of(definitions.front()));
        if (created.listener) {
            if (impl_->reserved_ports.contains(created.endpoint.port)) continue;
            endpoint = created.endpoint;
            if (!impl_->config.advertised_address.empty()) endpoint.address = impl_->config.advertised_address;
            listener = std::move(created.listener);
            if (requested.transport == TransportKind::WebTransport) {
                path = "/moq";
                protocol = std::string(app::alpn(identity_draft(*plan)));
                const auto host = endpoint.address.find(':') != std::string::npos
                    ? "[" + endpoint.address + "]" : endpoint.address;
                url = "https://" + host + ":" + std::to_string(endpoint.port) + path;
            }
            break;
        }
        if (created.error != transport::NativeQuicListenerError::BindFailed) {
            // The API reports only that the listener could not start; the operator needs the reason.
            std::fprintf(stderr, "publisher listener could not start on port %u%s\n",
                         static_cast<unsigned>(port),
                         Impl::describe_listener_failure(created, port).c_str());
            return {RunStartStatus::ListenerError, {}, {}};
        }
    }
    if (!listener) return {RunStartStatus::PortExhausted, {}, {}};

    // A replacement session needs a second port beside the run's own; claim
    // it now so an accepted run can never fail for want of one.
    std::optional<std::uint16_t> replacement_port;
    if (needs_replacement && !ephemeral) {
        for (std::uint32_t candidate = impl_->config.port_start; candidate <= impl_->config.port_end; ++candidate) {
            if (candidate == endpoint.port || impl_->reserved_ports.contains(static_cast<std::uint16_t>(candidate))) continue;
            replacement_port = static_cast<std::uint16_t>(candidate);
            break;
        }
        if (!replacement_port) return {RunStartStatus::PortExhausted, {}, {}};
    }

    const auto id = impl_->store->create_run(requested);
    {
        // Make the run self-describing: what the publisher declared, and every scenario
        // that was therefore never started.
        std::vector<storage::EvidenceEvent> declaration;
        const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        storage::EvidenceEvent capabilities;
        capabilities.wall_time_unix_ns = wall;
        capabilities.kind = "publisher_capabilities";
        capabilities.detail = std::string("fetch=") + (requested.publisher_capabilities.fetch ? "true" : "false");
        declaration.push_back(std::move(capabilities));
        for (const auto& [skipped_id, reason] : skipped) {
            storage::EvidenceEvent event;
            event.wall_time_unix_ns = wall;
            event.kind = "context_skipped";
            event.detail = reason;
            event.scenario_id = stored_scenario_id(*plan, skipped_id);
            declaration.push_back(std::move(event));
        }
        try {
            impl_->store->append_events(id, declaration);
        } catch (...) {
            const requirements::ScoreSummary failure{
                requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
            impl_->store->finalize(id, failure, {});
            return {RunStartStatus::ListenerError, {}, {}};
        }
    }
    auto worker = std::make_unique<Impl::Worker>();
    worker->id = id;
    worker->endpoint = endpoint;
    worker->replacement_port = replacement_port;
    auto* worker_ptr = worker.get();
    impl_->workers.push_back(std::move(worker));
    impl_->reserved_ports.insert(endpoint.port);
    if (replacement_port) impl_->reserved_ports.insert(*replacement_port);
    try {
        worker_ptr->thread = std::thread(
            [this, worker_ptr, listener = std::move(listener), plan = std::move(*plan),
             definitions = std::move(definitions)] () mutable {
                impl_->run(worker_ptr, std::move(listener), std::move(plan), std::move(definitions));
            });
    } catch (...) {
        impl_->workers.pop_back();
        impl_->reserved_ports.erase(endpoint.port);
        if (replacement_port) impl_->reserved_ports.erase(*replacement_port);
        try {
            storage::EvidenceEvent reason;
            reason.kind = "harness_error";
            reason.detail = "the run's worker thread could not be started";
            impl_->store->append_events(id, std::span(&reason, 1));
        } catch (...) {}
        const requirements::ScoreSummary failure{
            requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
        impl_->store->finalize(id, failure, {});
        return {RunStartStatus::ListenerError, {}, {}};
    }
    return {RunStartStatus::Started, id, endpoint, url, path, protocol};
}

std::optional<scenarios::RawProbeDefinition> NativeRunManager::resolve_probe(
    const NativeRunManagerConfig& manager_config, const RunConfig& run_config, std::string_view id) {
    return Impl::resolve_raw_probe(manager_config, run_config, id);
}

std::optional<scenarios::RawProbeDefinition> NativeRunManager::resolve_probe(
    const NativeRunManagerConfig& manager_config, const RunConfig& run_config, std::string_view id,
    DraftVersion wire_draft) {
    const scenarios::ScopedWireDraft wire(draft_number(wire_draft));
    return Impl::resolve_raw_probe(manager_config, run_config, id);
}

bool NativeRunManager::supports(DraftVersion draft) const noexcept {
    switch (draft) {
        case DraftVersion::Draft18: return true;
        case DraftVersion::Draft21: return impl_->draft21 != nullptr;
        // Draft 22 runs its shared scenarios on draft 21's family (lineage).
        case DraftVersion::Draft22: return impl_->draft22 != nullptr && impl_->draft21 != nullptr;
        case DraftVersion::MoqLite06: return impl_->moqlite06 != nullptr;
    }
    return false;
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
