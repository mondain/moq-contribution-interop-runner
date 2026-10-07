#include "json.h"
#include "detail.h"
#include "moq/interop/http/result_schema.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <string_view>

namespace moq::interop::http::detail {
namespace {

std::string escape_html(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char character : value) {
        switch (character) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            case '\0': escaped += "&#0;"; break;
            default: escaped.push_back(static_cast<char>(character));
        }
    }
    return escaped;
}

std::string transport_name(app::TransportKind value) {
    return value == app::TransportKind::NativeQuic ? "native-quic" : "webtransport";
}

std::string mode_name(app::RunMode value) {
    return value == app::RunMode::Observed ? "observed" : "driven";
}

std::string verdict_name(const storage::RunSummary& run) {
    if (!run.verdict) return "PENDING";
    switch (*run.verdict) {
        case requirements::RunVerdict::Pass: return "PASS";
        case requirements::RunVerdict::Fail: return "FAIL";
        case requirements::RunVerdict::Incomplete: return "INCOMPLETE";
        case requirements::RunVerdict::Error: return "ERROR";
    }
    return "UNKNOWN";
}

std::string ratio(const std::optional<requirements::ScoreSummary>& score,
                  requirements::ScoreRatio requirements::ScoreSummary::* member) {
    if (!score) return "0/0";
    const auto& value = (*score).*member;
    return std::to_string(value.earned) + "/" + std::to_string(value.possible);
}

std::string outcome_name(const nlohmann::json& row) {
    return row.at("outcome").is_null()
               ? "unobserved" : row.at("outcome").get<std::string>();
}

bool matches(const nlohmann::json& row, const ReportFilters& filters) {
    if (!filters.strength.empty() &&
        row.at("strength").get<std::string>() != filters.strength)
        return false;
    if (!filters.outcome.empty() && outcome_name(row) != filters.outcome)
        return false;
    if (!filters.section.empty() &&
        row.at("source").at("section").get<std::string>().rfind(filters.section, 0) != 0)
        return false;
    if (!filters.scenario.empty()) {
        const auto& scenarios = row.at("scenarios");
        if (std::find(scenarios.begin(), scenarios.end(), filters.scenario) ==
            scenarios.end())
            return false;
    }
    return true;
}

std::string ratio(const nlohmann::json& score, std::string_view key) {
    if (score.is_null()) return "0/0";
    const auto& value = score.at(std::string(key));
    return std::to_string(value.at("earned").get<std::uint64_t>()) + "/" +
           std::to_string(value.at("possible").get<std::uint64_t>());
}

}  // namespace

