#include "moq/interop/requirements/draft21_evaluators.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <tuple>

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

TEST(Draft21Evaluators, SetupProbeNeedsSetupBeforePublisherAction) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    auto context = passed_context();
    context.setup_probe = scenarios::Draft21SetupProbe::UnknownOption;
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-MUST-287").state,
              OutcomeState::NotRun);

    context.evidence.insert(
        context.evidence.begin() + 1,
        {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
         std::nullopt});
    const auto one = evaluate_draft21_announcement(catalog, context);
    EXPECT_EQ(outcome_for(one, "D21-9-1-MUST-287").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(one, "D21-9-1-MUST-288").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(one, "D21-9-1-MUST-290").state,
              OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(one, "D21-9-MUST-282").state,
              OutcomeState::NotRun);

    context.setup_probe = scenarios::Draft21SetupProbe::DuplicateUnknownOption;
    const auto duplicate = evaluate_draft21_announcement(catalog, context);
    EXPECT_EQ(outcome_for(duplicate, "D21-9-1-MUST-287").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(duplicate, "D21-9-1-MUST-288").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(duplicate, "D21-9-1-MUST-290").state,
              OutcomeState::Pass);

    std::swap(context.evidence[1], context.evidence[2]);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-MUST-290").state,
              OutcomeState::NotRun);
}

TEST(Draft21Evaluators, ForbiddenServerUriOptionsRequireExactPeerClose) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    for (const auto& probe : {
             std::pair{scenarios::Draft21SetupProbe::ServerAuthority,
                       "D21-9-1-1-MUST-293"},
             std::pair{scenarios::Draft21SetupProbe::ServerPath,
                       "D21-9-1-2-MUST-300"}}) {
        auto context = passed_context();
        context.complete = false;
        context.target_publish_seen = false;
        context.response_delivered = false;
        context.setup_probe = probe.first;
        context.evidence = {
            {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
             std::nullopt},
            {scenarios::Draft21AnnouncementEventKind::PeerClosed,
             std::nullopt, std::nullopt}};
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::NotRun);
        const auto expected = probe.first ==
                                      scenarios::Draft21SetupProbe::ServerAuthority
                                  ? 0x19u : 0x8u;
        context.evidence[1].application_close_code = expected;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::Pass);
        context.evidence[1].application_close_code = 3;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::Fail);
        context.evidence.clear();
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::NotRun);

        context = passed_context();
        context.setup_probe = probe.first;
        context.evidence.insert(
            context.evidence.begin() + 1,
            {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
             std::nullopt});
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::Fail);
    }
}

TEST(Draft21Evaluators, WebTransportServerUriOptionRowsRequireExactClose) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    for (const auto& [probe, webtransport_row, native_row, expected] : {
             std::tuple{scenarios::Draft21SetupProbe::ServerAuthority,
                        "D21-9-1-1-MUST-294", "D21-9-1-1-MUST-293", 0x19u},
             std::tuple{scenarios::Draft21SetupProbe::ServerPath,
                        "D21-9-1-2-MUST-301", "D21-9-1-2-MUST-300", 0x8u}}) {
        auto context = passed_context();
        context.complete = false;
        context.target_publish_seen = false;
        context.response_delivered = false;
        context.setup_probe = probe;
        context.evidence = {
            {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
             std::nullopt},
            {scenarios::Draft21AnnouncementEventKind::PeerClosed,
             std::nullopt, std::nullopt, expected}};
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              webtransport_row).state, OutcomeState::NotRun);
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              native_row).state, OutcomeState::Pass);

        context.webtransport = true;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              webtransport_row).state, OutcomeState::Pass);
        context.evidence[1].application_close_code = 3;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              webtransport_row).state, OutcomeState::Fail);
    }
}

TEST(Draft21Evaluators, PublisherWebTransportSetupOmitsAuthorityAndPath) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    auto context = passed_context();
    context.webtransport = true;
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::NotRun);

    context.evidence.insert(context.evidence.begin() + 1,
        {scenarios::Draft21AnnouncementEventKind::PeerSetupReceived, 2,
         std::nullopt});
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::Pass);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::Pass);

    context.complete = false;
    context.peer_setup_option_types = {5};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::Fail);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::NotRun);
    context.peer_setup_option_types = {1};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::Fail);
    context.webtransport = false;
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop::requirements
