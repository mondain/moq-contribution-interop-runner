#include "detail.h"
#include "json.h"
#include "moq/interop/app/unscored_probe_event_22.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string_view>

namespace moq::interop::http::detail {
namespace {

using Json = nlohmann::json;

template <typename Enum>
std::string_view name(Enum value);

template <>
std::string_view name(requirements::Strength value) {
    switch (value) {
        case requirements::Strength::Must: return "MUST";
        case requirements::Strength::MustNot: return "MUST NOT";
        case requirements::Strength::Should: return "SHOULD";
        case requirements::Strength::ShouldNot: return "SHOULD NOT";
        case requirements::Strength::May: return "MAY";
    }
    return "unknown";
}

template <>
std::string_view name(requirements::Applicability value) {
    switch (value) {
        case requirements::Applicability::Applicable: return "applicable";
        case requirements::Applicability::NotApplicable: return "not_applicable";
        case requirements::Applicability::Informative: return "informative";
    }
    return "unknown";
}

template <>
std::string_view name(requirements::Testability value) {
    switch (value) {
        case requirements::Testability::Testable: return "testable";
        case requirements::Testability::NotTestable: return "not_testable";
        case requirements::Testability::NotApplicable: return "not_applicable";
    }
    return "unknown";
}

template <>
std::string_view name(app::TransportKind value) {
    return value == app::TransportKind::NativeQuic ? "native-quic" : "webtransport";
}

template <>
std::string_view name(app::RunMode value) {
    return value == app::RunMode::Observed ? "observed" : "driven";
}

template <>
std::string_view name(storage::RunState value) {
    return value == storage::RunState::Active ? "active" : "finalized";
}

template <>
std::string_view name(requirements::RunVerdict value) {
    switch (value) {
        case requirements::RunVerdict::Pass: return "pass";
        case requirements::RunVerdict::Fail: return "fail";
        case requirements::RunVerdict::Incomplete: return "incomplete";
        case requirements::RunVerdict::Error: return "error";
    }
    return "unknown";
}

template <>
std::string_view name(requirements::OutcomeState value) {
    switch (value) {
        case requirements::OutcomeState::Pass: return "pass";
        case requirements::OutcomeState::Fail: return "fail";
        case requirements::OutcomeState::NotRun: return "not_run";
        case requirements::OutcomeState::NotTestable: return "not_testable";
        case requirements::OutcomeState::NotApplicable: return "not_applicable";
    }
    return "unknown";
}

Json ratio_json(const requirements::ScoreRatio& ratio) {
    return {{"earned", ratio.earned}, {"possible", ratio.possible}};
}

Json score_json(const requirements::ScoreSummary& score) {
    return {{"verdict", name(score.verdict)},
            {"required", ratio_json(score.required)},
            {"weighted", ratio_json(score.weighted)},
            {"coverage", ratio_json(score.coverage)}};
}

Json config_json(const app::RunConfig& config) {
    Json result = {{"draft", draft_json(config.draft)},
            {"transport", name(config.transport)},
            {"mode", name(config.mode)},
            {"scenarios", config.scenario_ids},
            {"timeout_ms", config.timeout.count()},
            {"publisher_capabilities", {{"fetch", config.publisher_capabilities.fetch}}}};
    if (config.track_fixture) {
        const auto encode_hex = [](std::string_view bytes) {
            constexpr char digits[] = "0123456789abcdef";
            std::string encoded;
            encoded.reserve(bytes.size() * 2);
            for (const unsigned char byte : bytes) {
                encoded.push_back(digits[byte >> 4]);
                encoded.push_back(digits[byte & 0x0f]);
            }
            return encoded;
        };
        Json fields = Json::array();
        for (const auto& field : config.track_fixture->namespace_fields) {
            fields.push_back(encode_hex(field));
        }
        result["track"] = {{"namespace_hex", std::move(fields)},
                           {"name_hex", encode_hex(config.track_fixture->track_name)}};
    }
    return result;
}

}  // namespace

Json error_json(const ApiError& error) {
    return {{"schema_version", 1},
            {"error", {{"status", error.status}, {"code", error.code}, {"message", error.message}}}};
}

Json build_json(const app::BuildInfo& build) {
    return {{"version", build.version},
            {"source_revision", build.source_revision},
            {"dependencies", build.dependencies}};
}

Json catalog_json(const requirements::RequirementCatalog& catalog) {
    std::map<std::string, std::size_t> applicability;
    std::map<std::string, std::size_t> testability;
    std::size_t publisher_relevant = 0;
    for (const auto& requirement : catalog.requirements) {
        ++applicability[std::string(name(requirement.applicability))];
        ++testability[std::string(name(requirement.testability))];
        if (requirement.applicability == requirements::Applicability::Applicable) {
            ++publisher_relevant;
        }
    }
    Json result = {{"draft", catalog_draft_json(catalog.draft)},
            {"source_sha256", catalog.source_sha256},
            {"complete", catalog.complete},
            {"requirement_count", catalog.requirements.size()},
            {"publisher_relevant_count", publisher_relevant},
            {"applicability_counts", applicability},
            {"testability_counts", testability}};
    // Only a staged (moq-lite) catalog carries the staged fields, so the MoQ Transport entries are unchanged.
    if (staged_catalog(catalog)) {
        result["staged"] = true;
        result["staged_note"] = kStagedNote;
    }
    return result;
}

Json requirement_json(const requirements::Requirement& requirement) {
    return {{"id", requirement.id},
            {"strength", name(requirement.strength)},
            {"source", {{"section", requirement.source.section},
                         {"first_line", requirement.source.first_line},
                         {"last_line", requirement.source.last_line},
                         {"occurrence", requirement.source.occurrence},
                         {"clause", requirement.source.clause}}},
            {"actor", requirement.actor},
            {"summary", requirement.summary},
            {"applicability", name(requirement.applicability)},
            {"testability", name(requirement.testability)},
            {"scenarios", requirement.scenarios},
            {"evaluators", requirement.evaluators},
            {"rationale", requirement.rationale}};
}

Json event_json(const storage::EvidenceEvent& event) {
    Json result = {{"sequence", event.sequence},
            {"monotonic_time_ns", event.monotonic_time_ns},
            {"wall_time_unix_ns", event.wall_time_unix_ns},
            {"kind", event.kind},
            {"detail", event.detail},
            {"connection_id", event.connection_id},
            {"stream_id", event.stream_id},
            {"request_id", event.request_id},
            {"scenario_id", event.scenario_id},
            {"requirement_id", event.requirement_id}};
    // An unscored draft 22 probe's verdict, read back from its detail as structured fields (only this kind
    // carries them, so every other event, and every draft 18/21 event, is unchanged).
    if (event.kind == app::kUnscoredProbeVerdictEvent) {
        if (const auto fields = app::parse_unscored_probe_detail_22(event.detail)) {
            result["verdict"] = fields->verdict;
            result["reason"] = fields->reason;
        }
    }
    return result;
}

Json run_summary_json(const storage::RunSummary& run) {
    return {{"id", run.id},
            {"config", config_json(run.config)},
            {"state", name(run.state)},
            {"created_at_unix_ns", run.created_at_unix_ns},
            {"finalized_at_unix_ns", run.finalized_at_unix_ns},
            {"verdict", run.verdict ? Json(name(*run.verdict)) : Json(nullptr)},
            {"score", run.score ? score_json(*run.score) : Json(nullptr)}};
}

Json run_json(const storage::RunRecord& run) {
    // Why the run, or one of its contexts, did not end normally. Built from the evidence the
    // run recorded, so the reason an operator sees is the one the harness stored.
    Json error_reasons = Json::array();
    Json truncated_contexts = Json::array();
    for (const auto& event : run.events) {
        const Json scenario = event.scenario_id ? Json(*event.scenario_id) : Json(nullptr);
        if (event.kind == "harness_error" || event.kind == "run_aborted" || event.kind == "run_stopped")
            error_reasons.push_back({{"scenario_id", scenario}, {"kind", event.kind}, {"detail", event.detail}});
        else if (event.kind == "context_event_limit")
            truncated_contexts.push_back({{"scenario_id", scenario}, {"detail", event.detail}});
    }
    Json run_error_reason = nullptr;
    if (run.score && run.score->verdict == requirements::RunVerdict::Error)
        run_error_reason = error_reasons.empty()
            ? Json("the run ended with an error and recorded no reason")
            : error_reasons.front().at("detail");
    Json outcomes = Json::array();
    for (const auto& outcome : run.outcomes) {
        outcomes.push_back({{"requirement_id", outcome.requirement_id},
                            {"state", name(outcome.state)}});
    }
    Json result = {{"id", run.id},
            {"config", config_json(run.config)},
            {"build", build_json(run.build)},
            {"state", name(run.state)},
            {"created_at_unix_ns", run.created_at_unix_ns},
            {"finalized_at_unix_ns", run.finalized_at_unix_ns},
            {"verdict", run.score ? Json(name(run.score->verdict)) : Json(nullptr)},
            {"score", run.score ? score_json(*run.score) : Json(nullptr)},
            {"run_error_reason", std::move(run_error_reason)},
            {"error_reasons", std::move(error_reasons)},
            {"truncated_contexts", std::move(truncated_contexts)},
            {"scoring_profile", std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "compatibility_error_mapping";
            }) ? "compatibility" : "standards"},
            {"outcomes", std::move(outcomes)},
            {"events", {{"total", run.events.size()},
                         {"href", "/api/v1/runs/" + run.id + "/events"}}}};
    // A moq-lite run is staged: its verdict is never pass (kStagedNote). MoQ Transport runs are unchanged.
    if (staged_draft(run.config.draft)) {
        result["staged"] = true;
        result["staged_note"] = kStagedNote;
    }
    return result;
}

Json pagination_json(std::size_t limit, std::size_t offset, std::size_t total,
                     const std::optional<std::size_t>& next_offset) {
    return {{"limit", limit}, {"offset", offset}, {"total", total},
            {"next_offset", next_offset ? Json(*next_offset) : Json(nullptr)}};
}

}  // namespace moq::interop::http::detail
