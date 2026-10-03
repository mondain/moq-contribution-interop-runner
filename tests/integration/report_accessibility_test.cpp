#include "detail.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace moq::interop::http::detail {
namespace {

TEST(ReportAccessibility, EscapesCatalogAndEvidenceTextWithoutScript) {
    requirements::RequirementCatalog catalog{
        18, "digest", true,
        {{"R&1", requirements::Strength::Must,
          {"5.1", 10, 12, 1, 1},
          "publisher", "<img src=x onerror=alert(1)>",
          requirements::Applicability::Applicable,
          requirements::Testability::Testable,
          {"scene<script>"}, {"evaluator"}, R"(expected "safe" & observed <bad>)"}}};
    storage::RunRecord run;
    run.id = "run-1";
    run.config = {app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
                  app::RunMode::Observed, {"scene<script>"},
                  std::chrono::milliseconds{1000}};
    run.build = {"0.1", "rev", {}};
    run.state = storage::RunState::Finalized;
    run.created_at_unix_ns = 1;
    run.finalized_at_unix_ns = 2;
    run.score = requirements::ScoreSummary{requirements::RunVerdict::Pass,
                                            {10, 10}, {10, 10}, {10, 10}};
    run.outcomes = {{"R&1", requirements::OutcomeState::Pass}};
    storage::EvidenceEvent event;
    event.sequence = 3;
    event.kind = "object<img>";
    event.detail = "<script>alert(1)</script>";
    event.requirement_id = "R&1";
    run.events = {event};

    const auto html = render_run_detail(run, catalog, {});
    EXPECT_NE(html.find("&lt;img src=x onerror=alert(1)&gt;"), std::string::npos);
    EXPECT_NE(html.find("R&amp;1"), std::string::npos);
    EXPECT_NE(html.find("expected &quot;safe&quot; &amp; observed &lt;bad&gt;"),
              std::string::npos);
    EXPECT_NE(html.find("&lt;script&gt;alert(1)&lt;/script&gt;"), std::string::npos);
    EXPECT_EQ(html.find("<script>"), std::string::npos);
    EXPECT_EQ(html.find("<img "), std::string::npos);
    EXPECT_NE(html.find("<details><summary>Evidence (1)</summary>"),
              std::string::npos);
    EXPECT_NE(html.find("Rows shown: 1 of 1"), std::string::npos);
    EXPECT_NE(html.find("<th scope=\"row\">"), std::string::npos);

    event.kind = "compatibility_error_mapping";
    run.events = {event};
    EXPECT_NE(render_run_detail(run, catalog, {}).find("Compatibility scoring:"), std::string::npos);

    ReportFilters filtered;
    filtered.outcome = "fail";
    const auto empty = render_run_detail(run, catalog, filtered);
    EXPECT_NE(empty.find("Rows shown: 0 of 1"), std::string::npos);
}

TEST(ReportAccessibility, ShowsTheNoFetchDeclarationAndNotApplicableReasonsAsText) {
    const auto row = [](std::string id, std::vector<std::string> scenarios) {
        return requirements::Requirement{std::move(id), requirements::Strength::Must,
            {"5.1", 10, 12, 1, 1}, "publisher", "behavior", requirements::Applicability::Applicable,
            requirements::Testability::Testable, std::move(scenarios), {"evaluator"}, "why"};
    };
    requirements::RequirementCatalog catalog{18, "digest", true,
        {row("ROW-FETCH", {"fetch-publisher-track-range"}),
         row("ROW-OTHER", {"subscribe-to-publisher-track"})}};
    storage::RunRecord run;
    run.id = "run-2";
    run.config = {app::DraftVersion::Draft18, app::TransportKind::NativeQuic, app::RunMode::Observed,
                  {"fetch-publisher-track-range", "subscribe-to-publisher-track"},
                  std::chrono::milliseconds{1000}};
    run.config.publisher_capabilities.fetch = false;
    run.build = {"0.1", "rev", {}};
    run.state = storage::RunState::Finalized;
    run.created_at_unix_ns = 1;
    run.finalized_at_unix_ns = 2;
    run.outcomes = {{"ROW-FETCH", requirements::OutcomeState::NotApplicable},
                    {"ROW-OTHER", requirements::OutcomeState::Pass}};
    run.score = requirements::score(catalog, run.outcomes);

    const auto html = render_run_detail(run, catalog, {});
    EXPECT_NE(html.find("<h2>Publisher capabilities</h2>"), std::string::npos);
    EXPECT_NE(html.find("FETCH: not implemented."), std::string::npos);
    EXPECT_NE(html.find("<caption>Scenarios skipped by the declaration</caption>"), std::string::npos);
    EXPECT_NE(html.find("<th scope=\"row\"><code>fetch-publisher-track-range</code></th>"
                        "<td>publisher declared no FETCH support</td>"),
              std::string::npos);
    EXPECT_NE(html.find("<strong>not_applicable</strong>"), std::string::npos);
    EXPECT_NE(html.find("Not applicable to this run:</strong> publisher declared no FETCH support"),
              std::string::npos);
    // A capable run shows none of this.
    run.config.publisher_capabilities.fetch = true;
    EXPECT_EQ(render_run_detail(run, catalog, {}).find("Publisher capabilities"), std::string::npos);
}

}  // namespace
TEST(ReportAccessibility, ShowsRunErrorReasonsAndTruncatedContextsAsEscapedText) {
    requirements::RequirementCatalog catalog{18, "digest", true,
        {{"ROW", requirements::Strength::Must, {"5.1", 10, 12, 1, 1}, "publisher", "behavior",
          requirements::Applicability::Applicable, requirements::Testability::Testable,
          {"scene"}, {"evaluator"}, "why"}}};
    storage::RunRecord run;
    run.id = "run-3";
    run.config = {app::DraftVersion::Draft18, app::TransportKind::NativeQuic, app::RunMode::Observed,
                  {"scene"}, std::chrono::milliseconds{1000}};
    run.build = {"0.1", "rev", {}};
    run.state = storage::RunState::Finalized;
    run.outcomes = {{"ROW", requirements::OutcomeState::NotRun}};
    run.score = requirements::ScoreSummary{requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
    storage::EvidenceEvent limit;
    limit.kind = "context_event_limit";
    limit.scenario_id = "scene<1>";
    limit.detail = "more than 4096 transport events in this context; evidence truncated; this context is unscored";
    storage::EvidenceEvent error;
    error.kind = "harness_error";
    error.scenario_id = "scene";
    error.detail = "the transport rejected <a probe stream write>";
    run.events = {limit, error};
    const auto html = render_run_detail(run, catalog, {});
    EXPECT_NE(html.find("<strong>Run error:</strong> the transport rejected &lt;a probe stream write&gt;"),
              std::string::npos);
    EXPECT_NE(html.find("<caption>Why contexts or the run ended with an error</caption>"), std::string::npos);
    EXPECT_NE(html.find("<caption>Contexts whose evidence was truncated and are not scored</caption>"),
              std::string::npos);
    EXPECT_NE(html.find("<code>scene&lt;1&gt;</code>"), std::string::npos);
    EXPECT_NE(html.find("this context is unscored"), std::string::npos);
    EXPECT_EQ(html.find("<a probe"), std::string::npos);
    // A clean run shows none of it.
    run.score = requirements::ScoreSummary{requirements::RunVerdict::Incomplete, {0, 0}, {0, 0}, {0, 0}};
    run.events.clear();
    const auto clean = render_run_detail(run, catalog, {});
    EXPECT_EQ(clean.find("Run error:"), std::string::npos);
    EXPECT_EQ(clean.find("were truncated"), std::string::npos);
}

}  // namespace moq::interop::http::detail
