#include "moq/interop/app/lite_run.h"

#include "moq/interop/app/lite_scenarios.h"
#include "moq/interop/app/publisher_driver.h"
#include "moq/interop/requirements/lite_evaluators.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_errors.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/session/lite_session.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace moq::interop::app {
namespace {

using namespace std::chrono_literals;
namespace l06 = wire::moqlite06;

constexpr std::size_t kMaximumSelection = 100;
// Per context, the decoded publisher messages and decode issues stored as evidence (each kind); the context end
// event says how many were left out. The transcript keeps every one for the evaluators.
constexpr std::size_t kMaximumStoredMessages = 4096;
constexpr std::size_t kMaximumStoredIssues = 1024;
// The provisional WebTransport session path (plan decision (e)).
constexpr std::string_view kWebTransportPath = "/moq";

std::string hex(std::span<const std::byte> bytes) {
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

std::string text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

const char* yes(bool value) { return value ? "true" : "false"; }

std::string broadcast_path_of(const TrackFixture& fixture) {
    std::string path;
    for (const auto& field : fixture.namespace_fields) {
        if (!path.empty()) path += '/';
        path += field;
    }
    return path;
}

// A track fixture usable as a lite broadcast path and track: 1..32 non-empty fields, a non-empty track name, at
// most 4096 bytes in all (the bounds the MoQ Transport runs apply).
bool usable_fixture(const TrackFixture& fixture) {
    if (fixture.namespace_fields.empty() || fixture.namespace_fields.size() > 32 || fixture.track_name.empty())
        return false;
    std::size_t total = fixture.track_name.size();
    for (const auto& field : fixture.namespace_fields) {
        if (field.empty()) return false;
        total += field.size();
    }
    return total <= 4096;
}

const char* binding_name(scenarios::LiteBinding binding) {
    switch (binding) {
        case scenarios::LiteBinding::NativeQuic: return "native_quic";
        case scenarios::LiteBinding::WebTransport: return "webtransport";
        case scenarios::LiteBinding::Unknown: return "unknown";
    }
    return "unknown";
}

scenarios::LiteBinding binding_of(TransportKind transport) {
    return transport == TransportKind::WebTransport ? scenarios::LiteBinding::WebTransport
                                                    : scenarios::LiteBinding::NativeQuic;
}

scenarios::LiteProbeDefinition build(std::string_view id, std::chrono::milliseconds deadline,
                                     std::string_view path, std::string_view track, const LiteSessionUrl& url) {
    namespace s = scenarios;
    if (id == "l06-setup-stream") return s::l06_setup_stream_probe(deadline);
    if (id == "l06-setup-unknown-parameter") return s::l06_setup_unknown_parameter_probe(deadline);
    if (id == "l06-setup-duplicate-parameter") return s::l06_setup_duplicate_parameter_probe(deadline);
    if (id == "l06-setup-duplicate-stream") return s::l06_setup_duplicate_stream_probe(deadline);
    if (id == "l06-setup-server-path") return s::l06_setup_server_path_probe(deadline);
    if (id == "l06-setup-server-role") return s::l06_setup_server_role_probe(deadline);
    if (id == "l06-setup-client-path") return s::l06_setup_client_path_probe(deadline, url.path, url.query);
    if (id == "l06-announce-prefix") return s::l06_announce_prefix_probe(deadline, path);
    if (id == "l06-announce-lifecycle") return s::l06_announce_lifecycle_probe(deadline, path);
    if (id == "l06-session-stream-close") return s::l06_session_stream_close_probe(deadline, path, track);
    if (id == "l06-subscribe-latest") return s::l06_subscribe_latest_probe(deadline, path, track);
    if (id == "l06-subscribe-refused") return s::l06_subscribe_refused_probe(deadline, path, track);
    if (id == "l06-subscribe-invalid-frame-bounds")
        return s::l06_subscribe_invalid_frame_bounds_probe(deadline, path, track);
    if (id == "l06-subscribe-group-floor") return s::l06_subscribe_group_floor_probe(deadline, path, track);
    if (id == "l06-subscribe-abutting-frame-start")
        return s::l06_subscribe_abutting_frame_start_probe(deadline, path, track);
    if (id == "l06-errors-unknown-stream-type") return s::l06_errors_unknown_stream_type_probe(deadline);
    if (id == "l06-errors-unknown-reset-code") return s::l06_errors_unknown_reset_code_probe(deadline, path, track);
    if (id == "l06-errors-reserved-reset-code") return s::l06_errors_reserved_reset_code_probe(deadline, path, track);
    if (id == "l06-errors-code-space") return s::l06_errors_code_space_probe(deadline);
    throw std::invalid_argument("no moq-lite-06 probe builder for scenario " + std::string(id));
}

// The decoded fields of a publisher message worth keeping beside its name.
std::string describe(const session::LiteMessage& message) {
    return std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, l06::SetupMessage>) {
            std::string result = "parameters=";
            for (std::size_t i = 0; i < value.parameters.size(); ++i) {
                if (i != 0) result += ',';
                result += std::to_string(value.parameters[i].id) + ":" + hex(value.parameters[i].value);
            }
            return result;
        } else if constexpr (std::is_same_v<T, l06::GroupHeader>) {
            return "subscribe_id=" + std::to_string(value.subscribe_id) + " group_sequence=" +
                   std::to_string(value.group_sequence) + " frame_start=" + std::to_string(value.frame_start);
        } else if constexpr (std::is_same_v<T, l06::Frame>) {
            return "timestamp_delta=" + std::to_string(value.timestamp_delta) +
                   " payload_bytes=" + std::to_string(value.payload.size());
        } else if constexpr (std::is_same_v<T, l06::AnnounceOk>) {
            return "hop_id=" + std::to_string(value.hop_id) + " active_count=" + std::to_string(value.active_count);
        } else if constexpr (std::is_same_v<T, l06::AnnounceStart>) {
            std::string hops;
            for (const auto hop : value.route.hop_ids) hops += (hops.empty() ? "" : ",") + std::to_string(hop);
            return "suffix=" + hex(std::as_bytes(std::span(value.suffix))) + " hop_ids=" + hops;
        } else if constexpr (std::is_same_v<T, l06::AnnounceEnd> || std::is_same_v<T, l06::AnnounceUpdate>) {
            return "announce_id=" + std::to_string(value.announce_id);
        } else if constexpr (std::is_same_v<T, l06::SubscribeOk> || std::is_same_v<T, l06::SubscribeEnd>) {
            return "group=" + std::to_string(value.group);
        } else if constexpr (std::is_same_v<T, l06::SubscribeDrop>) {
            return "group_start=" + std::to_string(value.group_start) + " group_end=" +
                   std::to_string(value.group_end) + " error_code=" + std::to_string(value.error_code);
        } else {
            return {};
        }
    }, message);
}

