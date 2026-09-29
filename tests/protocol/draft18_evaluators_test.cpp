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
    Requirement duplicate{
        "D18-5-1-MUST-004", Strength::Must,
        {"5.1", 1981, 1982, 1, 1}, "endpoint",
        "Fail a second same-role subscription with DUPLICATE_SUBSCRIPTION.",
        Applicability::Applicable, Testability::Testable,
        {"subscribe-again-to-established-publisher-track"},
        {"duplicate-subscription-rejected"}, ""};
    return {18, "fixture-digest", true, {required, duplicate, other}};
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

TEST(Draft18Evaluators, ScoresDuplicateSubscriptionOnlyForSameTrackAndCode) {
    const auto first = subscribe_request();
    auto second = subscribe_request();
    second.sequence = 2;
    auto& request = std::get<session::RequestObservedEvidence>(second.data);
    request.request_id = 3;
    request.stream_id = 3;
    auto& message = std::get<wire::draft18::SubscribeMessage>(request.message);
    message.request_id = 3;
    const session::EvidenceEvent rejected{
        3, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 3,
            session::RequestKind::Subscribe, 3,
            wire::draft18::RequestErrorMessage{0x19, 0, {}, std::nullopt}}};
    const ScenarioContext valid{
        "subscribe-again-to-established-publisher-track", true, true,
        {first, subscribe_ok(), second, rejected}};
    const auto valid_outcomes = evaluate_draft18(catalog(), {&valid, 1});
    EXPECT_EQ(outcome_for(valid_outcomes, "D18-5-1-MUST-004").state,
              OutcomeState::Pass);

    auto wrong_code = valid;
    auto& error = std::get<wire::draft18::RequestErrorMessage>(
        std::get<session::InitialResponseEvidence>(
            wrong_code.evidence.back().data).message);
    error.error_code = 0x11;
    const auto wrong_code_outcomes =
        evaluate_draft18(catalog(), {&wrong_code, 1});
    EXPECT_EQ(outcome_for(wrong_code_outcomes, "D18-5-1-MUST-004").state,
              OutcomeState::Fail);

    auto missing_response = valid;
    missing_response.evidence.pop_back();
    const auto missing_outcomes =
        evaluate_draft18(catalog(), {&missing_response, 1});
    EXPECT_EQ(outcome_for(missing_outcomes, "D18-5-1-MUST-004").state,
              OutcomeState::Fail);

    auto first_rejected = valid;
    std::get<session::InitialResponseEvidence>(
        first_rejected.evidence[1].data).message =
            wire::draft18::RequestErrorMessage{0x19, 0, {}, std::nullopt};
    const auto first_rejected_outcomes =
        evaluate_draft18(catalog(), {&first_rejected, 1});
    EXPECT_EQ(outcome_for(first_rejected_outcomes,
                          "D18-5-1-MUST-004").state,
              OutcomeState::NotRun);

    auto duplicate_response = valid;
    duplicate_response.evidence.push_back({
        4, session::EvidenceKind::ResponseViolation,
        session::ResponseViolationEvidence{
            session::RequestInitiator::Peer, 3, 3,
            wire::draft18::RequestErrorMessage{0x19, 0, {}, std::nullopt}}});
    const auto duplicate_outcomes =
        evaluate_draft18(catalog(), {&duplicate_response, 1});
    EXPECT_EQ(outcome_for(duplicate_outcomes, "D18-5-1-MUST-004").state,
              OutcomeState::Fail);

    auto different_track = valid;
    std::get<wire::draft18::SubscribeMessage>(
        std::get<session::RequestObservedEvidence>(
            different_track.evidence[2].data).message).track_name.bytes =
                {std::byte{'y'}};
    const auto different_track_outcomes =
        evaluate_draft18(catalog(), {&different_track, 1});
    EXPECT_EQ(outcome_for(different_track_outcomes, "D18-5-1-MUST-004").state,
              OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop::requirements