std::string render_run_list(std::span<const storage::RunSummary> runs,
                            const nlohmann::json& completeness) {
    std::ostringstream output;
    output << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>MoQ contribution interop results</title><style>"
           << report_styles() << "</style></head><body>"
              "<main><h1>MoQ contribution interop results</h1>"
              "<h2>Validator completeness</h2><p>Executable evaluator coverage is "
              "not publisher conformance. <a href=\"/results/completeness.json\">"
              "Download completeness JSON</a>.</p><table><caption>Static coverage and "
              "observed runs by draft and transport</caption><thead><tr>"
              "<th scope=\"col\">Draft</th><th scope=\"col\">Transport</th>"
              "<th scope=\"col\">Required evaluator coverage</th>"
              "<th scope=\"col\">Optional evaluator coverage</th>"
              "<th scope=\"col\">Runs</th><th scope=\"col\">Observed requirements</th>"
              "</tr></thead><tbody>";
    for (const auto& draft : completeness.at("drafts")) {
        for (const auto& transport : draft.at("transports")) {
            output << "<tr><td>" << draft.at("draft").get<unsigned>() << "</td><td>"
                   << escape_html(transport.at("transport").get<std::string>())
                   << "</td><td>" << draft.at("required_covered").get<std::size_t>()
                   << "/" << draft.at("required_total").get<std::size_t>()
                   << "</td><td>" << draft.at("optional_covered").get<std::size_t>()
                   << "/" << draft.at("optional_total").get<std::size_t>()
                   << "</td><td>" << transport.at("run_count").get<std::size_t>()
                   << "</td><td>"
                   << transport.at("observed_requirement_count").get<std::size_t>()
                   << "</td></tr>";
        }
    }
    output << "</tbody></table><h2>Runs</h2>";
    if (runs.empty()) {
        output << "<p>No runs have been created.</p>";
    } else {
        output << "<table><caption>Newest runs first</caption><thead><tr>"
                  "<th scope=\"col\">Run</th><th scope=\"col\">State</th>"
                  "<th scope=\"col\">Verdict</th><th scope=\"col\">Draft</th>"
                  "<th scope=\"col\">Transport</th><th scope=\"col\">Mode</th>"
                  "<th scope=\"col\">Scenarios</th><th scope=\"col\">Required</th>"
                  "<th scope=\"col\">Weighted</th><th scope=\"col\">Coverage</th>"
                  "</tr></thead><tbody>";
        for (const auto& run : runs) {
            const auto verdict = verdict_name(run);
            output << "<tr><th scope=\"row\"><a href=\"/results/" << escape_html(run.id) << "\">"
                   << escape_html(run.id) << "</a></th><td>"
                   << (run.state == storage::RunState::Active ? "ACTIVE" : "FINALIZED")
                   << "</td><td class=\"status " << verdict << "\">" << verdict
                   << "</td><td>" << detail::draft_display(run.config.draft) << "</td><td>"
                   << escape_html(transport_name(run.config.transport)) << "</td><td>"
                   << escape_html(mode_name(run.config.mode)) << "</td><td>";
            for (std::size_t i = 0; i < run.config.scenario_ids.size(); ++i) {
                if (i != 0) output << ", ";
                output << escape_html(run.config.scenario_ids[i]);
            }
            output << "</td><td>" << ratio(run.score, &requirements::ScoreSummary::required)
                   << "</td><td>" << ratio(run.score, &requirements::ScoreSummary::weighted)
                   << "</td><td>" << ratio(run.score, &requirements::ScoreSummary::coverage)
                   << "</td></tr>";
        }
        output << "</tbody></table>";
    }
    output << "</main></body></html>";
    return output.str();
}