bool driver_failed(const DriverResult& result) {
    return result.status == DriverStatus::Error || result.status == DriverStatus::TimedOut ||
           result.status == DriverStatus::Signaled ||
           (result.status == DriverStatus::Stopped && result.term_signal == SIGKILL) ||
           (result.status == DriverStatus::Exited && result.exit_code.value_or(1) != 0);
}

// One run's worker state and its evidence writers.
class LiteRun {
public:
    LiteRun(LiteRunEnvironment& environment, const RunConfig& config)
        : env_(environment), config_(config), url_(lite_session_url(config.transport)) {}

    void run(std::unique_ptr<transport::SessionTransport> listener) {
        bool listener_used = false;
        try {
            const auto& ids = config_.scenario_ids;
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (env_.stop_requested) break;
                current_ = ids[index];
                ordinal_ = index + 1;
                connection_id_.clear();
                std::optional<scenarios::LiteProbeDefinition> definition;
                try {
                    definition = lite_probe_for(config_, current_);
                } catch (const std::invalid_argument& error) {
                    // A builder that cannot fit its windows in the timeout: this context stores why and runs nothing.
                    operational_error_ = true;
                    context_event("harness_error", "the probe could not be built: " + std::string(error.what()));
                    context_event("context_skipped", "probe definition refused for timeout_ms=" +
                                                         std::to_string(config_.timeout.count()));
                    continue;
                }
                if (listener_used || !listener) {
                    close_listener(listener);
                    if (env_.stop_requested) break;
                    auto created = env_.recreate_listener();
                    if (!created.listener)
                        throw std::runtime_error("lite context listener could not rebind the run's port" +
                                                 created.failure);
                    listener = std::move(created.listener);
                }
                listener_used = true;
                context_event("context_ready", ready_detail(*definition));
                if (env_.stop_requested) break;
                const bool connected = run_context(*listener, std::move(*definition));
                if (env_.stop_requested || !connected) {
                    // Nothing after a publisher that never connected (or a stop) can run: name what did not.
                    for (std::size_t later = index + 1; later < ids.size(); ++later) {
                        current_ = ids[later];
                        context_event("context_skipped", env_.stop_requested
                            ? "the run was stopped before this context"
                            : "not run: the publisher did not connect in context " + std::to_string(index + 1));
                    }
                    current_ = ids[index];
                    break;
                }
            }
        } catch (const std::exception& error) {
            operational_error_ = true;
            try {
                retire_driver();
                context_event("harness_error", error.what());
            } catch (const std::exception& cleanup) {
                std::cerr << "lite run cleanup failed: " << cleanup.what() << '\n';
            }
        }
        close_listener(listener);
        finalize();
    }

