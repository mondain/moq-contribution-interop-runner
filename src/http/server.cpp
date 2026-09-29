#include "moq/interop/http/server.h"

#include "detail.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace moq::interop::http {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kDefaultPageSize = 50;
constexpr std::size_t kMaximumPageSize = 100;
constexpr std::int64_t kMaximumTimeoutMs = 3'600'000;

std::optional<std::string> decode_hex(std::string_view encoded) {
    if ((encoded.size() & 1u) != 0u || encoded.size() > 8192) {
        return std::nullopt;
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string bytes;
    bytes.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        const int high = nibble(encoded[index]);
        const int low = nibble(encoded[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes.push_back(static_cast<char>((high << 4) | low));
    }
    return bytes;
}

void json_response(httplib::Response& response, const Json& body, int status = 200) {
    response.status = status;
    response.set_content(body.dump(), "application/json");
}

void error_response(httplib::Response& response, const ApiError& error) {
    json_response(response, detail::error_json(error), error.status);
}

template <typename Function>
void guarded(httplib::Response& response, Function&& function) {
    try {
        function();
    } catch (const ApiError& error) {
        error_response(response, error);
    } catch (const std::exception& error) {
        std::cerr << "HTTP request failed: " << error.what() << '\n';
        error_response(response, {500, "internal_error", "The request could not be completed."});
    } catch (...) {
        std::cerr << "HTTP request failed with an unknown exception\n";
        error_response(response, {500, "internal_error", "The request could not be completed."});
    }
}

std::size_t parse_unsigned(std::string_view value, std::string_view field) {
    if (value.empty()) throw ApiError{400, "invalid_pagination", std::string(field) + " is invalid."};
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw ApiError{400, "invalid_pagination", std::string(field) + " is invalid."};
    }
    return result;
}

storage::RunQuery query(const httplib::Request& request) {
    storage::RunQuery result{kDefaultPageSize, 0};
    if (request.has_param("limit")) {
        result.limit = parse_unsigned(request.get_param_value("limit"), "limit");
    }
    if (request.has_param("offset")) {
        result.offset = parse_unsigned(request.get_param_value("offset"), "offset");
    }
    if (result.limit == 0 || result.limit > kMaximumPageSize) {
        throw ApiError{400, "invalid_pagination", "limit must be between 1 and 100."};
    }
    if (result.offset > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw ApiError{400, "invalid_pagination", "offset is too large."};
    }
    return result;
}

app::RunConfig parse_run_config(const httplib::Request& request) {
    Json body;
    try {
        body = Json::parse(request.body);
    } catch (const Json::exception&) {
        throw ApiError{400, "invalid_json", "Request body must be valid JSON."};
    }
    if (!body.is_object()) {
        throw ApiError{400, "invalid_run_config", "Run configuration must be an object."};
    }
    try {
        const int draft = body.at("draft").get<int>();
        const auto transport = body.at("transport").get<std::string>();
        const auto mode = body.at("mode").get<std::string>();
        const auto scenarios = body.at("scenarios").get<std::vector<std::string>>();
        const auto timeout = body.at("timeout_ms").get<std::int64_t>();
        if (draft != 18 && draft != 21) {
            throw ApiError{400, "invalid_run_config", "draft must be 18 or 21."};
        }
        if (transport != "native-quic" && transport != "webtransport") {
            throw ApiError{400, "invalid_run_config",
                           "transport must be native-quic or webtransport."};
        }
        if (mode != "observed" && mode != "driven") {
            throw ApiError{400, "invalid_run_config", "mode must be observed or driven."};
        }
        if (scenarios.size() > 100) {
            throw ApiError{400, "invalid_run_config", "at most 100 scenarios may be selected."};
        }
        if (timeout <= 0 || timeout > kMaximumTimeoutMs) {
            throw ApiError{400, "invalid_run_config",
                           "timeout_ms must be between 1 and 3600000."};
        }
        std::optional<app::TrackFixture> track_fixture;
        if (body.contains("track")) {
            const auto& track = body.at("track");
            const auto namespace_hex =
                track.at("namespace_hex").get<std::vector<std::string>>();
            if (namespace_hex.size() > 32) {
                throw ApiError{400, "invalid_run_config",
                               "track namespace may have at most 32 fields."};
            }
            app::TrackFixture fixture;
            std::size_t total_bytes = 0;
            for (const auto& encoded : namespace_hex) {
                auto field = decode_hex(encoded);
                if (!field || field->empty() ||
                    field->size() > 4096 - total_bytes) {
                    throw ApiError{400, "invalid_run_config",
                                   "track namespace field is invalid."};
                }
                total_bytes += field->size();
                fixture.namespace_fields.push_back(std::move(*field));
            }
            auto name = decode_hex(track.at("name_hex").get<std::string>());
            if (!name || name->size() > 4096 - total_bytes) {
                throw ApiError{400, "invalid_run_config",
                               "track name is invalid."};
            }
            fixture.track_name = std::move(*name);
            track_fixture = std::move(fixture);
        }
        return {draft == 18 ? app::DraftVersion::Draft18 : app::DraftVersion::Draft21,
                transport == "native-quic" ? app::TransportKind::NativeQuic
                                             : app::TransportKind::WebTransport,
                mode == "observed" ? app::RunMode::Observed : app::RunMode::Driven,
                scenarios, std::chrono::milliseconds(timeout),
                std::move(track_fixture)};
    } catch (const ApiError&) {
        throw;
    } catch (const Json::exception&) {
        throw ApiError{400, "invalid_run_config", "Run configuration fields are invalid."};
    }
}

template <typename Item, typename Converter>
Json page_json(const storage::Page<Item>& page, Converter converter) {
    Json items = Json::array();
    for (const auto& item : page.items) items.push_back(converter(item));
    return {{"schema_version", 1},
            {"pagination", detail::pagination_json(page.limit, page.offset, page.total,
                                                    page.next_offset)},
            {"items", std::move(items)}};
}

void html_headers(httplib::Response& response) {
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Content-Security-Policy",
                        "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; "
                        "frame-ancestors 'none'");
}

}  // namespace

