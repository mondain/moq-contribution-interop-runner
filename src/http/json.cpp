#include "detail.h"

#include <nlohmann/json.hpp>

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
    return {{"draft", static_cast<unsigned>(config.draft)},
            {"transport", name(config.transport)},
            {"mode", name(config.mode)},
            {"scenarios", config.scenario_ids},
            {"timeout_ms", config.timeout.count()}};
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
    return {{"draft", catalog.draft},
            {"source_sha256", catalog.source_sha256},
            {"complete", catalog.complete},
            {"requirement_count", catalog.requirements.size()},
            {"publisher_relevant_count", publisher_relevant},
            {"applicability_counts", applicability},
            {"testability_counts", testability}};
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
    return {{"sequence", event.sequence},
            {"monotonic_time_ns", event.monotonic_time_ns},
            {"wall_time_unix_ns", event.wall_time_unix_ns},
            {"kind", event.kind},
            {"detail", event.detail},
            {"connection_id", event.connection_id},
            {"stream_id", event.stream_id},
            {"request_id", event.request_id},
            {"scenario_id", event.scenario_id},
            {"requirement_id", event.requirement_id}};
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
    Json outcomes = Json::array();
    for (const auto& outcome : run.outcomes) {
        outcomes.push_back({{"requirement_id", outcome.requirement_id},
                            {"state", name(outcome.state)}});
    }
    return {{"id", run.id},
            {"config", config_json(run.config)},
            {"build", build_json(run.build)},
            {"state", name(run.state)},
            {"created_at_unix_ns", run.created_at_unix_ns},
            {"finalized_at_unix_ns", run.finalized_at_unix_ns},
            {"verdict", run.score ? Json(name(run.score->verdict)) : Json(nullptr)},
            {"score", run.score ? score_json(*run.score) : Json(nullptr)},
            {"outcomes", std::move(outcomes)},
            {"events", {{"total", run.events.size()},
                         {"href", "/api/v1/runs/" + run.id + "/events"}}}};
}

Json pagination_json(std::size_t limit, std::size_t offset, std::size_t total,
                     const std::optional<std::size_t>& next_offset) {
    return {{"limit", limit}, {"offset", offset}, {"total", total},
            {"next_offset", next_offset ? Json(*next_offset) : Json(nullptr)}};
}

}  // namespace moq::interop::http::detail
