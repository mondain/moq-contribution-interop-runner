#include "moq/interop/requirements/draft21_evaluators.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace moq::interop::requirements {
namespace {

const Outcome& outcome_for(const std::vector<Outcome>& outcomes,
                           const std::string& id) {
    const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                    [&](const Outcome& outcome) {
                                        return outcome.requirement_id == id;
                                    });
    if (found == outcomes.end()) throw std::logic_error("missing outcome");
    return *found;
}

RequirementCatalog small_catalog() {
    return {21, "fixture-digest", true,
            {{"D21-6-3-MUST-NOT-141", Strength::MustNot,
              {"6.3", 2111, 2114, 1, 1}, "publisher",
              "Only permitted messages start a request stream.",
              Applicability::Applicable, Testability::Testable,
              {"d21-publisher-request-stream-placement"},
              {"d21-request-stream-first-message-allowed"}, ""},
             {"D21-9-MUST-282", Strength::Must,
              {"9", 3368, 3369, 1, 1}, "publisher",
              "PUBLISH is first on its request stream.",
              Applicability::Applicable, Testability::Testable,
              {"d21-publisher-request-stream-placement"},
              {"d21-publisher-first-message-placement"}, ""},
             {"D21-OTHER-SHOULD-001", Strength::Should,
              {"9", 3370, 3371, 1, 1}, "publisher", "Other obligation",
              Applicability::Applicable, Testability::Testable,
              {"another-scenario"}, {"another-evaluator"}, ""}}};
}

scenarios::Draft21AnnouncementContext passed_context() {
    return {true, true, true,
            {{scenarios::Draft21AnnouncementEventKind::TransportEstablished,
              std::nullopt, std::nullopt},
             {scenarios::Draft21AnnouncementEventKind::PublishObserved, 0, 0},
             {scenarios::Draft21AnnouncementEventKind::ResponseDelivered, 0, 0}}};
}

TEST(Draft21Evaluators, PassesOnlyObservedPublisherOpening) {
    const auto outcomes = evaluate_draft21_announcement(
        small_catalog(), passed_context());
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D21-OTHER-SHOULD-001").state,
              OutcomeState::NotRun);
}

TEST(Draft21Evaluators, IncompleteExchangeStaysNotRun) {
    auto context = passed_context();
    context.complete = false;
    context.response_delivered = false;
    const auto outcomes = evaluate_draft21_announcement(
        small_catalog(), context);
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::NotRun);
}

TEST(Draft21Evaluators, InvalidFirstMessageFailsMustNotWithoutCompletedPublish) {
    auto context = passed_context();
    context.complete = false;
    context.target_publish_seen = false;
    context.response_delivered = false;
    context.evidence = {
        {scenarios::Draft21AnnouncementEventKind::InvalidRequestOpener,
         0, std::nullopt}};
    const auto outcomes = evaluate_draft21_announcement(
        small_catalog(), context);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Fail);
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::NotRun);
    EXPECT_EQ(score(small_catalog(), outcomes).verdict, RunVerdict::Fail);
}

TEST(Draft21Evaluators, CheckedInCatalogRetainsAllOtherRows) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    const auto outcomes = evaluate_draft21_announcement(
        catalog, passed_context());
    ASSERT_EQ(outcomes.size(), catalog.requirements.size());
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Pass);
    EXPECT_EQ(score(catalog, outcomes).verdict, RunVerdict::Incomplete);

    auto violation = passed_context();
    violation.complete = false;
    violation.target_publish_seen = false;
    violation.response_delivered = false;
    violation.evidence = {
        {scenarios::Draft21AnnouncementEventKind::InvalidRequestOpener,
         0, std::nullopt}};
    const auto failed = evaluate_draft21_announcement(catalog, violation);
    EXPECT_EQ(outcome_for(failed, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Fail);
    EXPECT_EQ(score(catalog, failed).verdict, RunVerdict::Fail);
}

}  // namespace
}  // namespace moq::interop::requirements
