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
    Requirement fetch{
        "D18-5-2-MUST-001", Strength::Must,
        {"5.2", 2159, 2160, 1, 1}, "publisher",
        "Send exactly one FETCH_OK or REQUEST_ERROR for each FETCH.",
        Applicability::Applicable, Testability::Testable,
        {"fetch-publisher-track-range"},
        {"exactly-one-fetch-ok-or-request-error"}, ""};
    Requirement namespace_response{
        "D18-6-1-MUST-001", Strength::Must,
        {"6.1", 2223, 2225, 1, 1}, "publisher", "Namespace response",
        Applicability::Applicable, Testability::Testable,
        {"subscribe-namespace-at-publisher"},
        {"exactly-one-namespace-subscription-response"}, ""};
    Requirement tracks_response{
        "D18-6-1-MUST-003", Strength::Must,
        {"6.1", 2223, 2225, 1, 3}, "publisher", "Track response",
        Applicability::Applicable, Testability::Testable,
        {"subscribe-tracks-at-publisher"},
        {"exactly-one-track-subscription-response"}, ""};
    return {18, "fixture-digest", true,
            {required, duplicate, fetch, namespace_response,
             tracks_response, other}};
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

session::EvidenceEvent fetch_request() {
    return {0, session::EvidenceKind::RequestObserved,
            session::RequestObservedEvidence{
                session::RequestInitiator::Local, 1,
                session::RequestKind::Fetch, 1,
                wire::draft18::FetchMessage{
                    1, wire::draft18::StandaloneFetch{
                        {{{std::byte{'n'}}}}, {{std::byte{'x'}}},
                        {0, 0}, {0, 1}}, {}}}};
}