std::string render_run_detail(const storage::RunRecord& run,
                              const requirements::RequirementCatalog& catalog,
                              const ReportFilters& filters) {
    const auto document = serialize_result(run, catalog);
    std::map<std::uint64_t, nlohmann::json> evidence;
    for (const auto& event : document.at("evidence"))
        evidence[event.at("sequence").get<std::uint64_t>()] = event;
    std::ostringstream output;
    output << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>MoQ run " << escape_html(run.id) << "</title><style>"
           << report_styles() << "</style></head><body><main>"
              "<nav><a href=\"/results\">All runs</a></nav><h1>Run "
           << escape_html(run.id) << "</h1><p>Draft "
           << (app::parse_draft(catalog.draft) ? detail::draft_display(*app::parse_draft(catalog.draft)) : std::to_string(catalog.draft)) << "; transport "
           << escape_html(transport_name(run.config.transport)) << "; mode "
           << escape_html(mode_name(run.config.mode)) << "; state "
           << (run.state == storage::RunState::Active ? "ACTIVE" : "FINALIZED")
           << ".</p><p><a href=\"/results/" << escape_html(run.id)
           << ".json\">Download JSON</a> · <a href=\"/results/"
           << escape_html(run.id) << ".tap\">Download TAP 14</a></p>";
    if (document.at("run").at("scoring_profile") == "compatibility")
        output << "<p><strong>Compatibility scoring:</strong> UNKNOWN_AUTH_TOKEN_ALIAS uses an explicitly configured REQUEST_ERROR code. The checked-in draft does not assign that request code.</p>";
    if (!run.config.publisher_capabilities.fetch) {
        // Visible text, not colour: the declaration, the scenarios it skipped and the rows it excluded.
        output << "<h2>Publisher capabilities</h2><p><strong>FETCH: not implemented.</strong> "
                  "This run declares that the publisher does not implement FETCH, which the draft "
                  "allows for an endpoint that is not a relay. Scenarios that need FETCH were not "
                  "started, and catalog rows whose every named scenario needs FETCH are reported "
                  "as not_applicable and left out of the required, weighted and coverage scores.</p>";
        const auto& skipped = document.at("skipped_scenarios");
        if (skipped.empty()) {
            output << "<p>No selected scenario needed FETCH.</p>";
        } else {
            output << "<table><caption>Scenarios skipped by the declaration</caption><thead><tr>"
                      "<th scope=\"col\">Scenario</th><th scope=\"col\">Reason</th></tr></thead><tbody>";
            for (const auto& entry : skipped)
                output << "<tr><th scope=\"row\"><code>"
                       << escape_html(entry.at("scenario_id").get<std::string>()) << "</code></th><td>"
                       << escape_html(entry.at("reason").get<std::string>()) << "</td></tr>";
            output << "</tbody></table>";
        }
    }
    const auto& score = document.at("run").at("score");
    const auto verdict = document.at("run").at("verdict").is_null()
                             ? "PENDING"
                             : document.at("run").at("verdict").get<std::string>();
    output << "<h2>Scores</h2><p>Verdict: <strong class=\"status\">"
           << escape_html(verdict) << "</strong>; Required score: "
           << ratio(score, "required") << "; Weighted score: "
           << ratio(score, "weighted") << "; Coverage: "
           << ratio(score, "coverage") << ".</p>";
    const auto& run_document = document.at("run");
    if (!run_document.at("run_error_reason").is_null())
        output << "<p><strong>Run error:</strong> "
               << escape_html(run_document.at("run_error_reason").get<std::string>()) << "</p>";
    if (!run_document.at("error_reasons").empty()) {
        output << "<table><caption>Why contexts or the run ended with an error</caption><thead><tr>"
                  "<th scope=\"col\">Scenario</th><th scope=\"col\">Kind</th>"
                  "<th scope=\"col\">Reason</th></tr></thead><tbody>";
        for (const auto& entry : run_document.at("error_reasons"))
            output << "<tr><th scope=\"row\"><code>"
                   << escape_html(entry.at("scenario_id").is_null() ? std::string{}
                                                                     : entry.at("scenario_id").get<std::string>())
                   << "</code></th><td>" << escape_html(entry.at("kind").get<std::string>()) << "</td><td>"
                   << escape_html(entry.at("detail").get<std::string>()) << "</td></tr>";
        output << "</tbody></table>";
    }
    if (!run_document.at("truncated_contexts").empty()) {
        output << "<table><caption>Contexts whose evidence was truncated and are not scored</caption>"
                  "<thead><tr><th scope=\"col\">Scenario</th><th scope=\"col\">Reason</th></tr></thead><tbody>";
        for (const auto& entry : run_document.at("truncated_contexts"))
            output << "<tr><th scope=\"row\"><code>"
                   << escape_html(entry.at("scenario_id").is_null() ? std::string{}
                                                                     : entry.at("scenario_id").get<std::string>())
                   << "</code></th><td>" << escape_html(entry.at("detail").get<std::string>()) << "</td></tr>";
        output << "</tbody></table>";
    }
    if (document.contains("unscored_probes")) {
        output << "<table><caption>Unscored probes (verdicts recorded as evidence; not part of any score)</caption>"
                  "<thead><tr><th scope=\"col\">Scenario</th><th scope=\"col\">Verdict</th>"
                  "<th scope=\"col\">Reason</th></tr></thead><tbody>";
        for (const auto& entry : document.at("unscored_probes"))
            output << "<tr><th scope=\"row\"><code>"
                   << escape_html(entry.at("scenario_id").is_null() ? std::string{}
                                                                     : entry.at("scenario_id").get<std::string>())
                   << "</code></th><td>" << escape_html(entry.at("verdict").get<std::string>()) << "</td><td>"
                   << escape_html(entry.at("reason").get<std::string>()) << "</td></tr>";
        output << "</tbody></table>";
    }
    output << "<form method=\"get\" action=\"/results/" << escape_html(run.id)
           << "\"><fieldset><legend>Filter requirements</legend>"
              "<label>Strength <input name=\"strength\" value=\""
           << escape_html(filters.strength)
           << "\"></label><label>Outcome <input name=\"outcome\" value=\""
           << escape_html(filters.outcome)
           << "\"></label><label>Section <input name=\"section\" value=\""
           << escape_html(filters.section)
           << "\"></label><label>Scenario <input name=\"scenario\" value=\""
           << escape_html(filters.scenario)
           << "\"></label><button type=\"submit\">Apply filters</button>"
              "</fieldset></form>";

    std::size_t shown = 0;
    std::ostringstream rows;
    for (const auto& row : document.at("requirements")) {
        if (!matches(row, filters)) continue;
        ++shown;
        const auto status = outcome_name(row);
        rows << "<tr><th scope=\"row\"><code>" << escape_html(row.at("id").get<std::string>())
             << "</code></th><td>" << escape_html(row.at("strength").get<std::string>())
             << "</td><td>" << escape_html(row.at("source").at("section").get<std::string>())
             << " (lines " << row.at("source").at("first_line").get<std::size_t>()
             << "–" << row.at("source").at("last_line").get<std::size_t>()
             << ")</td><td>" << escape_html(row.at("summary").get<std::string>())
             << "</td><td>" << escape_html(row.at("applicability").get<std::string>())
             << "; " << escape_html(row.at("testability").get<std::string>())
             << "</td><td>" << row.at("weight").get<std::uint64_t>()
             << (row.at("score_eligible").get<bool>() ? " eligible" : " excluded")
             << "</td><td><strong>" << escape_html(status) << "</strong></td><td>"
             << escape_html(row.at("rationale").get<std::string>());
        if (!row.at("not_applicable_reason").is_null())
            rows << "<p><strong>Not applicable to this run:</strong> "
                 << escape_html(row.at("not_applicable_reason").get<std::string>()) << "</p>";
        rows << "</td><td><details><summary>Evidence ("
             << row.at("evidence_sequences").size() << ")</summary>";
        if (row.at("evidence_sequences").empty()) {
            rows << "<p>No evidence recorded.</p>";
        } else {
            rows << "<ul>";
            for (const auto sequence : row.at("evidence_sequences")) {
                const auto found = evidence.find(sequence.get<std::uint64_t>());
                if (found == evidence.end()) continue;
                rows << "<li><code>" << escape_html(found->second.at("kind").get<std::string>())
                     << "</code>: " << escape_html(found->second.at("detail").get<std::string>())
                     << "</li>";
            }
            rows << "</ul>";
        }
        rows << "</details></td></tr>";
    }
    output << "<h2>Requirements</h2><p>Rows shown: " << shown
           << " of " << document.at("requirements").size() << "</p>"
              "<table><caption>Draft requirement outcomes</caption><thead><tr>"
              "<th scope=\"col\">Requirement</th><th scope=\"col\">Strength</th>"
              "<th scope=\"col\">Section</th><th scope=\"col\">Expected behavior</th>"
              "<th scope=\"col\">Applicability</th><th scope=\"col\">Weight</th>"
              "<th scope=\"col\">Outcome</th><th scope=\"col\">Rationale</th>"
              "<th scope=\"col\">Evidence</th></tr></thead><tbody>"
           << rows.str() << "</tbody></table></main></body></html>";
    return output.str();
}

}  // namespace moq::interop::http::detail
