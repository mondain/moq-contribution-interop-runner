#include "detail.h"
#include "json.h"
#include "moq/interop/http/result_schema.h"
#include "moq/interop/requirements/lite_evaluators.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace moq::interop::http {
namespace {

requirements::Requirement requirement(std::string id,
                                      requirements::Strength strength,
                                      requirements::Applicability applicability,
                                      requirements::Testability testability,
                                      std::string scenario) {
    return {std::move(id), strength, {"5.1", 20, 23, 1, 1},
            "publisher", "A publisher must do something",
            applicability, testability, {std::move(scenario)}, {"evaluator"}, "reason"};
}

requirements::RequirementCatalog catalog() {
    return {18, "sha256-example", true, {
        requirement("R1", requirements::Strength::Must,
                    requirements::Applicability::Applicable,
                    requirements::Testability::Testable, "scenario#1\nbad"),
        requirement("R2", requirements::Strength::MustNot,
                    requirements::Applicability::Applicable,
                    requirements::Testability::Testable, "scenario#1\nbad"),
        requirement("R3", requirements::Strength::May,
                    requirements::Applicability::NotApplicable,
                    requirements::Testability::NotApplicable, "scenario#1\nbad")
    }};
}

storage::RunRecord run() {
    storage::RunRecord value;
    value.id = "run-7";
    value.config = {app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
                    app::RunMode::Observed, {"scenario#1\nbad"},
                    std::chrono::milliseconds{1000}};
    value.build = {"0.1", "abcdef", {{"picoquic", "test"}}};
    value.state = storage::RunState::Finalized;
    value.created_at_unix_ns = 100;
    value.finalized_at_unix_ns = 200;
    value.score = requirements::ScoreSummary{requirements::RunVerdict::Fail,
                                             {10, 20}, {10, 20}, {20, 20}};
    value.outcomes = {{"R1", requirements::OutcomeState::Pass},
                      {"R2", requirements::OutcomeState::Fail},
                      {"R3", requirements::OutcomeState::NotApplicable}};
    storage::EvidenceEvent event;
    event.sequence = 4;
    event.kind = "peer_setup_received";
    event.detail = "<unexpected>&";
    event.requirement_id = "R1";
    value.events.push_back(std::move(event));
    return value;
}

TEST(ResultExport, IncludesEveryCatalogRowAndEvidenceWithoutChangingScore) {
    const auto document = serialize_result(run(), catalog());
    EXPECT_EQ(document.at("schema_version"), 1);
    EXPECT_EQ(document.at("draft_source_sha256"), "sha256-example");
    EXPECT_EQ(document.at("run").at("id"), "run-7");
    EXPECT_EQ(document.at("run").at("score").at("required").at("earned"), 10);
    ASSERT_EQ(document.at("requirements").size(), 3);
    EXPECT_EQ(document.at("requirements").at(0).at("id"), "R1");
    EXPECT_EQ(document.at("requirements").at(0).at("outcome"), "pass");
    EXPECT_EQ(document.at("requirements").at(0).at("weight"), 10);
    EXPECT_TRUE(document.at("requirements").at(0).at("required"));
    EXPECT_EQ(document.at("requirements").at(0).at("evidence_sequences"),
              nlohmann::json::array({4}));
    EXPECT_EQ(document.at("requirements").at(1).at("outcome"), "fail");
    EXPECT_EQ(document.at("requirements").at(1).at("weight"), 10);
    EXPECT_EQ(document.at("requirements").at(1).at("strength"), "MUST NOT");
    EXPECT_EQ(document.at("requirements").at(2).at("outcome"), "not_applicable");
    EXPECT_FALSE(document.at("requirements").at(2).at("score_eligible"));
    EXPECT_EQ(document.at("evidence").at(0).at("detail"), "<unexpected>&");
}

TEST(ResultExport, ConfiguredErrorMappingLabelsCompatibilityScoring) {
    auto value = run();
    EXPECT_EQ(serialize_result(value, catalog()).at("run").at("scoring_profile"), "standards");
    storage::EvidenceEvent event;
    event.kind = "compatibility_error_mapping";
    event.detail = "UNKNOWN_AUTH_TOKEN_ALIAS REQUEST_ERROR code=25";
    value.events.push_back(event);
    EXPECT_EQ(serialize_result(value, catalog()).at("run").at("scoring_profile"), "compatibility");
    EXPECT_NE(serialize_tap14(value, catalog()).find("\"scoring_profile\":\"compatibility\""), std::string::npos);
}

TEST(ResultExport, LeavesMissingObservationsExplicitAndZeroDenominatorIntact) {
    auto value = run();
    value.outcomes.clear();
    value.events.clear();
    value.score = requirements::ScoreSummary{requirements::RunVerdict::Error,
                                             {0, 0}, {0, 0}, {0, 0}};
    const auto document = serialize_result(value, catalog());
    EXPECT_TRUE(document.at("requirements").at(0).at("outcome").is_null());
    EXPECT_EQ(document.at("run").at("score").at("required").at("possible"), 0);
    EXPECT_EQ(document.at("run").at("verdict"), "error");
}

TEST(ResultExport, ErroredRunAlwaysCarriesAReasonAndTruncatedContextsAreListed) {
    auto value = run();
    value.events.clear();
    auto document = serialize_result(value, catalog());
    EXPECT_TRUE(document.at("run").at("run_error_reason").is_null());
    EXPECT_TRUE(document.at("run").at("error_reasons").empty());
    EXPECT_TRUE(document.at("run").at("truncated_contexts").empty());
    EXPECT_EQ(document.at("schema_version"), 1);

    storage::EvidenceEvent limit;
    limit.kind = "context_event_limit";
    limit.scenario_id = "scenario-a";
    limit.detail = "more than 4096 transport events in this context; evidence truncated; this context is unscored";
    storage::EvidenceEvent error;
    error.kind = "harness_error";
    error.scenario_id = "scenario-b";
    error.detail = "the transport rejected a probe stream write";
    value.events = {limit};
    value.score = requirements::ScoreSummary{requirements::RunVerdict::Incomplete, {0, 0}, {0, 0}, {0, 0}};
    document = serialize_result(value, catalog());
    // Truncation alone is not an error and gives no run-level error reason.
    EXPECT_TRUE(document.at("run").at("run_error_reason").is_null());
    ASSERT_EQ(document.at("run").at("truncated_contexts").size(), 1u);
    EXPECT_EQ(document.at("run").at("truncated_contexts").at(0).at("scenario_id"), "scenario-a");

    value.events = {limit, error};
    value.score = requirements::ScoreSummary{requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
    document = serialize_result(value, catalog());
    EXPECT_EQ(document.at("run").at("run_error_reason"), "the transport rejected a probe stream write");
    ASSERT_EQ(document.at("run").at("error_reasons").size(), 1u);
    EXPECT_EQ(document.at("run").at("error_reasons").at(0).at("kind"), "harness_error");

    // Even an error nobody recorded a reason for says so.
    value.events.clear();
    document = serialize_result(value, catalog());
    EXPECT_FALSE(document.at("run").at("run_error_reason").is_null());
}

TEST(ResultExport, InvalidMixedObservationsAreNotReportedAsPass) {
    auto value = run();
    value.score = requirements::ScoreSummary{requirements::RunVerdict::Error,
                                             {0, 0}, {0, 0}, {0, 0}};
    value.outcomes.push_back({"R1", requirements::OutcomeState::NotRun});
    const auto document = serialize_result(value, catalog());
    EXPECT_EQ(document.at("requirements").at(0).at("outcome"), "error");
    EXPECT_EQ(document.at("requirements").at(0).at("observations"),
              nlohmann::json::array({"pass", "not_run"}));
}

TEST(ResultExport, TapHasStablePlanAndEscapedName) {
    const auto tap = serialize_tap14(run(), catalog());
    EXPECT_EQ(tap,
              "TAP version 14\n"
              "1..1\n"
              "not ok 1 - scenario%231%0Abad\n"
              "  ---\n"
              "  {\"draft\":18,\"failed\":1,\"not_run\":0,\"passed\":1,"
              "\"result\":\"fail\",\"run_id\":\"run-7\","
              "\"scenario_id\":\"scenario#1\\nbad\",\"scoring_profile\":\"standards\"}\n"
              "  ...\n");
}

TEST(ResultExport, TapHandlesEmptyScenarioPlan) {
    auto value = run();
    value.config.scenario_ids.clear();
    EXPECT_EQ(serialize_tap14(value, catalog()),
              "TAP version 14\n1..0 # SKIP no scenarios selected\n");
}

TEST(ResultExport, TapMultipleScenariosCannotInjectDirectives) {
    auto value = run();
    value.config.scenario_ids = {"scenario#1\nbad", "unknown # SKIP forged"};
    const auto tap = serialize_tap14(value, catalog());
    EXPECT_EQ(tap.rfind("TAP version 14\n1..2\n", 0), 0);
    EXPECT_NE(tap.find("not ok 1 - scenario%231%0Abad\n"), std::string::npos);
    EXPECT_NE(tap.find("ok 2 - unknown %23 SKIP forged # SKIP no scored requirements\n"),
              std::string::npos);
    EXPECT_EQ(std::count(tap.begin(), tap.end(), '\n'), 10);
}

// A run in which the publisher declared no FETCH: one FETCH scenario (skipped) and one
// that is not. Rows: all-FETCH (not applicable), mixed (stays not run), independent (pass).
requirements::RequirementCatalog fetch_catalog() {
    const auto row = [](std::string id, requirements::Strength strength, std::vector<std::string> scenarios) {
        return requirements::Requirement{std::move(id), strength, {"5.1", 20, 23, 1, 1}, "publisher",
            "behavior", requirements::Applicability::Applicable, requirements::Testability::Testable,
            std::move(scenarios), {"evaluator"}, "reason"};
    };
    return {18, "sha256-example", true, {
        row("ROW-ALL-FETCH", requirements::Strength::Must, {"fetch-publisher-track-range"}),
        row("ROW-MIXED", requirements::Strength::Must, {"fetch-publisher-track-range", "subscribe-to-publisher-track"}),
        row("ROW-INDEPENDENT", requirements::Strength::Should, {"subscribe-to-publisher-track"})}};
}

storage::RunRecord no_fetch_run() {
    auto value = run();
    value.config.scenario_ids = {"fetch-publisher-track-range", "subscribe-to-publisher-track"};
    value.config.publisher_capabilities.fetch = false;
    value.outcomes = {{"ROW-ALL-FETCH", requirements::OutcomeState::NotApplicable},
                      {"ROW-MIXED", requirements::OutcomeState::NotRun},
                      {"ROW-INDEPENDENT", requirements::OutcomeState::Pass}};
    value.score = requirements::score(fetch_catalog(), value.outcomes);
    value.events.clear();
    return value;
}

TEST(ResultExport, DeclaredNoFetchIsSelfDescribingInJson) {
    const auto value = no_fetch_run();
    ASSERT_EQ(value.score->verdict, requirements::RunVerdict::Incomplete);
    // The all-FETCH row left the denominators; the mixed row did not.
    EXPECT_EQ(value.score->required.possible, 10u);
    EXPECT_EQ(value.score->weighted.possible, 13u);
    const auto document = serialize_result(value, fetch_catalog());
    EXPECT_FALSE(document.at("publisher_capabilities").at("fetch").get<bool>());
    EXPECT_FALSE(document.at("run").at("config").at("publisher_capabilities").at("fetch").get<bool>());
    ASSERT_EQ(document.at("skipped_scenarios").size(), 1u);
    EXPECT_EQ(document.at("skipped_scenarios").at(0).at("scenario_id"), "fetch-publisher-track-range");
    EXPECT_EQ(document.at("skipped_scenarios").at(0).at("reason"), "publisher declared no FETCH support");
    const auto& rows = document.at("requirements");
    EXPECT_EQ(rows.at(0).at("outcome"), "not_applicable");
    EXPECT_NE(rows.at(0).at("not_applicable_reason").get<std::string>().find("publisher declared no FETCH support"),
              std::string::npos);
    EXPECT_EQ(rows.at(1).at("outcome"), "not_run");
    EXPECT_TRUE(rows.at(1).at("not_applicable_reason").is_null());
    EXPECT_EQ(rows.at(2).at("outcome"), "pass");
    EXPECT_TRUE(rows.at(2).at("not_applicable_reason").is_null());
}

TEST(ResultExport, DefaultRunsReportCapableAndNoSkips) {
    const auto document = serialize_result(run(), catalog());
    EXPECT_TRUE(document.at("publisher_capabilities").at("fetch").get<bool>());
    EXPECT_TRUE(document.at("skipped_scenarios").empty());
    EXPECT_TRUE(document.at("requirements").at(0).at("not_applicable_reason").is_null());
}

TEST(ResultExport, TapSkipsFetchScenariosWithTheReason) {
    const auto tap = serialize_tap14(no_fetch_run(), fetch_catalog());
    EXPECT_EQ(tap.rfind("TAP version 14\n1..2\n# publisher_capabilities fetch=false", 0), 0u) << tap;
    EXPECT_NE(tap.find("ok 1 - fetch-publisher-track-range # SKIP publisher declared no FETCH support\n"),
              std::string::npos) << tap;
    EXPECT_NE(tap.find("\"skip_reason\":\"publisher declared no FETCH support\""), std::string::npos);
    EXPECT_NE(tap.find("\"result\":\"skip\""), std::string::npos);
    // The scenario that did run is still judged on its own rows (mixed row not run).
    EXPECT_NE(tap.find("not ok 2 - subscribe-to-publisher-track\n"), std::string::npos) << tap;
}

TEST(ResultExport, RejectsMismatchedCatalogDraft) {
    auto wrong = catalog();
    wrong.draft = 21;
    EXPECT_THROW(serialize_result(run(), wrong), std::invalid_argument);
    EXPECT_THROW(serialize_tap14(run(), wrong), std::invalid_argument);
}

// A moq-lite-06 catalog (draft number 106) with a reviewed row and a still-unreviewed baseline row.
requirements::RequirementCatalog lite_catalog() {
    auto reviewed = requirement("L06-1", requirements::Strength::Must,
                                requirements::Applicability::Applicable,
                                requirements::Testability::Testable, "lite-scenario");
    auto pending = requirement("L06-2", requirements::Strength::MustNot,
                               requirements::Applicability::Applicable,
                               requirements::Testability::Testable, "lite-scenario");
    pending.reviewed = false;
    return {106, "sha256-lite", false, {std::move(reviewed), std::move(pending)}};
}

storage::RunRecord lite_run() {
    auto value = run();
    value.config.draft = app::DraftVersion::MoqLite06;
    value.config.scenario_ids = {"lite-scenario"};
    value.outcomes = {{"L06-1", requirements::OutcomeState::NotRun},
                      {"L06-2", requirements::OutcomeState::NotRun}};
    value.events.clear();
    value.score = requirements::ScoreSummary{requirements::RunVerdict::Incomplete,
                                             {0, 20}, {0, 20}, {0, 20}};
    return value;
}

TEST(ResultExport, LiteCatalogSerializesTheDraftNameEverywhere) {
    const auto lite = lite_catalog();
    const auto document = serialize_result(lite_run(), lite);
    EXPECT_EQ(document.dump().find("106"), std::string::npos) << document.dump();
    EXPECT_EQ(document.at("run").at("config").at("draft"), "moq-lite-06");
    ASSERT_EQ(document.at("requirements").size(), 2u);

    const auto tap = serialize_tap14(lite_run(), lite);
    EXPECT_NE(tap.find("\"draft\":\"moq-lite-06\""), std::string::npos) << tap;
    EXPECT_EQ(tap.find("106"), std::string::npos) << tap;

    const auto summary = detail::catalog_json(lite);
    EXPECT_EQ(summary.at("draft"), "moq-lite-06");
    EXPECT_EQ(summary.at("requirement_count"), 2);
    EXPECT_FALSE(summary.at("complete").get<bool>());

    const auto page = detail::render_run_detail(lite_run(), lite, {});
    EXPECT_NE(page.find("Draft moq-lite-06;"), std::string::npos);
    EXPECT_EQ(page.find("Draft 106"), std::string::npos);
}

TEST(ResultExport, LiteCatalogRowsReviewedFalseSerializeWithoutThrowing) {
    const auto lite = lite_catalog();
    EXPECT_NO_THROW({
        const auto document = serialize_result(lite_run(), lite);
        EXPECT_EQ(document.at("requirements").at(1).at("id"), "L06-2");
        EXPECT_EQ(document.at("requirements").at(1).at("outcome"), "not_run");
        (void)serialize_tap14(lite_run(), lite);
        (void)detail::render_run_detail(lite_run(), lite, {});
    });
}

TEST(ResultExport, LiteCatalogAuditIsStagedWithTheLiteBindings) {
    const auto selection = detail::audit_catalog(lite_catalog());
    EXPECT_EQ(selection.bindings.size(), requirements::lite_executable_bindings().size());
    EXPECT_FALSE(selection.bindings.empty());
    EXPECT_EQ(selection.report.unreviewed_total, 1u);
    EXPECT_EQ(selection.report.unreviewed_required, 1u);
    // The synthetic catalog holds none of the bound rows: those bindings are orphans, nothing else blocks.
    for (const auto& finding : selection.report.findings)
        EXPECT_TRUE(!finding.blocking || finding.code == "orphan_binding") << finding.code;
    EXPECT_FALSE(selection.report.complete());
}

TEST(ResultExport, MoqtCatalogAuditKeepsBindingsAndTheNonStagedAudit) {
    auto complete = catalog();
    complete.draft = 18;
    const auto selection = detail::audit_catalog(complete);
    EXPECT_FALSE(selection.bindings.empty());
    EXPECT_EQ(selection.report.unreviewed_total, 0u);
    // Non-staged: the synthetic rows have no real bindings, so required rows are blocking findings.
    EXPECT_TRUE(std::any_of(selection.report.findings.begin(), selection.report.findings.end(),
                            [](const auto& finding) { return finding.blocking; }));
}

TEST(ResultExport, RejectsALiteRunAgainstAMoqtCatalogAndBack) {
    EXPECT_THROW(serialize_result(lite_run(), catalog()), std::invalid_argument);
    EXPECT_THROW(serialize_tap14(run(), lite_catalog()), std::invalid_argument);
}

TEST(ResultExport, DraftCellShowsNumbersAndNamesAndNeverThrows) {
    EXPECT_EQ(detail::draft_cell(nlohmann::json(22)), "22");
    EXPECT_EQ(detail::draft_cell(nlohmann::json("moq-lite-06")), "moq-lite-06");
    EXPECT_EQ(detail::draft_cell(nlohmann::json(nullptr)), "");
    EXPECT_EQ(detail::draft_cell(nlohmann::json::array()), "");
}

TEST(ResultExport, RunListShowsALiteCompletenessEntryByName) {
    const nlohmann::json completeness = {
        {"drafts", nlohmann::json::array({{
            {"draft", detail::catalog_draft_json(106)},
            {"required_covered", 0}, {"required_total", 3},
            {"optional_covered", 0}, {"optional_total", 1},
            {"transports", nlohmann::json::array({{
                {"transport", "native-quic"}, {"run_count", 0},
                {"observed_requirement_count", 0}}})}}})}};
    std::string html;
    EXPECT_NO_THROW(html = detail::render_run_list({}, completeness));
    EXPECT_NE(html.find("<td>moq-lite-06</td>"), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::http
