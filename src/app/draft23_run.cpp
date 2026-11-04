#include "moq/interop/app/draft23_run.h"
#include "moq/interop/app/publisher_driver.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft23_evaluators.h"
#include "moq/interop/session/draft23_probes.h"
#include "moq/interop/session/draft23_session.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
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
    bool delivered_at_goal = false;
    requirements::Draft23Evidence setup_context;
    setup_context.webtransport = config.transport == TransportKind::WebTransport;
    setup_context.authority = authority;
    setup_context.path = "/moq";
    setup_context.scenario = config.scenario_ids.front();
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
        settings.probe = session::find_draft23_probe(config.scenario_ids.front());
        // A probe that talks on the runner's SUBSCRIBE stream needs the runner to subscribe first.
        if (settings.probe && settings.probe->channel == session::Draft23ProbeChannel::SubscribeExtra) settings.subscribe_immediately = true;
        // A publisher that offers its tracks with PUBLISH needs them accepted; it does not wait for a SUBSCRIBE. The PUBLISH
        // smoke does so, and so do the probes that answer or extend one of the publisher's PUBLISH requests.
        settings.publisher_pushes = config.scenario_ids.front() == "d23-publish-smoke" ||
            (settings.probe && (settings.probe->channel == session::Draft23ProbeChannel::PublishReply ||
                                settings.probe->channel == session::Draft23ProbeChannel::PublishExtra));
        const session::Draft23Probe* settings_probe = settings.probe;
        session::Draft23Session session(*listener, std::move(settings));
        const auto endpoint = std::string(config.transport == TransportKind::WebTransport ? "https://" : "moqt://") + authority + "/moq";
        event("context_start", "draft=23 protocol=moqt-23 endpoint=" + endpoint + " scoring=staged");
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
        // Once media is flowing the run lingers briefly so PUBLISH_DONE, stream FINs and the late data streams can be
        // observed. The smoke verdict is fixed at that moment: later activity can add evidence but never undo delivery.
        constexpr auto kLinger = std::chrono::milliseconds(1500);
        std::optional<std::chrono::steady_clock::time_point> goal_at;
        std::optional<std::chrono::steady_clock::time_point> probe_injected_at;
        std::optional<std::chrono::steady_clock::time_point> probe_reply_at;
        const auto probe_window = std::min<std::chrono::milliseconds>(config.timeout, std::chrono::milliseconds(3000));
        while (!stopped && std::chrono::steady_clock::now() < deadline && !session.error()) {
            for (const auto& e : listener->poll(256)) session.on_event(e);
            session.flush();
            if (!connected && session.stats().connected) {
                connected = true;
                deadline = std::chrono::steady_clock::now() + config.timeout;
            }
            if (!goal_at && session.stats().setup_received && session.stats().subscription_established && session.stats().progressing()) {
                goal_at = std::chrono::steady_clock::now();
                delivered_at_goal = !session.error() && !stopped;
            }
            if (const auto& probe_state = session.stats().probe) {
                const auto now = std::chrono::steady_clock::now();
                if (probe_state->injected && !probe_injected_at) probe_injected_at = now;
                // The probe ends the session: stop at the publisher's close, or when it has had its window to react.
                // A reply probe is answered on its own stream and leaves the session open.
                if (probe_state->reply && !probe_reply_at) probe_reply_at = now;
                // After a REQUEST_ERROR reply the FIN may follow a moment later; wait briefly so the rejection is judged whole.
                const bool reply_settled = probe_reply_at && settings_probe->expect == session::Draft23ProbeExpect::RequestError &&
                    (probe_state->reply_fin || now - *probe_reply_at >= std::chrono::milliseconds(300));
                if (probe_state->close || reply_settled || (probe_injected_at && now - *probe_injected_at >= probe_window)) break;
            }
            if (goal_at) {
                if (std::chrono::steady_clock::now() - *goal_at >= kLinger) break;
                const auto& requests = session.stats().requests;
                const bool established = std::any_of(requests.begin(), requests.end(), [](const auto& r) { return r.established; });
                const bool all_finished = std::all_of(requests.begin(), requests.end(), [](const auto& r) { return !r.established || r.fin; });
                if (established && all_finished) break;
            }
            if (handle.valid()) {
                auto result = driver.poll(handle);
                if (result.status != DriverStatus::Running) {
                    event("publisher_process", serialize_driver_result(result));
                    // Whatever the publisher already sent has been processed; stop waiting for more.
                    for (const auto& e : listener->poll(256)) session.on_event(e);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto& stats = session.stats();
        if (stats.probe) {
            const auto& probe = *settings_probe;
            nlohmann::json detail{{"scenario",probe.scenario_id},{"requirement",probe.requirement_id},
                {"expected_code",probe.expected_code},{"injected",stats.probe->injected},
                {"reply_bytes",stats.probe->reply_bytes},{"reply_fin",stats.probe->reply_fin},
                {"other_stream_ends",[&] {
                    nlohmann::json ends = nlohmann::json::array();
                    for (const auto& other : stats.probe->other_stream_ends)
                        ends.push_back({{"stream_id",other.stream_id},
                            {"kind",other.end.kind==session::Draft23ProbeStreamEnd::Kind::Reset?"reset":"stop_sending"},
                            {"code",other.end.code ? nlohmann::json(*other.end.code) : nlohmann::json()}});
                    return ends; }()},
                {"stream_end",stats.probe->stream_end ? nlohmann::json{{"kind",stats.probe->stream_end->kind==session::Draft23ProbeStreamEnd::Kind::Reset?"reset":"stop_sending"},
                    {"code",stats.probe->stream_end->code ? nlohmann::json(*stats.probe->stream_end->code) : nlohmann::json()}} : nlohmann::json()},
                {"reply",stats.probe->reply ? nlohmann::json{{"type",stats.probe->reply->type},
                    {"error_code",stats.probe->reply->error_code ? nlohmann::json(*stats.probe->reply->error_code) : nlohmann::json()},
                    {"detail_types",stats.probe->reply->detail_types}} : nlohmann::json()},
                {"close",stats.probe->close ? nlohmann::json{{"application",stats.probe->close->application},
                    {"code",stats.probe->close->code}} : nlohmann::json()}};
            if (session.error()) detail["session_note"] = *session.error();
            event("draft23_probe",detail.dump());
            if (stats.probe->close) event("peer_closed",detail.dump());
            setup_context.probe_scenario = config.scenario_ids.front();
            setup_context.probe = stats.probe;
        } else {
        nlohmann::json summary{{"setup_received",stats.setup_received},{"namespace_announced",stats.namespace_announced},
            {"subscription_established",stats.subscription_established},{"objects",stats.objects},
            {"distinct_objects",stats.distinct_objects},{"payload_bytes",stats.payload_bytes},
            {"progressing",stats.progressing()},{"conformance_scored",false}};
        summary["status"] = delivered_at_goal && !stopped ? "PASS" : "NOT_RUN";
        if (session.error()) summary["error"] = *session.error();
        event("draft23_smoke",summary.dump());
        }
        setup_context.published_tracks = stats.published_tracks;
        setup_context.announced_namespaces = stats.announced_namespaces;
        setup_context.connected = stats.connected;
        setup_context.max_datagram_payload = stats.max_datagram_payload;
        setup_context.empty_track_name = stats.empty_track_name;
        setup_context.empty_namespace_field = stats.empty_namespace_field;
        setup_context.duplicate_parameter = stats.duplicate_parameter;
        setup_context.delta_overflow = stats.delta_overflow;
        setup_context.control_stream_closed = stats.control_stream_closed;
        setup_context.padding_streams = stats.padding_streams;
        setup_context.padding_nonzero = stats.padding_nonzero;
        setup_context.data_streams = stats.data_streams;
        setup_context.requests = stats.requests;
        setup_context.subscription_alias = stats.subscription_alias;
        setup_context.direct_subscribe = config.scenario_ids.front() == "d23-direct-subscribe-smoke";
        setup_context.duplicate_request_id = stats.duplicate_request_id;
        setup_context.oversize_control_message = stats.oversize_control_message;
        if (!stats.requests.empty()) {
            nlohmann::json requests = nlohmann::json::array();
            for (const auto& request : stats.requests) {
                nlohmann::json messages = nlohmann::json::array();
                for (const auto& message : request.messages) messages.push_back({{"type",message.type},{"body_length",message.body_length}});
                requests.push_back({{"stream_id",request.stream_id},{"local",request.local},{"established",request.established},
                    {"request_id",request.request_id ? nlohmann::json(*request.request_id) : nlohmann::json()},
                    {"messages",messages},{"fin",request.fin},{"fin_after_publish_done",request.fin_after_publish_done},
                    {"publish_done",request.publish_done ? nlohmann::json{{"status_code",request.publish_done->status_code},
                        {"stream_count",request.publish_done->stream_count}} : nlohmann::json()}});
            }
            event("request_observed",nlohmann::json{{"requests",requests}}.dump());
        }
        if (!stats.data_streams.empty()) {
            nlohmann::json streams = nlohmann::json::array();
            for (const auto& stream : stats.data_streams) {
                streams.push_back({{"stream_id",stream.stream_id},{"type_flags",stream.type_flags},{"valid_header",stream.valid_header},
                    {"track_alias",stream.track_alias},{"group_id",stream.group_id},
                    {"subgroup_id",stream.subgroup_id ? nlohmann::json(*stream.subgroup_id) : nlohmann::json()},
                    {"objects",stream.objects},{"fin",stream.fin},{"reset",stream.reset}});
            }
            event("peer_stream_classified",nlohmann::json{{"streams",streams},{"truncated",stats.data_streams_truncated}}.dump());
        }
        if (stats.peer_setup) {
            setup_context.peer_setup = stats.peer_setup;
            nlohmann::json options = nlohmann::json::array();
            for (const auto& option : stats.peer_setup->options) {
                const auto* value = std::get_if<std::vector<std::byte>>(&option.value);
                options.push_back({{"type",option.type},{"bytes",value ? value->size() : 0},
                    {"number",std::holds_alternative<std::uint64_t>(option.value) ? nlohmann::json(std::get<std::uint64_t>(option.value)) : nlohmann::json()}});
            }
            event("peer_setup_received",nlohmann::json{{"frame_length",stats.peer_setup->frame_length},{"options",options}}.dump());
        }
        if (!stats.connected) { operational_error = true; event("harness_error", "draft-23 publisher did not connect before deadline"); }
        if (stopped) { operational_error = true; event("run_stopped", "operator stopped draft-23 smoke"); }
    } catch (const std::exception& error) {
        operational_error = true;
        try { event("harness_error",error.what()); } catch (...) {}
    }
    if (handle.valid()) {
        const auto result = driver.stop(handle);
        try { event("publisher_process",serialize_driver_result(result)); } catch (...) { operational_error = true; }
        // A publisher that rejects a probe by closing the session may exit non-zero; that is the behavior under test.
        const bool probe_run = session::find_draft23_probe(config.scenario_ids.front()) != nullptr;
        if (result.status == DriverStatus::Error || result.status == DriverStatus::TimedOut || result.status == DriverStatus::Signaled ||
            (!probe_run && result.status == DriverStatus::Exited && result.exit_code.value_or(1) != 0)) operational_error = true;
    }
    (void)listener->close(0, {});
    try {
        // Rows outside the reviewed SETUP slice stay at their staged state, so smoke delivery alone never scores them.
        auto outcomes = requirements::evaluate_draft23(catalog, setup_context);
        auto score = requirements::score_staged(catalog, outcomes);
        if (operational_error) score.verdict = requirements::RunVerdict::Error;
        store.finalize(id,score,outcomes);
    } catch (...) {
        try { store.finalize(id,{requirements::RunVerdict::Error,{0,0},{0,0},{0,0}},{}); } catch (...) {}
    }
}
}
