#include "moq/interop/http/server.h"
#include "json.h"
#include "moq/interop/http/result_schema.h"
#include "moq/interop/app/publisher_capabilities.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/app/draft_traits.h"
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
#include "moq/interop/scenarios/immutable_repeat.h"
#include "moq/interop/scenarios/object_repeat.h"
#include "moq/interop/scenarios/draft18_contribution.h"
#include "moq/interop/scenarios/request_goaway.h"
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/draft21_contribution.h"
#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/draft21_response.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_evaluators.h"
#include "moq/interop/requirements/execution_audit.h"

#include "detail.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <set>
#include <span>
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

detail::ReportFilters report_filters(const httplib::Request& request) {
    detail::ReportFilters filters;
    const auto read = [&](std::string_view name, std::size_t maximum) {
        if (!request.has_param(std::string(name))) return std::string{};
        auto value = request.get_param_value(std::string(name));
        if (value.size() > maximum ||
            std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20; }))
            throw ApiError{400, "invalid_report_filter", "Report filter is invalid."};
        return value;
    };
    filters.strength = read("strength", 16);
    filters.outcome = read("outcome", 32);
    filters.section = read("section", 64);
    filters.scenario = read("scenario", 256);
    if (!filters.strength.empty() &&
        filters.strength != "MUST" && filters.strength != "MUST NOT" &&
        filters.strength != "SHOULD" && filters.strength != "SHOULD NOT" &&
        filters.strength != "MAY")
        throw ApiError{400, "invalid_report_filter", "Strength filter is invalid."};
    if (!filters.outcome.empty() &&
        filters.outcome != "pass" && filters.outcome != "fail" &&
        filters.outcome != "not_run" && filters.outcome != "not_testable" &&
        filters.outcome != "not_applicable" && filters.outcome != "unobserved" &&
        filters.outcome != "error")
        throw ApiError{400, "invalid_report_filter", "Outcome filter is invalid."};
    return filters;
}

app::RunConfig parse_run_config(const httplib::Request& request,
                                const app::PublisherCapabilities& default_capabilities) {
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
        const auto parsed_draft = detail::parse_draft_json(body.at("draft"));
        const auto transport = body.at("transport").get<std::string>();
        const auto mode = body.at("mode").get<std::string>();
        const auto scenarios = body.at("scenarios").get<std::vector<std::string>>();
        const auto timeout = body.at("timeout_ms").get<std::int64_t>();
        // The integer form is MoQ Transport only and moq-lite is its string; 106 is never accepted.
        if (!parsed_draft) {
            throw ApiError{400, "invalid_run_config", "draft must be 18, 21 or 22."};
        }
        const unsigned draft = app::draft_number(*parsed_draft);
        if (transport != "native-quic" && transport != "webtransport") {
            throw ApiError{400, "invalid_run_config",
                           "transport must be native-quic or webtransport."};
        }
        if (mode != "observed" && mode != "driven") {
            throw ApiError{400, "invalid_run_config", "mode must be observed or driven."};
        }
        if (scenarios.empty() || scenarios.size() > 100 ||
            std::set<std::string>(scenarios.begin(),scenarios.end()).size() != scenarios.size() ||
            std::any_of(scenarios.begin(),scenarios.end(),[](const auto& id) { return id.empty(); })) {
            throw ApiError{400, "invalid_run_config", "Select between 1 and 100 distinct nonempty scenarios."};
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
        if (track_fixture && std::any_of(scenarios.begin(),scenarios.end(),[&](const auto& id) {
            return app::fetch_first_object_scenario(static_cast<unsigned>(draft),id) ||
                app::fetch_group_order_scenario(static_cast<unsigned>(draft),id) ||
                app::immutable_repeat_scenario(static_cast<unsigned>(draft),id) ||
                app::object_repeat_scenario(static_cast<unsigned>(draft),id) ||
                (draft == 21 && id == "d21-publish-state-notify-on-fetch") ||
                (draft == 22 && app::implementation_scenario_id(id) == "d21-publish-state-notify-on-fetch") ||
                // Every own draft 22 scenario builds its requests from the fixture and refuses the fixtures
                // this check refuses (draft22_*.cpp build()), so name the fixture instead of a generic 400.
                (draft == 22 && app::own_scenario_22(id).has_value()) ||
                app::gap_raw_scenario(static_cast<unsigned>(draft),id) ||
                app::subscriber_notify_scenario(static_cast<unsigned>(draft),id) ||
                app::established_update_scenario(static_cast<unsigned>(draft),id);
        })) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : track_fixture->namespace_fields) {
                std::vector<std::byte> value;
                for (const unsigned char byte : field) value.push_back(static_cast<std::byte>(byte));
                fields.push_back(std::move(value));
            }
            std::vector<std::byte> name;
            for (const unsigned char byte : track_fixture->track_name) name.push_back(static_cast<std::byte>(byte));
            if (!scenarios::fetch_first_object_fixture_valid(fields,name))
                throw ApiError{400,"invalid_run_config","Invalid FETCH track namespace or name."};
        }
        if (track_fixture && std::any_of(scenarios.begin(),scenarios.end(),[&](const auto& id) {
            return app::discovery_overlap_scenario(static_cast<unsigned>(draft),id);
        })) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : track_fixture->namespace_fields) {
                std::vector<std::byte> value;
                for (const unsigned char byte : field) value.push_back(static_cast<std::byte>(byte));
                fields.push_back(std::move(value));
            }
            if (!scenarios::discovery_overlap_namespace_valid(fields))
                throw ApiError{400,"invalid_run_config","Discovery overlap namespace requires at most 31 fields and 4094 bytes, with a nonreserved first field."};
        }
        // Declared capabilities: an explicit per-run value wins over the startup default.
        app::PublisherCapabilities capabilities = default_capabilities;
        if (body.contains("publisher_capabilities")) {
            const auto& declared = body.at("publisher_capabilities");
            if (!declared.is_object()) {
                throw ApiError{400, "invalid_publisher_capabilities",
                               "publisher_capabilities must be an object such as {\"fetch\": false}."};
            }
            for (const auto& [name, value] : declared.items()) {
                if (name != "fetch") {
                    throw ApiError{400, "invalid_publisher_capabilities",
                                   "Unknown publisher capability; the only supported name is fetch."};
                }
                if (!value.is_boolean()) {
                    throw ApiError{400, "invalid_publisher_capabilities",
                                   "publisher_capabilities.fetch must be true or false."};
                }
                capabilities.fetch = value.get<bool>();
            }
        }
        return {*parsed_draft,
                transport == "native-quic" ? app::TransportKind::NativeQuic
                                             : app::TransportKind::WebTransport,
                mode == "observed" ? app::RunMode::Observed : app::RunMode::Driven,
                scenarios, std::chrono::milliseconds(timeout),
                std::move(track_fixture), capabilities};
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