private:
    void stamp(storage::EvidenceEvent& event, std::optional<std::uint64_t> at_ns = std::nullopt) const {
        const auto now = clock_.now_ns();
        const auto when = at_ns.value_or(now);
        event.monotonic_time_ns = static_cast<std::int64_t>(when - std::min(when, started_ns_));
        event.wall_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() -
            static_cast<std::int64_t>(now - std::min(now, when));
        if (!connection_id_.empty()) event.connection_id = connection_id_;
        if (!current_.empty()) event.scenario_id = current_;
    }

    void context_event(std::string kind, std::string detail) {
        storage::EvidenceEvent event;
        event.kind = std::move(kind);
        event.detail = std::move(detail);
        stamp(event);
        if (event.kind != "publisher_process") event.detail += " ordinal=" + std::to_string(ordinal_);
        env_.store.append_events(env_.id, std::span(&event, 1));
    }

    std::string ready_detail(const scenarios::LiteProbeDefinition& definition) const {
        return "endpoint=" + lite_endpoint_uri(config_.transport, env_.authority) +
               " binding=" + binding_name(definition.binding) +
               " session_url_has_path=" + yes(definition.session_url_has_path) +
               " session_url_path=" + definition.session_url_path +
               " session_url_query=" + definition.session_url_query +
               " broadcast_path=" + definition.broadcast_path + " track_name=" + definition.track_name +
               " connect_deadline_ms=" + std::to_string(definition.connect_deadline.value_or(definition.deadline).count()) +
               " deadline_ms=" + std::to_string(definition.deadline.count()) +
               " reconnect=fresh-session publisher_identity=unverified";
    }

    void close_listener(std::unique_ptr<transport::SessionTransport>& listener) {
        if (!listener) return;
        // Cleanup events belong to no context's proof.
        (void)listener->close(0, {});
        const auto until = std::chrono::steady_clock::now() + 20ms;
        do {
            (void)listener->poll(256);
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < until);
        listener.reset();
    }

    void start_driver() {
        DriverRequest request;
        request.executable = env_.config.driver_executable;
        request.arguments = env_.config.driver_arguments;
        request.run_id = env_.id;
        request.scenario_id = current_;
        request.endpoint = lite_endpoint_uri(config_.transport, env_.authority);
        request.draft = DraftVersion::MoqLite06;
        request.transport = config_.transport;
        request.track = *config_.track_fixture;
        request.fixture = env_.config.driver_fixture;
        request.tls_ca = env_.config.driver_tls_ca.empty() ? env_.config.certificate_path : env_.config.driver_tls_ca;
        request.log_dir = env_.config.driver_log_root / env_.id / (std::to_string(ordinal_) + "-" + current_);
        request.scenario_timeout = config_.timeout;
        // The context waits up to the timeout for the connection, then up to the timeout for the probe.
        request.process_timeout = 2 * config_.timeout + 1000ms;
        const auto started = driver_.start(request);
        if (started.status == DriverStartStatus::Started) {
            handle_ = started.handle;
            return;
        }
        DriverResult failure;
        failure.error = started.error;
        publisher_process(failure);
        throw std::runtime_error("publisher driver start failed: " + started.error);
    }

    void publisher_process(const DriverResult& result) {
        context_event("publisher_process", serialize_driver_result(result));
    }

    // The driver's final result (stored); true when it failed.
    bool retire_driver() {
        if (!handle_.valid()) return false;
        const auto result = driver_.stop(handle_);
        handle_ = {};
        publisher_process(result);
        return driver_failed(result);
    }

    // Runs one context; false when the publisher never established a moq-lite-06 session.
    bool run_context(transport::SessionTransport& listener, scenarios::LiteProbeDefinition definition) {
        if (config_.mode == RunMode::Driven) start_driver();
        const auto deadline_ms = definition.deadline;
        scenarios::LiteProbeController controller(std::move(definition), listener, clock_);
        std::string operational;
        while (!env_.stop_requested) {
            if (!controller.poll()) break;
            if (handle_.valid()) {
                const auto process = driver_.poll(handle_);
                // A publisher process that ended before it connected cannot connect any more.
                if (process.status != DriverStatus::Running && !controller.transcript().established) {
                    operational = "the publisher process ended before it established a session";
                    break;
                }
            }
            std::this_thread::sleep_for(1ms);
        }
        // Read on this thread only (LiteProbeController::transcript is not thread-safe).
        auto transcript = controller.transcript();
        const bool stopped = env_.stop_requested.load();
        if (stopped && !operational.empty()) operational.clear();
        if (stopped && controller.running()) {
            // Interrupted: never judged (evaluate_lite drops a harness_failed transcript).
            transcript.complete = false;
            transcript.harness_failed = true;
            transcript.harness_failure_reason = "the run was stopped during this context";
        }
        const bool process_failed = retire_driver();
        if (!operational.empty()) {
            transcript.complete = false;
            transcript.harness_failed = true;
            transcript.harness_failure_reason = operational;
        } else if (process_failed && !transcript.complete && !transcript.harness_failed && !stopped) {
            transcript.harness_failed = true;
            transcript.harness_failure_reason = "publisher process failed";
        }
        store_evidence(transcript);
        const bool connected = transcript.established || stopped;
        if (transcript.harness_failed && !stopped) {
            operational_error_ = true;
            context_event("harness_error", transcript.harness_failure_reason.empty()
                ? "harness failure without a recorded reason" : transcript.harness_failure_reason);
        } else if (!transcript.established && !stopped) {
            operational_error_ = true;
            context_event("harness_error", "the publisher did not establish a moq-lite-06 session within " +
                                               std::to_string(config_.timeout.count()) +
                                               " ms (no connection, or a connection refused by the listener: "
                                               "for example another ALPN or WebTransport protocol)");
        }
        if (transcript.event_limit_reached) context_event("context_event_limit", transcript.event_limit_reason);
        const bool clean = transcript.complete && !transcript.harness_failed && !transcript.timed_out && transcript.established;
        context_event(clean ? "context_complete" : "context_end", end_detail(transcript, deadline_ms, stopped));
        // Judged now; only the verdicts stay until finalize (the transcript ends with this context).
        verdicts_.push_back(requirements::judge_lite_context(env_.catalog, transcript));
        retained_bytes_ += requirements::lite_retained_bytes(verdicts_.back());
        if (env_.on_context_judged) env_.on_context_judged(verdicts_.size(), retained_bytes_);
        return connected;
    }

    std::string ms_since(std::uint64_t base, std::optional<std::uint64_t> at) const {
        if (!at) return "none";
        return std::to_string((*at - std::min(*at, base)) / 1000000);
    }

    std::string end_detail(const scenarios::LiteTranscript& t, std::chrono::milliseconds deadline, bool stopped) const {
        const auto base = t.established ? t.established_ns : t.started_ns;
        std::string detail = "complete=" + std::string(yes(t.complete)) + " timed_out=" + yes(t.timed_out) +
            " event_limit=" + yes(t.event_limit_reached) + " harness_failed=" + yes(t.harness_failed) +
            " established=" + yes(t.established) + " stimulus_delivered=" + yes(t.stimulus_delivered) +
            " peer_closed_early=" + yes(t.peer_closed_early) + " runner_closed=" + yes(t.runner_closed) +
            " cancelled=" + yes(stopped) +
            // Every lite probe is time-bounded: its allowances end it, the deadline bounds it.
            " time_bounded=true deadline_ms=" + std::to_string(deadline.count()) +
            " elapsed_ms=" + ms_since(base, t.ended_ns) +
            " lite_messages_stored=" + std::to_string(stored_messages_) + "/" + std::to_string(total_messages_) +
            // Group payload bytes the transcript kept as length and FIN only (the recorder decoded them).
            " group_payload_bytes_dropped=" + std::to_string(t.payload_bytes_dropped) +
            " lite_decode_errors_stored=" + std::to_string(stored_issues_) + "/" + std::to_string(total_issues_) +
            " steps=";
        for (std::size_t i = 0; i < t.steps.size(); ++i) {
            const auto& step = t.steps[i];
            if (i != 0) detail += ';';
            detail += std::to_string(step.index) + ":" + step.label + ":" + std::string(scenarios::to_string(step.kind)) +
                      (step.dynamic ? ":dynamic" : "") + ":current_ms=" + ms_since(base, step.current_at_ns) +
                      ":executed_ms=" + ms_since(base, step.executed_at_ns) +
                      ":gate_expired=" + yes(step.gate_expired);
            if (!step.skipped_reason.empty()) detail += ":skipped=" + step.skipped_reason;
            if (step.refused) detail += ":refused=" + std::to_string(static_cast<unsigned>(*step.refused));
        }
        return detail;
    }

    void stimulus(const scenarios::LiteStepRecord& step, std::uint64_t base, std::vector<storage::EvidenceEvent>& out) {
        using Kind = scenarios::LiteStep::Kind;
        const bool acts = step.kind == Kind::ResetStream || step.kind == Kind::StopSending ||
                          step.kind == Kind::CloseSession;
        if (!step.executed() || !(step.accepted > 0 || step.fin_accepted || acts)) return;
        storage::EvidenceEvent event;
        event.kind = step.delivered() ? "raw_probe_stimulus" : "raw_probe_partial_stimulus";
        if (step.stream_id) event.stream_id = std::to_string(*step.stream_id);
        event.detail = "label=" + step.label + " step_kind=" + std::string(scenarios::to_string(step.kind)) +
            " step_index=" + std::to_string(step.index) + " dynamic=" + yes(step.dynamic) +
            " executed_ms=" + ms_since(base, step.executed_at_ns) + " gate_expired=" + yes(step.gate_expired) +
            " accepted=" + std::to_string(step.accepted) + " fin=" + yes(step.fin_accepted) +
            " code=" + std::to_string(step.code) +
            (step.refused ? " refused=" + std::to_string(static_cast<unsigned>(*step.refused)) : std::string{}) +
            " bytes=" + hex(step.bytes);
        // The SUBSCRIBE a Subscribe-stream stimulus carries (its Position, row 020's evidence).
        if (const auto subscribe = scenarios::l06_decode_subscribe_stimulus(step.bytes)) {
            const auto& range = subscribe->range;
            event.detail += " subscribe_id=" + std::to_string(subscribe->subscribe_id) +
                " group_start=" + std::to_string(range.group_start) + " frame_start=" +
                std::to_string(range.frame_start) + " group_end=" + std::to_string(range.group_end) +
                " frame_end=" + std::to_string(range.frame_end) + " max_age_ms=" +
                std::to_string(range.subscriber_max_age_ms);
        }
        event.detail += " ordinal=" + std::to_string(ordinal_);
        stamp(event, step.executed_at_ns);
        out.push_back(std::move(event));
    }

    // The evidence kinds the lite bindings declare (lite_evaluators.h), from the transcript, in one batch.
    void store_evidence(const scenarios::LiteTranscript& t) {
        std::vector<storage::EvidenceEvent> batch;
        const auto base = t.established ? t.established_ns : t.started_ns;
        const auto suffix = " ordinal=" + std::to_string(ordinal_);
        for (std::size_t index = 0; index < t.events.size(); ++index) {
            const auto& source = t.events[index];
            const auto at = index < t.event_times.size() ? std::optional{t.event_times[index]} : std::nullopt;
            storage::EvidenceEvent event;
            const auto position = " transport_event_index=" + std::to_string(index) + " at_ms=" + ms_since(base, at);
            if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&source)) {
                connection_id_ = hex(established->local_connection_id);
                event.kind = "transport_established";
                event.detail = "local_connection_id=" + connection_id_ + " peer_connection_id=" +
                    hex(established->peer_connection_id) + " alpn=" + text(established->alpn) +
                    " max_datagram_payload=" + std::to_string(established->max_datagram_payload);
            } else if (const auto* data = std::get_if<transport::StreamDataEvent>(&source)) {
                // Stream bytes are stored as decoded messages (lite_message); only a FIN is a stream ending.
                if (!data->fin) continue;
                event.kind = "raw_probe_transport_event";
                event.stream_id = std::to_string(data->stream_id);
                // The bytes the event carried as received (a Group stream's are not kept in the transcript).
                const auto size = index < t.event_data_sizes.size() ? t.event_data_sizes[index] : data->data.size();
                event.detail = "operation=peer-fin bytes=" + std::to_string(size);
            } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&source)) {
                event.kind = "raw_probe_transport_event";
                event.stream_id = std::to_string(reset->stream_id);
                event.detail = "operation=peer-reset application_error=" +
                    (reset->application_error ? std::to_string(*reset->application_error) : "unavailable");
            } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&source)) {
                event.kind = "raw_probe_transport_event";
                event.stream_id = std::to_string(stop->stream_id);
                event.detail = "operation=peer-stop-sending application_error=" +
                    (stop->application_error ? std::to_string(*stop->application_error) : "unavailable");
            } else if (const auto* close = std::get_if<transport::PeerCloseEvent>(&source)) {
                event.kind = "peer_close";
                event.detail = std::string(close->error_space == transport::CloseErrorSpace::Application
                                               ? "application" : "transport") +
                    " close code=" + std::to_string(close->error_code) + " reason=" + hex(close->reason);
            } else if (const auto* local = std::get_if<transport::LocalCloseEvent>(&source)) {
                event.kind = "local_close";
                event.detail = std::string(local->error_space == transport::CloseErrorSpace::Application
                                               ? "application" : "transport") +
                    " close code=" + std::to_string(local->error_code) + " reason=" + hex(local->reason);
            } else if (std::holds_alternative<transport::EventQueueOverflowEvent>(source) ||
                       std::holds_alternative<transport::TransportErrorEvent>(source)) {
                event.kind = "harness_limit";
                event.detail = "lite probe transport evidence unavailable";
            } else {
                continue;
            }
            event.detail += position + suffix;
            stamp(event, at);
            batch.push_back(std::move(event));
        }
        stimulus(t.runner_setup, base, batch);
        for (const auto& step : t.steps) stimulus(step, base, batch);
        for (const auto* record : scenarios::lite06::peer_streams(t)) {
            storage::EvidenceEvent event;
            event.kind = std::string(session::kLiteStreamOpenedKind);
            event.stream_id = std::to_string(record->stream_id);
            event.detail = "stream_kind=" + std::string(session::to_string(record->kind)) + " stream_type=" +
                (record->stream_type ? std::to_string(*record->stream_type) : "none") +
                " bidirectional=" + yes(record->bidirectional) + " opened_ms=" + ms_since(base, record->opened_ns) +
                " bytes=" + std::to_string(record->bytes) + " fin=" + yes(record->fin_seen) +
                " reset_code=" + (record->reset_code ? std::to_string(*record->reset_code) : "none") +
                " stop_sending_code=" + (record->stop_sending_code ? std::to_string(*record->stop_sending_code) : "none") +
                suffix;
            stamp(event, record->opened_ns);
            batch.push_back(std::move(event));
        }
        stored_messages_ = total_messages_ = stored_issues_ = total_issues_ = 0;
        for (const auto& record : t.streams) {
            for (const auto* message : session::peer_messages(record)) {
                ++total_messages_;
                if (stored_messages_ >= kMaximumStoredMessages) continue;
                ++stored_messages_;
                storage::EvidenceEvent event;
                event.kind = std::string(session::kLiteMessageKind);
                event.stream_id = std::to_string(record.stream_id);
                event.detail = "message=" + std::string(session::lite_message_name(message->message)) + " " +
                    describe(message->message) + " at_ms=" + ms_since(base, message->at_ns) +
                    " stream_event_index=" + std::to_string(message->stream_event_index) + suffix;
                stamp(event, message->at_ns);
                batch.push_back(std::move(event));
            }
            for (const auto* issue : session::peer_issues(record)) {
                ++total_issues_;
                if (stored_issues_ >= kMaximumStoredIssues) continue;
                ++stored_issues_;
                storage::EvidenceEvent event;
                event.kind = std::string(session::kLiteDecodeErrorKind);
                event.stream_id = std::to_string(record.stream_id);
                event.detail = "code=" + issue->code + " detail=" + issue->detail +
                    " stream_event_index=" + std::to_string(issue->stream_event_index) + suffix;
                stamp(event);
                batch.push_back(std::move(event));
            }
        }
        if (!batch.empty()) env_.store.append_events(env_.id, batch);
    }

    void finalize() {
        try {
            auto outcomes = requirements::aggregate_lite(env_.catalog, verdicts_);
            auto summary = requirements::score_staged(env_.catalog, outcomes);
            if (operational_error_ || env_.stop_requested) summary.verdict = requirements::RunVerdict::Error;
            if (env_.stop_requested)
                context_event("run_stopped", "the run was stopped before every selected context finished");
            env_.store.finalize(env_.id, summary, outcomes);
        } catch (const std::exception& error) {
            std::cerr << "lite run " << env_.id << " finalization failed: " << error.what() << '\n';
            try {
                context_event("harness_error", std::string("finalization failed: ") + error.what());
            } catch (...) {}
            try {
                const requirements::ScoreSummary failure{requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
                env_.store.finalize(env_.id, failure, {});
            } catch (const std::exception& again) {
                std::cerr << "lite run " << env_.id << " could not be finalized: " << again.what() << '\n';
            }
        }
    }

    LiteRunEnvironment& env_;
    const RunConfig& config_;
    LiteSessionUrl url_;
    mutable scenarios::SteadyLiteClock clock_;
    std::uint64_t started_ns_{clock_.now_ns()};
    PublisherDriver driver_;
    DriverHandle handle_;
    std::string current_;
    std::size_t ordinal_{0};
    std::string connection_id_;
    bool operational_error_{false};
    requirements::LiteVerdicts verdicts_;
    std::size_t retained_bytes_{0};
    std::size_t stored_messages_{0};
    std::size_t total_messages_{0};
    std::size_t stored_issues_{0};
    std::size_t total_issues_{0};
};

}  // namespace