session::EvidenceEvent fetch_error() {
    return {1, session::EvidenceKind::InitialResponseObserved,
            session::InitialResponseEvidence{
                session::RequestInitiator::Peer, 1,
                session::RequestKind::Fetch, 1,
                wire::draft18::RequestErrorMessage{
                    0x11, 0, {}, std::nullopt}}};
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

TEST(Draft18Evaluators, ScoresOneCorrelatedFetchResponse) {
    const ScenarioContext rejected{
        "fetch-publisher-track-range", true, true,
        {fetch_request(), fetch_error()}};
    const auto rejected_outcomes = evaluate_draft18(catalog(), {&rejected, 1});
    EXPECT_EQ(outcome_for(rejected_outcomes, "D18-5-2-MUST-001").state,
              OutcomeState::Pass);

    auto accepted = rejected;
    std::get<session::InitialResponseEvidence>(accepted.evidence[1].data)
        .message = wire::draft18::FetchOkMessage{0, {0, 1}, {}, {}};
    const auto accepted_outcomes = evaluate_draft18(catalog(), {&accepted, 1});
    EXPECT_EQ(outcome_for(accepted_outcomes, "D18-5-2-MUST-001").state,
              OutcomeState::Pass);
}

TEST(Draft18Evaluators, MissingOrDuplicateFetchResponseFails) {
    const ScenarioContext missing{
        "fetch-publisher-track-range", true, true, {fetch_request()}};
    const auto missing_outcomes = evaluate_draft18(catalog(), {&missing, 1});
    EXPECT_EQ(outcome_for(missing_outcomes, "D18-5-2-MUST-001").state,
              OutcomeState::Fail);

    const ScenarioContext duplicate{
        "fetch-publisher-track-range", true, true,
        {fetch_request(), fetch_error(),
         {2, session::EvidenceKind::ResponseViolation,
          session::ResponseViolationEvidence{
              session::RequestInitiator::Peer, 1, 1,
              wire::draft18::RequestErrorMessage{
                  0x11, 0, {}, std::nullopt}}}}};
    const auto duplicate_outcomes = evaluate_draft18(catalog(), {&duplicate, 1});
    EXPECT_EQ(outcome_for(duplicate_outcomes, "D18-5-2-MUST-001").state,
              OutcomeState::Fail);

    auto undelivered = missing;
    undelivered.stimulus_delivered = false;
    const auto undelivered_outcomes =
        evaluate_draft18(catalog(), {&undelivered, 1});
    EXPECT_EQ(outcome_for(undelivered_outcomes, "D18-5-2-MUST-001").state,
              OutcomeState::NotRun);
}

TEST(Draft18Evaluators, DiscoveryResponsesMustBeUniqueAndCorrelated) {
    for (const bool tracks : {false, true}) {
        const auto kind = tracks ? session::RequestKind::SubscribeTracks
                                 : session::RequestKind::SubscribeNamespace;
        const auto id = tracks ? "D18-6-1-MUST-003" : "D18-6-1-MUST-001";
        const auto scenario = tracks ? "subscribe-tracks-at-publisher"
                                     : "subscribe-namespace-at-publisher";
        const wire::draft18::Message request_message = tracks
            ? wire::draft18::Message{wire::draft18::SubscribeTracksMessage{1, {}, {}}}
            : wire::draft18::Message{wire::draft18::SubscribeNamespaceMessage{1, {}, {}}};
        const session::EvidenceEvent request{
            0, session::EvidenceKind::RequestObserved,
            session::RequestObservedEvidence{
                session::RequestInitiator::Local, 1, kind, 1,
                request_message}};
        const session::EvidenceEvent ok{
            1, session::EvidenceKind::InitialResponseObserved,
            session::InitialResponseEvidence{
                session::RequestInitiator::Peer, 1, kind, 1,
                wire::draft18::RequestOkMessage{{}, {}}}};
        ScenarioContext context{scenario, true, true, {request, ok}};
        EXPECT_EQ(outcome_for(evaluate_draft18(catalog(), {&context, 1}), id).state,
                  OutcomeState::Pass);
        auto wrong_stream = context;
        std::get<session::InitialResponseEvidence>(wrong_stream.evidence[1].data)
            .stream_id = 5;
        EXPECT_EQ(outcome_for(evaluate_draft18(catalog(), {&wrong_stream, 1}), id).state,
                  OutcomeState::Fail);
        auto wrong_kind = context;
        std::get<session::InitialResponseEvidence>(wrong_kind.evidence[1].data)
            .request_kind = session::RequestKind::Publish;
        EXPECT_EQ(outcome_for(evaluate_draft18(catalog(), {&wrong_kind, 1}), id).state,
                  OutcomeState::Fail);
        std::get<session::InitialResponseEvidence>(context.evidence[1].data)
            .message = wire::draft18::RequestErrorMessage{0x11, 0, {}, std::nullopt};
        EXPECT_EQ(outcome_for(evaluate_draft18(catalog(), {&context, 1}), id).state,
                  OutcomeState::Pass);
        context.evidence.push_back({
            2, session::EvidenceKind::ResponseViolation,
            session::ResponseViolationEvidence{
                session::RequestInitiator::Peer, 1, 1,
                wire::draft18::RequestOkMessage{{}, {}}}});
        EXPECT_EQ(outcome_for(evaluate_draft18(catalog(), {&context, 1}), id).state,
                  OutcomeState::Fail);
        context.evidence.pop_back();
        context.evidence.pop_back();
        EXPECT_EQ(outcome_for(evaluate_draft18(catalog(), {&context, 1}), id).state,
                  OutcomeState::Fail);
        context.stimulus_delivered = false;
        EXPECT_EQ(outcome_for(evaluate_draft18(catalog(), {&context, 1}), id).state,
                  OutcomeState::NotRun);
    }
}

TEST(Draft18Evaluators, PublisherSetupOptionMultiplicityUsesObservedTypes) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        18, root / "docs", root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(
        source, root / "requirements/draft18.json");
    constexpr const char* row = "D18-10-3-MUST-NOT-001";
    ScenarioContext context{"subscribe-to-publisher-track", true, true,
                            {subscribe_request(), subscribe_ok()}};
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::NotRun);

    const auto integer = [](std::uint64_t type) {
        return wire::draft18::KeyValuePair{
            type, wire::draft18::VarIntValue{1, {}}};
    };
    context.evidence.insert(context.evidence.begin(),
        {0, session::EvidenceKind::PeerSetupReceived,
         session::SetupEvidence{2, wire::draft18::SetupMessage{
             {integer(4)}}}});
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::Pass);
    context.evidence.pop_back();
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::NotRun);
    context.evidence.push_back(subscribe_ok());
    std::get<session::SetupEvidence>(context.evidence.front().data)
        .setup.options = {integer(4), integer(4)};
    context.complete = false;
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::Fail);
    std::get<session::SetupEvidence>(context.evidence.front().data)
        .setup.options = {integer(0x9d), integer(0x9d)};
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::NotRun);
}

TEST(Draft18Evaluators, WebTransportPublisherSetupOmitsAuthorityAndPath) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        18, root / "docs", root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(
        source, root / "requirements/draft18.json");
    constexpr const char* authority = "D18-10-3-1-1-MUST-NOT-002";
    constexpr const char* path = "D18-10-3-1-2-MUST-NOT-002";
    ScenarioContext context{"subscribe-to-publisher-track", true, true,
                            {subscribe_request(), subscribe_ok()}};
    context.webtransport = true;
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          authority).state, OutcomeState::NotRun);
    context.evidence.insert(context.evidence.begin(),
        {0, session::EvidenceKind::PeerSetupReceived,
         session::SetupEvidence{2, wire::draft18::SetupMessage{}}});
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          authority).state, OutcomeState::Pass);
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          path).state, OutcomeState::Pass);
    context.evidence.pop_back();
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          authority).state, OutcomeState::NotRun);
    context.evidence.push_back(subscribe_ok());

    auto& options = std::get<session::SetupEvidence>(context.evidence.front().data)
                        .setup.options;
    context.complete = false;
    options = {{5, wire::draft18::ByteValue{{std::byte{'x'}}}}};
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          authority).state, OutcomeState::Fail);
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          path).state, OutcomeState::NotRun);
    options = {{1, wire::draft18::ByteValue{{std::byte{'/'}}}}};
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          authority).state, OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          path).state, OutcomeState::Fail);
    context.webtransport = false;
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}),
                          path).state, OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop::requirements