bool has_declared_evidence(const storage::RunRecord& run,
                           std::span<const requirements::ExecutableBinding> bindings,
                           const std::string& requirement_id) {
    if (std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "compatibility_error_mapping" && event.requirement_id == requirement_id;
        })) return false;
    for (const auto& binding : bindings) {
        if (binding.requirement_id != requirement_id || binding.evidence_kinds.empty() ||
            std::find(run.config.scenario_ids.begin(), run.config.scenario_ids.end(),
                      binding.scenario_id) == run.config.scenario_ids.end()) continue;
        const bool complete = std::all_of(binding.evidence_kinds.begin(),
            binding.evidence_kinds.end(), [&](const std::string& kind) {
                return std::any_of(run.events.begin(), run.events.end(),
                    [&](const storage::EvidenceEvent& event) {
                        return event.kind == kind && event.scenario_id &&
                               *event.scenario_id == binding.scenario_id;
                    });
            });
        if (complete) return true;
    }
    return false;
}

// The executable bindings of a configured catalog's draft (draft 22's are draft 21's translated through the
// lineage plus its own).
std::vector<requirements::ExecutableBinding> executable_bindings(app::DraftVersion draft) {
    switch (draft) {
        case app::DraftVersion::Draft18: return requirements::draft18_executable_bindings();
        case app::DraftVersion::Draft21: return requirements::draft21_executable_bindings();
        case app::DraftVersion::Draft22: return requirements::draft22_executable_bindings();
        case app::DraftVersion::MoqLite06:
            throw std::logic_error("moq-lite has no MoQ Transport executable bindings");
    }
    throw std::logic_error("unknown draft");
}

