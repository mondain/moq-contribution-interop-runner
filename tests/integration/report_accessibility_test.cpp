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

    ReportFilters filtered;
    filtered.outcome = "fail";
    const auto empty = render_run_detail(run, catalog, filtered);
    EXPECT_NE(empty.find("Rows shown: 0 of 1"), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::http::detail