class HttpServer::Impl {
public:
    Impl(std::shared_ptr<const requirements::RequirementCatalog> supplied_draft18,
         std::shared_ptr<const requirements::RequirementCatalog> supplied_draft21,
         std::shared_ptr<storage::RunStore> supplied_store, app::BuildInfo supplied_build,
         ServerConfig supplied_config,
         std::shared_ptr<app::NativeRunManager> supplied_runs)
        : draft18(std::move(supplied_draft18)), draft21(std::move(supplied_draft21)),
          store(std::move(supplied_store)), build(std::move(supplied_build)),
          config(std::move(supplied_config)), runs(std::move(supplied_runs)) {
        if (!draft18 || !draft21 || !store) {
            throw std::invalid_argument("HTTP server dependencies must not be null");
        }
        if (draft18->draft != 18 || draft21->draft != 21) {
            throw std::invalid_argument("HTTP server catalogs must be draft 18 and draft 21");
        }
        register_routes();
    }

    void register_routes() {
        server.Get("/healthz", [this](const httplib::Request&, httplib::Response& response) {
            guarded(response, [this, &response] {
                try {
                    static_cast<void>(store->list({1, 0}));
                } catch (const std::exception& error) {
                    std::cerr << "Database readiness check failed: " << error.what() << '\n';
                    throw ApiError{503, "database_not_ready", "The database is not ready."};
                } catch (...) {
                    std::cerr << "Database readiness check failed with an unknown exception\n";
                    throw ApiError{503, "database_not_ready", "The database is not ready."};
                }
                json_response(response, {{"schema_version", 1},
                                         {"status", "ok"},
                                         {"database", {{"ready", true}}},
                                         {"supported_drafts", {18, 21}},
                                         {"executable_profiles", Json::array({
                                             {{"draft", 18}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "subscribe-to-publisher-track"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft18)}},
                                             {{"draft", 21}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "d21-publisher-request-stream-placement"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft21)}},
                                             {{"draft", 21}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "d21-setup-unknown-options"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft21)}},
                                             {{"draft", 21}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "d21-setup-duplicate-unknown-options"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft21)}}})},
                                         {"validator", detail::build_json(build)}});
            });
        });
        server.Get("/api/v1/drafts", [this](const httplib::Request&, httplib::Response& response) {
            guarded(response, [this, &response] {
                json_response(response, {{"schema_version", 1},
                                         {"drafts", {detail::catalog_json(*draft18),
                                                     detail::catalog_json(*draft21)}}});
            });
        });
        server.Get("/api/v1/requirements", [this](const httplib::Request& request,
                                                   httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                if (!request.has_param("draft")) {
                    throw ApiError{400, "missing_draft", "draft query parameter is required."};
                }
                const auto draft = request.get_param_value("draft");
                const auto* catalog = draft == "18" ? draft18.get() : draft == "21" ? draft21.get() : nullptr;
                if (!catalog) throw ApiError{400, "unsupported_draft", "draft must be 18 or 21."};
                const auto page = query(request);
                Json items = Json::array();
                const auto begin = std::min(page.offset, catalog->requirements.size());
                const auto count = std::min(page.limit, catalog->requirements.size() - begin);
                for (std::size_t index = begin; index < begin + count; ++index) {
                    items.push_back(detail::requirement_json(catalog->requirements[index]));
                }
                const auto next = begin + count < catalog->requirements.size()
                                      ? std::optional<std::size_t>(begin + count) : std::nullopt;
                json_response(response, {{"schema_version", 1}, {"draft", catalog->draft},
                    {"pagination", detail::pagination_json(page.limit, page.offset,
                                                            catalog->requirements.size(), next)},
                    {"items", std::move(items)}});
            });
        });
        server.Post("/api/v1/runs", [this](const httplib::Request& request,
                                            httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                const auto requested = parse_run_config(request);
                const bool draft18_scenario =
                    requested.draft == app::DraftVersion::Draft18 &&
                    requested.scenario_ids ==
                        std::vector<std::string>{"subscribe-to-publisher-track"};
                const bool draft21_scenario =
                    requested.draft == app::DraftVersion::Draft21 &&
                    requested.scenario_ids.size() == 1 &&
                    (requested.scenario_ids.front() ==
                         "d21-publisher-request-stream-placement" ||
                     requested.scenario_ids.front() ==
                         "d21-setup-unknown-options" ||
                     requested.scenario_ids.front() ==
                         "d21-setup-duplicate-unknown-options");
                if ((!draft18_scenario && !draft21_scenario) ||
                    requested.transport != app::TransportKind::NativeQuic ||
                    requested.mode != app::RunMode::Observed ||
                    (runs && !runs->supports(requested.draft))) {
                    throw ApiError{422, "unsupported_run_config",
                                   "The requested native-QUIC observed scenario is not executable."};
                }
                if (!requested.track_fixture || requested.timeout < std::chrono::milliseconds(2)) {
                    throw ApiError{400, "invalid_run_config",
                                   "This scenario requires track and timeout_ms of at least 2."};
                }
                if (!runs) {
                    throw ApiError{503, "publisher_listener_unavailable",
                                   "The native publisher listener is not configured."};
                }
                const auto started = runs->start(requested);
                switch (started.status) {
                case app::RunStartStatus::Started:
                    json_response(response, {{"schema_version", 1},
                        {"run", detail::run_json(store->load(started.id))},
                        {"publisher_endpoint", {{"address", started.endpoint.address},
                                                {"port", started.endpoint.port},
                                                {"alpn", draft21_scenario ? "moqt-21" : "moqt-18"}}}}, 201);
                    return;
                case app::RunStartStatus::Unsupported:
                    throw ApiError{422, "unsupported_run_config", "This run configuration is not executable."};
                case app::RunStartStatus::InvalidConfig:
                    throw ApiError{400, "invalid_run_config", "Run configuration is invalid."};
                case app::RunStartStatus::PortExhausted:
                    throw ApiError{503, "publisher_ports_exhausted", "No publisher listener port is available."};
                case app::RunStartStatus::ListenerError:
                    throw ApiError{503, "publisher_listener_unavailable", "The native publisher listener could not start."};
                }
            });
        });
        server.Get("/api/v1/runs", [this](const httplib::Request& request,
                                           httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                json_response(response, page_json(store->list(query(request)), detail::run_summary_json));
            });
        });
        server.Get(R"(/api/v1/runs/(.+)/events)", [this](const httplib::Request& request,
                                                          httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                try {
                    json_response(response, page_json(store->list_events(request.matches[1], query(request)),
                                                      detail::event_json));
                } catch (const std::out_of_range&) {
                    throw ApiError{404, "run_not_found", "The requested run was not found."};
                }
            });
        });
        server.Get(R"(/api/v1/runs/(.+))", [this](const httplib::Request& request,
                                                   httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                try {
                    json_response(response, {{"schema_version", 1},
                                             {"run", detail::run_json(store->load(request.matches[1]))}});
                } catch (const std::out_of_range&) {
                    throw ApiError{404, "run_not_found", "The requested run was not found."};
                }
            });
        });
        server.Post(R"(/api/v1/runs/(.+)/stop)", [this](const httplib::Request& request,
                                                       httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                const std::string id = request.matches[1];
                storage::RunRecord run;
                try {
                    run = store->load(id);
                } catch (const std::out_of_range&) {
                    throw ApiError{404, "run_not_found", "The requested run was not found."};
                }
                if (run.state == storage::RunState::Finalized) {
                    throw ApiError{409, "run_finalized", "The run has already finalized."};
                }
                if (!runs) {
                    throw ApiError{503, "publisher_listener_unavailable",
                                   "The native publisher listener is not configured."};
                }
                if (!runs->stop(id)) {
                    throw ApiError{409, "run_not_active", "The run is not active in this process."};
                }
                json_response(response, {{"schema_version", 1},
                                         {"run", detail::run_json(store->load(id))}});
            });
        });
        server.Get(R"(/results/(.+)\.json)", [this](const httplib::Request& request,
                                                      httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                try {
                    json_response(response, {{"schema_version", 1},
                                             {"run", detail::run_json(store->load(request.matches[1]))}});
                } catch (const std::out_of_range&) {
                    throw ApiError{404, "run_not_found", "The requested run was not found."};
                }
            });
        });
        server.Get("/results", [this](const httplib::Request&, httplib::Response& response) {
            guarded(response, [this, &response] {
                const auto runs = store->list({100, 0});
                response.status = 200;
                html_headers(response);
                response.set_content(detail::render_run_list(runs.items), "text/html; charset=utf-8");
            });
        });
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.status == 404 && response.body.empty()) {
                error_response(response, {404, "not_found", "The requested resource was not found."});
            }
        });
    }

    std::shared_ptr<const requirements::RequirementCatalog> draft18;
    std::shared_ptr<const requirements::RequirementCatalog> draft21;
    std::shared_ptr<storage::RunStore> store;
    app::BuildInfo build;
    ServerConfig config;
    std::shared_ptr<app::NativeRunManager> runs;
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> active{false};
    std::uint16_t bound_port = 0;
    std::mutex lifecycle;
};

