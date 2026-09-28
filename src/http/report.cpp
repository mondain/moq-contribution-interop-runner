#include "detail.h"

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

}  // namespace

std::string render_run_list(std::span<const storage::RunSummary> runs) {
    std::ostringstream output;
    output << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>MoQ contribution interop results</title><style>"
              "body{background:#fff;color:#17212b;font-family:sans-serif;margin:2rem}"
              "table{border-collapse:collapse;width:100%}th,td{border:1px solid #6b7280;padding:.5rem;text-align:left}"
              "th{background:#dbeafe}.status{font-weight:bold}.PASS{color:#116329}.FAIL,.ERROR{color:#a40000}"
              ".PENDING,.INCOMPLETE{color:#704b00}</style></head><body>"
              "<main><h1>MoQ contribution interop results</h1>";
    if (runs.empty()) {
        output << "<p>No runs have been created.</p>";
    } else {
        output << "<table><caption>Newest runs first</caption><thead><tr><th>Run</th><th>State</th>"
                  "<th>Verdict</th><th>Draft</th><th>Transport</th><th>Mode</th><th>Scenarios</th>"
                  "<th>Required</th><th>Weighted</th><th>Coverage</th></tr></thead><tbody>";
        for (const auto& run : runs) {
            const auto verdict = verdict_name(run);
            output << "<tr><td><a href=\"/results/" << escape_html(run.id) << ".json\">"
                   << escape_html(run.id) << "</a></td><td>"
                   << (run.state == storage::RunState::Active ? "ACTIVE" : "FINALIZED")
                   << "</td><td class=\"status " << verdict << "\">" << verdict
                   << "</td><td>" << static_cast<unsigned>(run.config.draft) << "</td><td>"
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

}  // namespace moq::interop::http::detail
