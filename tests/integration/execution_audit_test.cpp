#include "moq/interop/requirements/execution_audit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace moq::interop::requirements {
namespace {

RequirementCatalog catalog() {
    return {18, "source", true, {{"R1", Strength::Must,
        {"1", 1, 1, 1, 1}, "publisher", "Observable behavior",
        Applicability::Applicable, Testability::Testable,
        {"subscribe"}, {"one-response"}, "Synthetic reason"}}};
}

storage::RunRecord run(std::string id, app::TransportKind transport,
                       OutcomeState outcome = OutcomeState::Pass) {
    const app::RunConfig config{app::DraftVersion::Draft18, transport,
        app::RunMode::Observed, {"subscribe"}, std::chrono::seconds(1),
        app::TrackFixture{{"media"}, "video"}};
    storage::EvidenceEvent request;
    request.kind = "request_observed";
    request.scenario_id = "subscribe";
    request.wall_time_unix_ns = 100;
    storage::EvidenceEvent response;
    response.kind = "initial_response_observed";
    response.scenario_id = "subscribe";
    response.wall_time_unix_ns = 101;
    return {std::move(id), config, {"test", "revision", {}},
        storage::RunState::Finalized, 10, 20,
        ScoreSummary{outcome == OutcomeState::Pass ? RunVerdict::Pass
                                                   : RunVerdict::Fail,
                     {outcome == OutcomeState::Pass ? 10u : 0u, 10},
                     {outcome == OutcomeState::Pass ? 10u : 0u, 10},
                     {10, 10}},
        {{"R1", outcome}}, {request, response}};
}

std::vector<ExecutableBinding> bindings() {
    return {{18, "R1", "subscribe", "one-response",
             {"request_observed", "initial_response_observed"}}};
}

TEST(ExecutionAuditTest, CanonicalHashIgnoresRunIdentityAndTimestamps) {
    auto first = run("run-one", app::TransportKind::NativeQuic);
    auto second = run("run-two", app::TransportKind::NativeQuic);
    second.created_at_unix_ns = 500;
    second.finalized_at_unix_ns = 600;
    second.events[0].wall_time_unix_ns = 800;
    second.events[1].monotonic_time_ns = 900;
    first.config.track_fixture->track_name = std::string("\xff", 1);
    second.config.track_fixture->track_name = std::string("\xff", 1);
    EXPECT_EQ(canonical_result_sha256(first), canonical_result_sha256(second));
    second.outcomes[0].state = OutcomeState::Fail;
    EXPECT_NE(canonical_result_sha256(first), canonical_result_sha256(second));
}

TEST(ExecutionAuditTest, CanonicalHashIgnoresEvidenceOrderButNotMultiplicity) {
    auto first = run("first", app::TransportKind::WebTransport);
    auto second = run("second", app::TransportKind::WebTransport);
    std::reverse(second.events.begin(), second.events.end());
    EXPECT_EQ(canonical_result_sha256(first), canonical_result_sha256(second));
    second.events.push_back(second.events.front());
    EXPECT_NE(canonical_result_sha256(first), canonical_result_sha256(second));
}

TEST(ExecutionAuditTest, CompatibilityMappingsSeparateRepeatGroups) {
    auto first = run("first", app::TransportKind::NativeQuic);
    auto second = run("second", app::TransportKind::NativeQuic, OutcomeState::Fail);
    storage::EvidenceEvent mapping;
    mapping.kind = "compatibility_error_mapping";
    mapping.scenario_id = "subscribe";
    mapping.requirement_id = "R1";
    mapping.detail = "UNKNOWN_AUTH_TOKEN_ALIAS REQUEST_ERROR code=23";
    first.events.push_back(mapping);
    mapping.detail = "UNKNOWN_AUTH_TOKEN_ALIAS REQUEST_ERROR code=25";
    second.events.push_back(mapping);
    EXPECT_NE(canonical_result_sha256(first), canonical_result_sha256(second));
    EXPECT_TRUE(audit_execution(catalog(), bindings(), std::vector{first, second}).consistent());
    second.events.back().detail = first.events.back().detail;
    EXPECT_FALSE(audit_execution(catalog(), bindings(), std::vector{first, second}).consistent());
}

TEST(ExecutionAuditTest, StoredScoreMismatchAndUnfinishedRunAreExplicit) {
    auto incorrect = run("incorrect", app::TransportKind::NativeQuic);
    incorrect.score->required.earned = 0;
    auto active = run("active", app::TransportKind::WebTransport);
    active.state = storage::RunState::Active;
    active.score.reset();
    active.outcomes.clear();
    const std::vector runs{incorrect, active};
    const auto audit = audit_execution(catalog(), bindings(), runs);
    EXPECT_FALSE(audit.consistent());
    EXPECT_TRUE(std::any_of(audit.findings.begin(), audit.findings.end(),
        [](const auto& finding) {
            return finding.code == "stored_score_mismatch";
        }));
    EXPECT_TRUE(std::any_of(audit.findings.begin(), audit.findings.end(),
        [](const auto& finding) {
            return finding.code == "unfinished_run";
        }));
}

TEST(ExecutionAuditTest, MissingEvidenceCannotValidatePassingRow) {
    auto observed = run("run-missing", app::TransportKind::WebTransport);
    observed.events.pop_back();
    const std::vector runs{observed};
    const auto audit = audit_execution(catalog(), bindings(), runs);
    EXPECT_FALSE(audit.consistent());
    EXPECT_EQ(audit.scored_rows, 1u);
    ASSERT_EQ(audit.findings.size(), 1u);
    EXPECT_EQ(audit.findings[0].code, "missing_evaluator_evidence");
    EXPECT_EQ(audit.findings[0].requirement_id, "R1");
}

TEST(ExecutionAuditTest, PassingRowCanUseEvidenceFromSecondSelectedScenario) {
    auto observed = run("run-second", app::TransportKind::WebTransport);
    observed.config.scenario_ids.push_back("alternate");
    for (auto& event : observed.events) event.scenario_id = "alternate";
    auto registered = bindings();
    registered.push_back({18, "R1", "alternate", "one-response",
                          {"request_observed", "initial_response_observed"}});
    const std::vector runs{observed};
    const auto audit = audit_execution(catalog(), registered, runs);
    EXPECT_TRUE(audit.consistent());
}

TEST(ExecutionAuditTest, ContradictoryRepetitionsAreFlaggedWithinTransport) {
    const std::vector runs{
        run("first", app::TransportKind::NativeQuic),
        run("second", app::TransportKind::NativeQuic, OutcomeState::Fail),
        run("webtransport", app::TransportKind::WebTransport)};
    const auto audit = audit_execution(catalog(), bindings(), runs);
    EXPECT_FALSE(audit.consistent());
    EXPECT_EQ(audit.run_count, 3u);
    EXPECT_EQ(audit.scored_rows, 3u);
    EXPECT_EQ(std::count_if(audit.findings.begin(), audit.findings.end(),
        [](const auto& finding) {
            return finding.code == "nondeterministic_result";
        }), 1);
}

TEST(ExecutionAuditTest, IdenticalRepetitionsStayConsistent) {
    const std::vector runs{
        run("one", app::TransportKind::NativeQuic),
        run("two", app::TransportKind::NativeQuic),
        run("three", app::TransportKind::NativeQuic)};
    const auto audit = audit_execution(catalog(), bindings(), runs);
    EXPECT_TRUE(audit.consistent());
    EXPECT_TRUE(audit.findings.empty());
}

TEST(ExecutionAuditTest, DifferentTimeoutOrValidatorRevisionIsNotARepeat) {
    auto shorter = run("shorter", app::TransportKind::NativeQuic);
    auto longer = run("longer", app::TransportKind::NativeQuic,
                      OutcomeState::Fail);
    longer.config.timeout = std::chrono::seconds(2);
    auto newer = run("newer", app::TransportKind::NativeQuic,
                     OutcomeState::Fail);
    newer.build.source_revision = "different-revision";
    const std::vector runs{shorter, longer, newer};
    const auto audit = audit_execution(catalog(), bindings(), runs);
    EXPECT_TRUE(audit.consistent());
}

TEST(ExecutionAuditTest, PersistedRepetitionsKeepStableCanonicalResults) {
    storage::SqliteRunStore store(":memory:", {"test", "revision", {}});
    std::vector<storage::RunRecord> stored;
    for (int index = 0; index < 3; ++index) {
        const auto fixture = run("placeholder", app::TransportKind::WebTransport);
        const auto id = store.create_run(fixture.config);
        store.append_events(id, fixture.events);
        store.finalize(id, *fixture.score, fixture.outcomes);
        stored.push_back(store.load(id));
    }
    const auto audit = audit_execution(catalog(), bindings(), stored);
    EXPECT_TRUE(audit.consistent());
    EXPECT_EQ(audit.run_count, 3u);
    EXPECT_EQ(audit.scored_rows, 3u);
}

}  // namespace
}  // namespace moq::interop::requirements