HttpServer::HttpServer(std::shared_ptr<const requirements::RequirementCatalog> draft18,
                       std::shared_ptr<const requirements::RequirementCatalog> draft21,
                       std::shared_ptr<storage::RunStore> store, app::BuildInfo build,
                       ServerConfig config,
                       std::shared_ptr<app::NativeRunManager> runs)
    : impl_(std::make_unique<Impl>(std::move(draft18), std::move(draft21), std::move(store),
                                   std::move(build), std::move(config), std::move(runs))) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->active) return true;
    if (impl_->thread.joinable()) impl_->thread.join();
    const int port = impl_->config.port == 0
        ? impl_->server.bind_to_any_port(impl_->config.bind_address)
        : (impl_->server.bind_to_port(impl_->config.bind_address, impl_->config.port)
               ? impl_->config.port
               : -1);
    if (port <= 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        std::cerr << "HTTP listener bind failed\n";
        return false;
    }
    impl_->bound_port = static_cast<std::uint16_t>(port);
    impl_->active = true;
    impl_->thread = std::thread([impl = impl_.get()] {
        impl->server.listen_after_bind();
        impl->active = false;
    });
    impl_->server.wait_until_ready();
    if (!impl_->server.is_running()) {
        std::cerr << "HTTP listener stopped during startup\n";
        if (impl_->thread.joinable()) impl_->thread.join();
        impl_->active = false;
        return false;
    }
    return true;
}

void HttpServer::stop() {
    std::unique_lock lock(impl_->lifecycle);
    impl_->server.stop();
    auto thread = std::move(impl_->thread);
    lock.unlock();
    if (thread.joinable()) thread.join();
    impl_->active = false;
}

bool HttpServer::running() const { return impl_->active; }
std::uint16_t HttpServer::port() const { return impl_->bound_port; }

}  // namespace moq::interop::http
