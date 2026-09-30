#include "moq/interop/http/result_schema.h"

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
              "\"scenario_id\":\"scenario#1\\nbad\"}\n"
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

TEST(ResultExport, RejectsMismatchedCatalogDraft) {
    auto wrong = catalog();
    wrong.draft = 21;
    EXPECT_THROW(serialize_result(run(), wrong), std::invalid_argument);
    EXPECT_THROW(serialize_tap14(run(), wrong), std::invalid_argument);
}

}  // namespace
}  // namespace moq::interop::http
