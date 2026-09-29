#include "moq/interop/requirements/draft18_evaluators.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

namespace moq::interop::requirements {
namespace {

RequirementCatalog catalog() {
    Requirement required{
        "D18-5-1-MUST-001", Strength::Must,
        {"5.1", 1936, 1937, 1, 1}, "publisher",
        "Send exactly one SUBSCRIBE_OK or REQUEST_ERROR for each SUBSCRIBE.",
        Applicability::Applicable, Testability::Testable,
        {"subscribe-to-publisher-track"},
        {"exactly-one-subscribe-ok-or-request-error"}, ""};
    Requirement other{
        "D18-OTHER-SHOULD-001", Strength::Should,
        {"5.1", 1940, 1941, 1, 1}, "publisher", "Other obligation",
        Applicability::Applicable, Testability::Testable,
        {"another-scenario"}, {"another-evaluator"}, ""};
    return {18, "fixture-digest", true, {required, other}};
}

session::EvidenceEvent subscribe_request() {
    return {0, session::EvidenceKind::RequestObserved,
            session::RequestObservedEvidence{
                session::RequestInitiator::Local, 1,
                session::RequestKind::Subscribe, 1,
                wire::draft18::SubscribeMessage{1, {}, {}, {}}}};
}

session::EvidenceEvent subscribe_ok() {
    return {1, session::EvidenceKind::InitialResponseObserved,
            session::InitialResponseEvidence{
                session::RequestInitiator::Peer, 1,
                session::RequestKind::Subscribe, 1,
                wire::draft18::SubscribeOkMessage{7, {}, {}}}};
}

const Outcome& outcome_for(const std::vector<Outcome>& outcomes,
                           const std::string& id) {
    const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                    [&](const Outcome& outcome) {
                                        return outcome.requirement_id == id;
                                    });
    EXPECT_NE(found, outcomes.end());
    return *found;
}

TEST(Draft18Evaluators, ScoresOneCorrelatedSubscribeResponseOnly) {
    const ScenarioContext context{
        "subscribe-to-publisher-track", true, true,
        {subscribe_request(), subscribe_ok()}};
    const auto outcomes = evaluate_draft18(catalog(), {&context, 1});
    EXPECT_EQ(outcome_for(outcomes, "D18-5-1-MUST-001").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D18-OTHER-SHOULD-001").state,
              OutcomeState::NotRun);
}

TEST(Draft18Evaluators, MissingResponseAfterDeliveredStimulusFails) {
    const ScenarioContext context{
        "subscribe-to-publisher-track", true, true,
        {subscribe_request()}};
    const auto outcomes = evaluate_draft18(catalog(), {&context, 1});
    EXPECT_EQ(outcome_for(outcomes, "D18-5-1-MUST-001").state,
              OutcomeState::Fail);
}

TEST(Draft18Evaluators, UndeliveredStimulusRemainsNotRun) {
    const ScenarioContext context{
        "subscribe-to-publisher-track", true, false,
        {subscribe_request()}};
    const auto outcomes = evaluate_draft18(catalog(), {&context, 1});
    EXPECT_EQ(outcome_for(outcomes, "D18-5-1-MUST-001").state,
              OutcomeState::NotRun);
}

TEST(Draft18Evaluators, DuplicateResponseFails) {
    const ScenarioContext context{
        "subscribe-to-publisher-track", true, true,
        {subscribe_request(), subscribe_ok(),
         {2, session::EvidenceKind::ResponseViolation,
          session::ResponseViolationEvidence{
              session::RequestInitiator::Peer, 1, 1,
              wire::draft18::SubscribeOkMessage{7, {}, {}}}}}};
    const auto outcomes = evaluate_draft18(catalog(), {&context, 1});
    EXPECT_EQ(outcome_for(outcomes, "D18-5-1-MUST-001").state,
              OutcomeState::Fail);
}

TEST(Draft18Evaluators, CheckedInCatalogRetainsUnimplementedRowsAsNotRun) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        18, root / "docs", root / "requirements/draft-digests.json");
    const auto actual = RequirementCatalog::load(
        source, root / "requirements/draft18.json");
    const ScenarioContext context{
        "subscribe-to-publisher-track", true, true,
        {subscribe_request(), subscribe_ok()}};
    const auto outcomes = evaluate_draft18(actual, {&context, 1});
    ASSERT_EQ(outcomes.size(), actual.requirements.size());
    EXPECT_EQ(outcome_for(outcomes, "D18-5-1-MUST-001").state,
              OutcomeState::Pass);
    EXPECT_EQ(score(actual, outcomes).verdict, RunVerdict::Incomplete);
}

}  // namespace
}  // namespace moq::interop::requirements