// One entry per configured catalog: drafts 18 and 21 always, draft 22 only when its catalog is configured
// (`draft22` is null otherwise, and there is then no draft 22 entry).
Json completeness_json(const requirements::RequirementCatalog& draft18,
                       const requirements::RequirementCatalog& draft21,
                       const requirements::RequirementCatalog* draft22,
                       const storage::RunStore& store, const app::BuildInfo& build) {
    Json drafts = Json::array();
    std::vector<const requirements::RequirementCatalog*> catalogs{&draft18, &draft21};
    if (draft22) catalogs.push_back(draft22);
    for (const auto* catalog : catalogs) {
        const auto bindings = executable_bindings(*app::parse_draft(catalog->draft));
        const auto audit = requirements::audit_completeness(
            *catalog, bindings, app::executable_scenarios(catalog->draft));
        Json findings = Json::array();
        for (const auto& finding : audit.findings) {
            findings.push_back({{"code", finding.code},
                                {"requirement_id", finding.requirement_id},
                                {"detail", finding.detail},
                                {"blocking", finding.blocking}});
        }
        Json residuals = Json::array();
        for (const auto& row : catalog->requirements) {
            const char* classification = nullptr;
            if (row.applicability == requirements::Applicability::Informative)
                classification = "informative";
            else if (row.applicability == requirements::Applicability::NotApplicable)
                classification = "not_applicable";
            else if (row.testability == requirements::Testability::NotTestable)
                classification = "not_testable";
            if (!classification) continue;
            residuals.push_back({{"requirement_id", row.id},
                                 {"classification", classification},
                                 {"reason", row.rationale},
                                 {"section", row.source.section},
                                 {"first_line", row.source.first_line}});
        }
        Json transports = Json::array();
        for (const auto transport : {app::TransportKind::NativeQuic,
                                     app::TransportKind::WebTransport}) {
            std::vector<storage::RunRecord> runs;
            for (std::size_t offset = 0;; offset += 100) {
                const auto page = store.list({100, offset});
                for (const auto& summary : page.items) {
                    if (static_cast<unsigned>(summary.config.draft) == catalog->draft &&
                        summary.config.transport == transport)
                        runs.push_back(store.load(summary.id));
                }
                if (!page.next_offset) break;
            }
            const auto execution = requirements::audit_execution(*catalog, bindings, runs);
            Json execution_findings = Json::array();
            for (const auto& finding : execution.findings) {
                execution_findings.push_back({{"code", finding.code},
                                              {"run_id", finding.run_id},
                                              {"requirement_id", finding.requirement_id},
                                              {"detail", finding.detail}});
            }
            std::set<std::string> observed;
            for (const auto& run : runs) {
                for (const auto& outcome : run.outcomes) {
                    if ((outcome.state == requirements::OutcomeState::Pass ||
                         outcome.state == requirements::OutcomeState::Fail) &&
                        has_declared_evidence(run, bindings, outcome.requirement_id))
                        observed.insert(outcome.requirement_id);
                }
            }
            Json not_run = Json::array();
            for (const auto& row : catalog->requirements) {
                if (row.applicability != requirements::Applicability::Applicable ||
                    row.testability != requirements::Testability::Testable ||
                    observed.contains(row.id)) continue;
                not_run.push_back({{"requirement_id", row.id},
                                   {"classification", "not_run"},
                                   {"reason", "No evidence-backed scored observation for this draft and transport."},
                                   {"section", row.source.section},
                                   {"first_line", row.source.first_line}});
            }
            transports.push_back({{"transport", transport == app::TransportKind::NativeQuic
                                                  ? "native-quic" : "webtransport"},
                                  {"run_count", execution.run_count},
                                  {"scored_rows", execution.scored_rows},
                                  {"observed_requirement_count", observed.size()},
                                  {"execution_consistent", execution.consistent()},
                                  {"execution_finding_count", execution.findings.size()},
                                  {"execution_findings", std::move(execution_findings)},
                                  {"not_run_count", not_run.size()},
                                  {"not_run", std::move(not_run)}});
        }
        drafts.push_back({{"draft", detail::catalog_draft_json(catalog->draft)},
                          {"source_sha256", catalog->source_sha256},
                          {"catalog_rows", catalog->requirements.size()},
                          {"required_covered", audit.required_covered},
                          {"required_total", audit.required_total},
                          {"optional_covered", audit.optional_covered},
                          {"optional_total", audit.optional_total},
                          {"evaluator_complete", audit.complete()},
                          {"findings", std::move(findings)},
                          {"classified_residuals", std::move(residuals)},
                          {"transports", std::move(transports)}});
    }
    return {{"schema_version", 1}, {"source_revision", build.source_revision},
            {"drafts", std::move(drafts)}};
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
        if (config.draft22_catalog && config.draft22_catalog->draft != 22) {
            throw std::invalid_argument("HTTP server draft 22 catalog must be draft 22");
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
                Json profiles = Json::array({
                                             {{"draft", 18}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "subscribe-to-publisher-track"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft18)}},
                                             {{"draft", 18}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "subscribe-again-to-established-publisher-track"},
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
                                                  app::DraftVersion::Draft21)}},
                                             {{"draft", 21}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "d21-server-sends-authority"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft21)}},
                                             {{"draft", 21}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "d21-server-sends-path"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft21)}},
                                             {{"draft", 18}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "fetch-publisher-track-range"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft18)}},
                                             {{"draft", 18}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "subscribe-namespace-at-publisher"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft18)}},
                                             {{"draft", 18}, {"transport", "native-quic"},
                                              {"mode", "observed"},
                                              {"scenario", "subscribe-tracks-at-publisher"},
                                              {"configured", runs && runs->supports(
                                                  app::DraftVersion::Draft18)}}});
                const auto native_count = profiles.size();
                for (std::size_t index = 0; index < native_count; ++index) {
                    auto webtransport = profiles.at(index);
                    webtransport["transport"] = "webtransport";
                    profiles.push_back(std::move(webtransport));
                }
                const auto append_profile = [&](unsigned draft, std::string_view id,
                                                const char* transport) {
                    profiles.push_back({{"draft", draft}, {"transport", transport},
                        {"mode", "observed"}, {"scenario", id},
                        {"configured", runs && runs->supports(static_cast<app::DraftVersion>(draft))}});
                };
                for (const auto& probe : scenarios::draft18_close_profiles()) {
                    if (!probe.webtransport_only) append_profile(18, probe.scenario_id, "native-quic");
                    if (!probe.native_only) append_profile(18, probe.scenario_id, "webtransport");
                }
                for (const auto& probe : scenarios::draft21_close_probes()) {
                    append_profile(21, probe.definition.id, "native-quic");
                    append_profile(21, probe.definition.id, "webtransport");
                }
                for (const auto& profile : scenarios::draft18_request_profiles()) {
                    append_profile(18, profile.definition.id, "native-quic");
                    append_profile(18, profile.definition.id, "webtransport");
                }
                for (const auto& profile : scenarios::draft21_request_profiles()) {
                    append_profile(21, profile.definition.id, "native-quic");
                    append_profile(21, profile.definition.id, "webtransport");
                }
                std::set<std::string> peer_scenarios;
                for (const auto& profile : scenarios::draft18_peer_close_probes()) {
                    if (!peer_scenarios.insert(profile.definition.id).second) continue;
                    append_profile(18, profile.definition.id, "native-quic");
                    append_profile(18, profile.definition.id, "webtransport");
                }
                for (const auto& profile : scenarios::draft21_peer_close_probes()) {
                    append_profile(21, profile.definition.id, "native-quic");
                    append_profile(21, profile.definition.id, "webtransport");
                }
                for (const auto& profile : scenarios::draft21_response_probes()) {
                    append_profile(21, profile.definition.id, "native-quic");
                    append_profile(21, profile.definition.id, "webtransport");
                }
                for (const auto& profile : scenarios::draft18_response_probes()) {
                    append_profile(18, profile.definition.id, "native-quic");
                    append_profile(18, profile.definition.id, "webtransport");
                }
                for (const unsigned draft : {18u, 21u}) {
                    std::set<std::string> fetch_scenarios;
                    const auto fetches = draft == 18 ? scenarios::draft18_fetch_probes()
                                                    : scenarios::draft21_fetch_probes();
                    for (const auto& profile : fetches) {
                        if (!fetch_scenarios.insert(profile.definition.id).second) continue;
                        append_profile(draft, profile.definition.id, "native-quic");
                        append_profile(draft, profile.definition.id, "webtransport");
                    }
                }
                for (const unsigned draft : {18u, 21u}) {
                    const auto cancellations = draft == 18 ? scenarios::draft18_subscription_cancel_probes()
                                                          : scenarios::draft21_subscription_cancel_probes();
                    for (const auto& profile : cancellations) {
                        append_profile(draft, profile.definition.id, "native-quic");
                        append_profile(draft, profile.definition.id, "webtransport");
                    }
                }
                for (const auto& profile : scenarios::draft21_fetch_response_probes()) {
                    append_profile(21, profile.definition.id, "native-quic");
                    append_profile(21, profile.definition.id, "webtransport");
                }
                for (const auto& profile : scenarios::draft21_request_response_probes()) {
                    append_profile(21, profile.definition.id, "native-quic");
                    append_profile(21, profile.definition.id, "webtransport");
                }
                for (const auto& profile : scenarios::draft21_range_filter_probes()) {
                    append_profile(21, profile.definition.id, "native-quic");
                    append_profile(21, profile.definition.id, "webtransport");
                }
                for (const unsigned draft : {18u,21u}) {
                    const auto overlaps = draft == 18 ? scenarios::draft18_discovery_overlap_probes()
                                                       : scenarios::draft21_discovery_overlap_probes();
                    std::set<std::string> seen;
                    for (const auto& profile : overlaps) if (seen.insert(profile.definition.id).second) {
                        append_profile(draft,profile.definition.id,"native-quic");
                        append_profile(draft,profile.definition.id,"webtransport");
                    }
                }
                for (const unsigned draft : {18u,21u}) {
                    const auto first_fetches = draft == 18 ? scenarios::draft18_fetch_first_object_probes()
                                                          : scenarios::draft21_fetch_first_object_probes();
                    std::set<std::string> seen;
                    for (const auto& profile : first_fetches) if (seen.insert(profile.definition.id).second) {
                        append_profile(draft,profile.definition.id,"native-quic");
                        append_profile(draft,profile.definition.id,"webtransport");
                    }
                }
                for (const unsigned draft : {18u,21u}) {
                    const auto group_orders = draft == 18 ? scenarios::draft18_fetch_group_order_probes()
                                                         : scenarios::draft21_fetch_group_order_probes();
                    for (const auto& profile : group_orders) {
                        append_profile(draft,profile.definition.id,"native-quic");
                        append_profile(draft,profile.definition.id,"webtransport");
                    }
                }
                for (const unsigned draft : {18u,21u}) {
                    const auto goaways = draft == 18 ? scenarios::draft18_request_goaway_probes()
                                                    : scenarios::draft21_request_goaway_probes();
                    for (const auto& profile : goaways) {
                        append_profile(draft,profile.definition.id,"native-quic");
                        append_profile(draft,profile.definition.id,"webtransport");
                    }
                }
                for (const unsigned draft : {18u,21u}) {
                    const auto repeated = draft == 18 ? scenarios::draft18_immutable_repeat_probes()
                                                      : scenarios::draft21_immutable_repeat_probes();
                    std::set<std::string> seen;
                    for (const auto& profile : repeated) if (seen.insert(profile.definition.id).second) {
                        append_profile(draft,profile.definition.id,"native-quic");
                        append_profile(draft,profile.definition.id,"webtransport");
                    }
                }
                for (const unsigned draft : {18u,21u}) {
                    const auto repeated = draft == 18 ? scenarios::draft18_object_repeat_probes()
                                                      : scenarios::draft21_object_repeat_probes();
                    std::set<std::string> seen;
                    for (const auto& profile : repeated) if (seen.insert(profile.definition.id).second) {
                        append_profile(draft,profile.definition.id,"native-quic");
                        append_profile(draft,profile.definition.id,"webtransport");
                    }
                }
                for (const auto& profile : scenarios::draft18_gap_a_probes()) {
                    append_profile(18, profile.definition.id, "native-quic");
                    if (!profile.native_only) append_profile(18, profile.definition.id, "webtransport");
                }
                {
                    std::set<std::string> contribution_scenarios;
                    for (const auto& profile : scenarios::draft18_contribution_probes()) {
                        if (!contribution_scenarios.insert(profile.definition.id).second) continue;
                        append_profile(18, profile.definition.id, "native-quic");
                        append_profile(18, profile.definition.id, "webtransport");
                    }
                }
                // Draft-21 gap slice A: raw probe scenarios.
                for (const auto id : app::kDraft21GapRawScenarios) {
                    if (!app::gap_webtransport_only_scenario(id)) append_profile(21, id, "native-quic");
                    if (!app::gap_native_only_scenario(id)) append_profile(21, id, "webtransport");
                }
                // Draft-21 gap slice A: announcement-controller scenarios.
                for (const auto id : app::kDraft21GapAnnouncementScenarios) {
                    if (!app::gap_webtransport_only_scenario(id)) append_profile(21, id, "native-quic");
                    if (!app::gap_native_only_scenario(id)) append_profile(21, id, "webtransport");
                }
                {
                    std::set<std::string> seen;
                    for (const auto& profile : scenarios::draft21_contribution_probes()) {
                        if (!seen.insert(profile.definition.id).second) continue;
                        append_profile(21, profile.definition.id, "native-quic");
                        append_profile(21, profile.definition.id, "webtransport");
                    }
                }
                // Draft 22, only when this server accepts draft 22 runs (its catalog is configured): every
                // executable draft 22 scenario (executable_scenarios(22): the lineage-shared ids with a draft 21
                // implementation, then the implemented own ids). A shared id is listed on the transports its draft 21 implementation's
                // profiles above list (same family, same transport limits); an own scenario is a raw probe on
                // both transports. Unscored probes are executable by id but are not listed (no catalog row
                // names them). `configured` says whether this runner's manager executes the profile now,
                // as for drafts 18 and 21.
                if (accepts_runs(app::DraftVersion::Draft22)) {
                    std::map<std::string, std::vector<std::string>> draft21_transports;
                    for (const auto& profile : profiles) {
                        if (profile.at("draft") != 21) continue;
                        draft21_transports[profile.at("scenario").get<std::string>()].push_back(
                            profile.at("transport").get<std::string>());
                    }
                    for (const auto id : app::executable_scenarios(22)) {
                        if (app::own_scenario_22(id)) {
                            append_profile(22, id, "native-quic");
                            append_profile(22, id, "webtransport");
                            continue;
                        }
                        const auto implementation = app::implementation_scenario_id(id);
                        if (!implementation) continue;
                        const auto found = draft21_transports.find(std::string(*implementation));
                        if (found == draft21_transports.end()) continue;
                        for (const auto& transport : found->second)
                            append_profile(22, id, transport.c_str());
                    }
                }
                for (auto& profile : profiles) {
                    profile["requires_fetch"] = app::scenario_requires_fetch(
                        profile.at("draft").get<unsigned>(), profile.at("scenario").get<std::string>());
                }
                const auto observed_count = profiles.size();
                for (std::size_t index = 0; index < observed_count; ++index) {
                    auto driven = profiles.at(index);
                    driven["mode"] = "driven";
                    driven["configured"] = driven.at("configured").get<bool>() &&
                        runs->supports_driven();
                    profiles.push_back(std::move(driven));
                }
                json_response(response, {{"schema_version", 1},
                                         {"status", "ok"},
                                         {"database", {{"ready", true}}},
                                         {"supported_drafts", supported_drafts()},
                                         {"executable_profiles", std::move(profiles)},
                                         {"publisher_capability_defaults",
                                          {{"fetch", config.default_publisher_capabilities.fetch}}},
                                         {"validator", detail::build_json(build)}});
            });
        });
        server.Get("/api/v1/drafts", [this](const httplib::Request&, httplib::Response& response) {
            guarded(response, [this, &response] {
                Json drafts = Json::array();
                std::vector<const requirements::RequirementCatalog*> listed = {draft18.get(), draft21.get()};
                if (config.draft22_catalog) listed.push_back(config.draft22_catalog.get());
                for (const auto* catalog : listed) {
                    auto entry = detail::catalog_json(*catalog);
                    entry["runnable"] = app::runnable(*app::parse_draft(catalog->draft));
                    drafts.push_back(std::move(entry));
                }
                json_response(response, {{"schema_version", 1}, {"drafts", std::move(drafts)}});
            });
        });
        server.Get("/api/v1/requirements", [this](const httplib::Request& request,
                                                   httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                if (!request.has_param("draft")) {
                    throw ApiError{400, "missing_draft", "draft query parameter is required."};
                }
                const auto draft = request.get_param_value("draft");
                const requirements::RequirementCatalog* catalog = nullptr;
                if (draft == "18") catalog = draft18.get();
                else if (draft == "21") catalog = draft21.get();
                else if (draft == "22") catalog = config.draft22_catalog.get();
                if (!catalog) {
                    throw ApiError{400, "unsupported_draft",
                                   config.draft22_catalog ? "draft must be 18, 21 or 22."
                                                          : "draft must be 18 or 21."};
                }
                const auto page = query(request);
                Json items = Json::array();
                const auto begin = std::min(page.offset, catalog->requirements.size());
                const auto count = std::min(page.limit, catalog->requirements.size() - begin);
                for (std::size_t index = begin; index < begin + count; ++index) {
                    items.push_back(detail::requirement_json(catalog->requirements[index]));
                }
                const auto next = begin + count < catalog->requirements.size()
                                      ? std::optional<std::size_t>(begin + count) : std::nullopt;
                json_response(response, {{"schema_version", 1}, {"draft", detail::catalog_draft_json(catalog->draft)},
                    {"pagination", detail::pagination_json(page.limit, page.offset,
                                                            catalog->requirements.size(), next)},
                    {"items", std::move(items)}});
            });
        });
        server.Post("/api/v1/runs", [this](const httplib::Request& request,
                                            httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                const auto requested = parse_run_config(request, config.default_publisher_capabilities);
                if (!accepts_runs(requested.draft))
                    throw ApiError{422, "draft_not_runnable",
                        "Draft " + detail::draft_display(requested.draft) +
                        " is not runnable on this runner."};
                {
                    // Say which part of the selection is unsupported, and why, so the caller
                    // does not have to bisect a long scenario list.
                    const auto draft_number = static_cast<unsigned>(requested.draft);
                    for (const auto& id : requested.scenario_ids) {
                        if (!app::executable_scenario(draft_number, id))
                            throw ApiError{422, "unsupported_run_config",
                                "Scenario '" + id + "' is not an executable scenario for draft " +
                                std::to_string(draft_number) + "."};
                    }
                    if (requested.scenario_ids.size() > 1) {
                        for (const auto& id : requested.scenario_ids) {
                            if (!app::raw_probe_scenario(draft_number, id))
                                throw ApiError{422, "unsupported_run_config",
                                    "Scenario '" + id + "' is a typed scenario and cannot be "
                                    "combined with other scenarios in one run; only raw-probe "
                                    "scenarios can be selected together. Run it on its own."};
                        }
                    }
                    if (requested.mode == app::RunMode::Driven && runs && !runs->supports_driven())
                        throw ApiError{422, "unsupported_run_config",
                            "Driven mode is not configured on this runner; start it with "
                            "--driver-executable or use observed mode."};
                    if (runs && !runs->supports(requested.draft))
                        throw ApiError{422, "unsupported_run_config",
                            "Draft " + std::to_string(draft_number) +
                            " is not supported by this runner's publisher listener."};
                }
                {
                    // Every selected scenario needs a capability the publisher declared absent:
                    // nothing could run, so name the first one and the capability.
                    const auto draft = static_cast<unsigned>(requested.draft);
                    if (std::all_of(requested.scenario_ids.begin(), requested.scenario_ids.end(), [&](const auto& id) {
                            return app::scenario_skip_reason(draft, id, requested.publisher_capabilities).has_value();
                        })) {
                        const auto& id = requested.scenario_ids.front();
                        throw ApiError{422, "scenario_requires_publisher_capability",
                                       "Scenario " + id + " requires the publisher capability " +
                                       std::string(app::scenario_required_capability(draft, id).value_or("unknown")) +
                                       ", which this run declares the publisher does not implement."};
                    }
                }
                if ((!requested.track_fixture && std::any_of(requested.scenario_ids.begin(),requested.scenario_ids.end(),[&](const auto& id) {
                        return app::scenario_requires_track(static_cast<unsigned>(requested.draft),id);
                    })) ||
                    (requested.mode == app::RunMode::Driven && !requested.track_fixture) ||
                    requested.timeout < std::chrono::milliseconds(2)) {
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
                    {
                    Json publisher_endpoint = {
                        {"address", started.endpoint.address},
                        {"port", started.endpoint.port},
                        {"alpn", requested.transport == app::TransportKind::WebTransport
                                     ? "h3" : std::string(app::alpn(requested.draft))}};
                    if (requested.transport == app::TransportKind::WebTransport) {
                        publisher_endpoint["url"] = started.url;
                        publisher_endpoint["path"] = started.path;
                        publisher_endpoint["protocol"] = started.protocol;
                    }
                    json_response(response, {{"schema_version", 1},
                        {"run", detail::run_json(store->load(started.id))},
                        {"publisher_endpoint", std::move(publisher_endpoint)}}, 201);
                    return;
                    }
                case app::RunStartStatus::Unsupported:
                    throw ApiError{422, "unsupported_run_config", "This run configuration is not executable."};
                case app::RunStartStatus::InvalidConfig:
                    throw ApiError{400, "invalid_run_config", "Run configuration is invalid."};
                case app::RunStartStatus::PortExhausted:
                    throw ApiError{503, "publisher_ports_exhausted", "No publisher listener port is available."};
                case app::RunStartStatus::ListenerError:
                    throw ApiError{503, "publisher_listener_unavailable", "The publisher listener could not start."};
                case app::RunStartStatus::ScenarioRequiresCapability:
                    throw ApiError{422, "scenario_requires_publisher_capability",
                                   "Scenario " + started.scenario + " requires the publisher capability " +
                                   started.capability + ", which this run declares the publisher does not implement."};
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
        server.Get("/results/completeness.json", [this](const httplib::Request&,
                                                        httplib::Response& response) {
            guarded(response, [this, &response] {
                json_response(response, completeness_json(*draft18, *draft21, config.draft22_catalog.get(),
                                                          *store, build));
            });
        });
        server.Get(R"(/results/(.+)\.json)", [this](const httplib::Request& request,
                                                      httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                try {
                    const auto run = store->load(request.matches[1]);
                    const auto& catalog = catalog_for(run.config.draft);
                    json_response(response, serialize_result(run, catalog));
                } catch (const std::out_of_range&) {
                    throw ApiError{404, "run_not_found", "The requested run was not found."};
                }
            });
        });
        server.Get(R"(/results/(.+)\.tap)", [this](const httplib::Request& request,
                                                     httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                try {
                    const auto run = store->load(request.matches[1]);
                    const auto& catalog = catalog_for(run.config.draft);
                    response.status = 200;
                    response.set_header("X-Content-Type-Options", "nosniff");
                    response.set_content(serialize_tap14(run, catalog),
                                         "text/plain; charset=utf-8");
                } catch (const std::out_of_range&) {
                    throw ApiError{404, "run_not_found", "The requested run was not found."};
                }
            });
        });
        server.Get(R"(/results/(.+))", [this](const httplib::Request& request,
                                               httplib::Response& response) {
            guarded(response, [this, &request, &response] {
                const auto filters = report_filters(request);
                try {
                    const auto run = store->load(request.matches[1]);
                    const auto& catalog = catalog_for(run.config.draft);
                    response.status = 200;
                    html_headers(response);
                    response.set_content(detail::render_run_detail(run, catalog, filters),
                                         "text/html; charset=utf-8");
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
                response.set_content(detail::render_run_list(
                    runs.items, completeness_json(*draft18, *draft21, config.draft22_catalog.get(), *store, build)),
                    "text/html; charset=utf-8");
            });
        });
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.status == 404 && response.body.empty()) {
                error_response(response, {404, "not_found", "The requested resource was not found."});
            }
        });
    }

    // Whether POST /api/v1/runs takes runs for `draft`: it must be runnable and this server must have its
    // catalog to present and score them (a runner without the draft 22 catalog refuses draft 22 runs).
    // /healthz lists exactly these drafts as supported_drafts.
    bool accepts_runs(app::DraftVersion draft) const {
        if (!app::runnable(draft)) return false;
        switch (draft) {
            case app::DraftVersion::Draft18: return true;
            case app::DraftVersion::Draft21: return true;
            case app::DraftVersion::Draft22: return config.draft22_catalog != nullptr;
            case app::DraftVersion::MoqLite06: return false;  // not runnable yet; refused as draft_not_runnable
        }
        return false;
    }

    Json supported_drafts() const {
        Json drafts = Json::array();
        for (const auto draft : {app::DraftVersion::Draft18, app::DraftVersion::Draft21, app::DraftVersion::Draft22})
            if (accepts_runs(draft)) drafts.push_back(app::draft_number(draft));
        return drafts;
    }

    // The catalog a stored run is presented with, chosen by the run's (wire) draft. A stored draft 22 run on a
    // runner without the draft 22 catalog is a clear conflict, never an internal error.
    const requirements::RequirementCatalog& catalog_for(app::DraftVersion draft) const {
        switch (draft) {
            case app::DraftVersion::Draft18: return *draft18;
            case app::DraftVersion::Draft21: return *draft21;
            case app::DraftVersion::Draft22:
                if (config.draft22_catalog) return *config.draft22_catalog;
                break;
            case app::DraftVersion::MoqLite06: break;  // no moq-lite catalog is configured yet
        }
        const auto number = detail::draft_display(draft);
        throw ApiError{409, "draft_catalog_not_configured",
                       "Draft " + number + " results need the draft " + number +
                       " catalog, which this runner does not have configured."};
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