LiteSessionUrl lite_session_url(TransportKind transport) {
    if (transport == TransportKind::WebTransport) return {true, std::string(kWebTransportPath), {}};
    return {};
}

std::string lite_endpoint_uri(TransportKind transport, std::string_view authority) {
    if (transport == TransportKind::WebTransport) return "https://" + std::string(authority) + std::string(kWebTransportPath);
    return "moql://" + std::string(authority);
}

std::optional<RunStartStatus> lite_start_refusal(const RunConfig& config) {
    if (config.draft != DraftVersion::MoqLite06) return RunStartStatus::Unsupported;
    const auto& ids = config.scenario_ids;
    if (ids.empty() || ids.size() > kMaximumSelection) return RunStartStatus::InvalidConfig;
    if (config.timeout < 2ms || config.timeout > 3600000ms) return RunStartStatus::InvalidConfig;
    if (config.track_fixture && !usable_fixture(*config.track_fixture)) return RunStartStatus::InvalidConfig;
    std::set<std::string> seen;
    for (const auto& id : ids) {
        const auto traits = lite_executable_scenario(id);
        if (id.empty() || !seen.insert(id).second || !traits) return RunStartStatus::InvalidConfig;
        if ((traits->requires_track || config.mode == RunMode::Driven) && !config.track_fixture)
            return RunStartStatus::InvalidConfig;
    }
    return std::nullopt;
}

scenarios::LiteProbeDefinition lite_probe_for(const RunConfig& config, std::string_view id) {
    if (!lite_executable_scenario(id))
        throw std::invalid_argument("not an executable moq-lite-06 scenario: " + std::string(id));
    const auto url = lite_session_url(config.transport);
    const std::string path = config.track_fixture ? broadcast_path_of(*config.track_fixture) : std::string{};
    const std::string track = config.track_fixture ? config.track_fixture->track_name : std::string{};
    auto definition = build(id, config.timeout, path, track, url);
    definition.connect_deadline = config.timeout;
    definition.binding = binding_of(config.transport);
    // Set together and from one source (the Task 7 ruling): the client-path evaluators read all four.
    definition.session_url_has_path = url.has_path;
    definition.session_url_path = url.path;
    definition.session_url_query = url.query;
    return definition;
}

void run_lite(LiteRunEnvironment& environment, std::unique_ptr<transport::SessionTransport> listener,
              const RunConfig& config) {
    LiteRun(environment, config).run(std::move(listener));
}

}  // namespace moq::interop::app
