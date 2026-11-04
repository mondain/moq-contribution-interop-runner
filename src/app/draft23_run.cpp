#include "moq/interop/app/draft23_run.h"
#include "moq/interop/app/publisher_driver.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft23_evaluators.h"
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
        nlohmann::json summary{{"setup_received",stats.setup_received},{"namespace_announced",stats.namespace_announced},
            {"subscription_established",stats.subscription_established},{"objects",stats.objects},
            {"distinct_objects",stats.distinct_objects},{"payload_bytes",stats.payload_bytes},
            {"progressing",stats.progressing()},{"conformance_scored",false}};
        summary["status"] = delivered_at_goal && !stopped ? "PASS" : "NOT_RUN";
        if (session.error()) summary["error"] = *session.error();
        event("draft23_smoke",summary.dump());
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
        if (result.status == DriverStatus::Error || result.status == DriverStatus::TimedOut || result.status == DriverStatus::Signaled ||
            (result.status == DriverStatus::Exited && result.exit_code.value_or(1) != 0)) operational_error = true;
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
