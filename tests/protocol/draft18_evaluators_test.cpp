#include "moq/interop/requirements/draft18_evaluators.h"

#include <gtest/gtest.h>
#include "moq/interop/scenarios/draft18_close.h"
#include "moq/interop/scenarios/draft18_peer_close.h"
#include "moq/interop/scenarios/draft18_request.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "../support/raw_probe_transcript.h"

#include <algorithm>
#include <filesystem>
#include <utility>

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

TEST(Draft18Evaluators, PublisherRequestStreamOpenerRequiresObservedValidMessage) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        18, root / "docs", root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(
        source, root / "requirements/draft18.json");
    constexpr const char* row = "D18-3-3-MUST-NOT-001";
    ScenarioContext context{"subscribe-to-publisher-track", true, true,
                            {subscribe_request(), subscribe_ok()}};
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::NotRun);

    context.evidence.push_back({
        2, session::EvidenceKind::PeerStreamClassified,
        session::StreamEvidence{0, session::PeerStreamKind::Request}});
    context.evidence.push_back({
        3, session::EvidenceKind::RequestObserved,
        session::RequestObservedEvidence{
            session::RequestInitiator::Peer, 0,
            session::RequestKind::Publish, 0,
            wire::draft18::PublishMessage{0, {}, {}, 0, {}, {}}}});
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::Pass);
    auto without_subscribe_response = context;
    without_subscribe_response.evidence.erase(
        without_subscribe_response.evidence.begin() + 1);
    EXPECT_EQ(outcome_for(evaluate_draft18(checked,
                  {&without_subscribe_response, 1}), row).state,
              OutcomeState::Pass);

    context.evidence.pop_back();
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::NotRun);
    context.evidence.push_back({
        3, session::EvidenceKind::ProtocolViolation,
        session::ProtocolViolationEvidence{0, 0x3, {}, 0x2f00}});
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::Fail);
    std::get<session::ProtocolViolationEvidence>(
        context.evidence.back().data).opener_message_type = 0x3fffu;
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::Fail);

    context.complete = false;
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::Fail);

    context.evidence.pop_back();
    context.evidence.push_back({
        3, session::EvidenceKind::ProtocolViolation,
        session::ProtocolViolationEvidence{0, 0x3, {}, std::nullopt}});
    EXPECT_EQ(outcome_for(evaluate_draft18(checked, {&context, 1}), row).state,
              OutcomeState::NotRun);
}

TEST(Draft18PeerCloseEvaluators, ActualOpeningRequestAndAcceptedResponseAreRequired) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(18, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft18.json");
    for (const auto& profile : scenarios::draft18_peer_close_probes()) {
        SCOPED_TRACE(profile.requirement_id);
        const bool publish = profile.definition.id == "receive-reason-phrase-length-over-1024" ||
                             profile.definition.id == "receive-publish-request-ok-with-track-properties" ||
                             profile.definition.id == "receive-request-update-ok-with-track-properties";
        const bool namespace_request = profile.definition.id == "receive-publish-namespace-ok-with-track-properties" ||
                                       profile.definition.id == "receive-publish-namespace-redirect-with-nonempty-track-name";
        const auto opener = publish ? test::probe_bytes({0x1d, 0, 8, 0, 1, 1, 'n', 1, 'x', 0, 0}) :
            namespace_request ? test::probe_bytes({6, 0, 5, 0, 1, 1, 'n', 0}) :
                test::probe_bytes({0xd, 0, 7, 0, 1, 1, 'n', 1, 'x', 0});
        auto transcript = test::raw_probe_transcript(profile.definition, opener);
        if (profile.definition.writes.size() == 2) {
            transcript.writes[0].delivery_event_count = 3;
            transcript.events.push_back(transport::StreamDataEvent{0, test::probe_bytes({2, 0, 2, 2, 0}), false});
            transcript.writes[1].delivery_event_count = 4;
            transcript.delivery_event_count = 4;
        }
        transcript.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
        ScenarioContext context;
        context.scenario_id = profile.definition.id;
        context.complete = context.stimulus_delivered = true;
        context.raw_probe = transcript;
        const auto state = [&](const auto& candidate) {
            const auto outcomes = evaluate_draft18(catalog, std::span(&candidate, 1));
            return std::find_if(outcomes.begin(), outcomes.end(), [&](const auto& row) {
                return row.requirement_id == profile.requirement_id;
            })->state;
        };
        EXPECT_EQ(state(context), OutcomeState::Pass);
        context.raw_probe->writes.front().accepted--;
        EXPECT_EQ(state(context), OutcomeState::NotRun);
        context.raw_probe = transcript;
        std::get<transport::PeerCloseEvent>(context.raw_probe->events.back()).error_code = 4;
        EXPECT_EQ(state(context), OutcomeState::Fail);
        context.raw_probe = transcript;
        context.raw_probe->delivery_event_count = 2;
        EXPECT_EQ(state(context), OutcomeState::NotRun);
        context.raw_probe = transcript;
        std::get<transport::StreamDataEvent>(context.raw_probe->events[2]).stream_id = 4;
        EXPECT_EQ(state(context), OutcomeState::NotRun);
    }
}

}  // namespace
// A publisher that announces its namespace first waits for the acknowledgement (section 10.15)
// before it reads other streams, so a close stimulus is never processed without it.
TEST(Draft18CloseEvaluators, TheRunnerAcknowledgesThePublishersNamespaceAnnouncement) {
    for (const auto& profile : scenarios::draft18_close_profiles()) {
        const auto definition = scenarios::draft18_close_probe(profile.scenario_id, std::chrono::milliseconds(10));
        EXPECT_TRUE(definition.acknowledge_publisher_namespace) << profile.scenario_id;
    }
}

TEST(Draft18CloseEvaluators, RequireExactDeliveredTranscriptAndApplicationCloseCode) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(18,root / "docs",root / "requirements/draft-digests.json");
    const auto checked_catalog = RequirementCatalog::load(source,root / "requirements/draft18.json");
    for (const auto& profile : scenarios::draft18_close_profiles()) {
        SCOPED_TRACE(std::string(profile.scenario_id));
        const auto definition = scenarios::draft18_close_probe(profile.scenario_id, std::chrono::milliseconds(10));
        auto transcript = test::raw_probe_transcript(definition);
        transcript.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,profile.expected_close.value_or(3),{}});
        transcript.complete = transcript.stimulus_delivered = true;
        ScenarioContext context;
        context.scenario_id = profile.scenario_id;
        context.webtransport = profile.webtransport_only;
        context.complete = context.stimulus_delivered = true;
        context.raw_probe = transcript;
        auto evaluate = [&] { return outcome_for(evaluate_draft18(checked_catalog,std::span(&context,1)),std::string(profile.requirement_id)).state; };
        EXPECT_EQ(evaluate(),OutcomeState::Pass);
        if (profile.expected_close) {
            std::get<transport::PeerCloseEvent>(context.raw_probe->events.back()).error_code = 0;
            EXPECT_EQ(evaluate(),OutcomeState::Fail);
        }
        context.raw_probe = transcript;
        context.raw_probe->setup.accepted--;
        EXPECT_EQ(evaluate(),OutcomeState::NotRun);
        context.raw_probe = transcript;
        std::get<transport::PeerCloseEvent>(context.raw_probe->events.back()).error_space = transport::CloseErrorSpace::Transport;
        EXPECT_EQ(evaluate(),OutcomeState::NotRun);
        context.raw_probe = transcript;
        context.raw_probe->events.insert(context.raw_probe->events.begin(),transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
        EXPECT_EQ(evaluate(),OutcomeState::NotRun);
        context.raw_probe = transcript;
        context.raw_probe->setup.stream_id = 9;
        EXPECT_EQ(evaluate(),OutcomeState::NotRun);
        if (!transcript.writes.empty()) {
            context.raw_probe = transcript;
            context.raw_probe->writes.front().stream_id =
                transcript.writes.front().write.channel == scenarios::RawProbeChannel::Control ? 7 : 3;
            EXPECT_EQ(evaluate(),OutcomeState::NotRun);
        }
        context.raw_probe = transcript;
        context.raw_probe->harness_failed = true;
        EXPECT_EQ(evaluate(),OutcomeState::NotRun);
        context.raw_probe = transcript;
        context.raw_probe->timed_out = true;
        EXPECT_EQ(evaluate(),OutcomeState::NotRun);
    }
}

TEST(Draft18RequestEvaluators, CatalogRowsRequireMatchingResponseAfterDelivery) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(18, root / "docs", root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(source, root / "requirements/draft18.json");
    for (const auto& profile : scenarios::draft18_request_profiles()) {
        SCOPED_TRACE(profile.definition.id);
        auto transcript = test::raw_probe_transcript(profile.definition);
        if (profile.compatibility_error)
            transcript.unknown_auth_token_alias_compatibility_code = profile.expected_error;
        transcript.events.push_back(transport::StreamDataEvent{
            1, test::probe_bytes({5, 0, 3, static_cast<unsigned>(profile.expected_error), 0, 0}), true});
        ScenarioContext context;
        context.scenario_id = profile.definition.id;
        context.complete = context.stimulus_delivered = true;
        context.raw_probe = transcript;
        const auto evaluate = [&] {
            return outcome_for(evaluate_draft18(checked, std::span(&context, 1)), profile.requirement_id).state;
        };
        EXPECT_EQ(evaluate(),profile.requirement_id == "D18-10-2-2-MUST-007"
            ? OutcomeState::NotRun : OutcomeState::Pass);
        std::get<transport::StreamDataEvent>(context.raw_probe->events.back()).data[3] =
            static_cast<std::byte>(profile.expected_error + 1);
        EXPECT_EQ(evaluate(), OutcomeState::Fail);
        context.raw_probe = transcript;
        --context.raw_probe->writes.front().accepted;
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
        context.raw_probe = transcript;
        std::get<transport::StreamDataEvent>(context.raw_probe->events.back()).stream_id = 5;
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
        context.raw_probe = transcript;
        context.raw_probe->delivery_event_count = transcript.events.size();
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
    }
}

TEST(Draft18RequestEvaluators, EveryNamedContextIsRequiredAndFailureDominatesSuccess) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(18,root / "docs",root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(source,root / "requirements/draft18.json");
    constexpr const char* id = "D18-10-2-2-MUST-007";
    std::vector<ScenarioContext> contexts;
    for (const auto& profile : scenarios::draft18_request_profiles()) {
        if (profile.requirement_id != id) continue;
        auto transcript = test::raw_probe_transcript(profile.definition);
        transcript.unknown_auth_token_alias_compatibility_code = 0x17;
        transcript.events.push_back(transport::StreamDataEvent{1,test::probe_bytes({5,0,3,0x17,0,0}),true});
        ScenarioContext context;
        context.scenario_id = profile.definition.id;
        context.complete = context.stimulus_delivered = true;
        context.raw_probe = std::move(transcript);
        contexts.push_back(std::move(context));
    }
    ASSERT_EQ(contexts.size(),2u);
    const auto evaluate = [&](const std::vector<ScenarioContext>& observed) {
        return outcome_for(evaluate_draft18(checked,observed),id).state;
    };
    EXPECT_EQ(evaluate(contexts),OutcomeState::Pass);
    EXPECT_EQ(evaluate({contexts.front()}),OutcomeState::NotRun);
    auto incomplete = contexts;
    --incomplete.front().raw_probe->writes.front().accepted;
    EXPECT_EQ(evaluate(incomplete),OutcomeState::NotRun);
    auto repeated = contexts;
    repeated.push_back(contexts.front());
    EXPECT_EQ(evaluate(repeated),OutcomeState::NotRun);
    auto failed = contexts;
    std::get<transport::StreamDataEvent>(failed.front().raw_probe->events.back()).data[3] = std::byte{1};
    EXPECT_EQ(evaluate(failed),OutcomeState::Fail);
    EXPECT_EQ(evaluate({failed.back(),failed.front()}),OutcomeState::Fail);
    EXPECT_EQ(evaluate({failed.front()}),OutcomeState::Fail);
    failed.push_back(contexts.front());
    EXPECT_EQ(evaluate(failed),OutcomeState::Fail);
}

TEST(Draft18CloseEvaluators, MissingAndRepeatedContextsCannotPassButObservedFailureStillFails) {
    const auto definition = scenarios::draft18_close_probe("absolute-range-end-group-overflow",std::chrono::milliseconds(10));
    auto transcript = test::raw_probe_transcript(definition);
    transcript.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
    ScenarioContext context;
    context.scenario_id = definition.id;
    context.complete = context.stimulus_delivered = true;
    context.raw_probe = transcript;
    Requirement required{"D18-5-1-2-MUST-001",Strength::Must,{"5.1.2",2094,2095,1,1},
        "endpoint","Range overflow",Applicability::Applicable,Testability::Testable,
        {definition.id,"additional-required-context"},{"session-closed-protocol-violation"},""};
    const auto evaluate = [&](const Requirement& row,const std::vector<ScenarioContext>& observed) {
        const RequirementCatalog checked{18,"fixture",true,{row}};
        return outcome_for(evaluate_draft18(checked,observed),row.id).state;
    };
    EXPECT_EQ(evaluate(required,{context}),OutcomeState::NotRun);
    required.scenarios.pop_back();
    EXPECT_EQ(evaluate(required,{context}),OutcomeState::Pass);
    EXPECT_EQ(evaluate(required,{context,context}),OutcomeState::NotRun);
    auto failed = context;
    std::get<transport::PeerCloseEvent>(failed.raw_probe->events.back()).error_code = 0;
    EXPECT_EQ(evaluate(required,{failed,context}),OutcomeState::Fail);
    required.scenarios.push_back("additional-required-context");
    EXPECT_EQ(evaluate(required,{failed}),OutcomeState::Fail);
}

TEST(Draft18ResponseEvaluators, ActualCatalogRequiresCompleteUniqueCleanupEvidence) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(18, root / "docs", root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(source, root / "requirements/draft18.json");
    const auto profiles = scenarios::draft18_response_probes();
    ASSERT_EQ(profiles.size(), 2u);
    for (const auto& profile : profiles) {
        SCOPED_TRACE(profile.requirement_id);
        const bool subscription = profile.expectation == scenarios::Draft18ResponseExpectation::FailedSubscriptionCleanup;
        auto transcript = test::raw_probe_transcript(profile.definition);
        transcript.writes.front().delivery_event_count = transcript.events.size();
        transcript.events.push_back(transport::StreamDataEvent{1, subscription
            ? test::probe_bytes({4, 0, 4, 0, 0, 4, 1}) : test::probe_bytes({7, 0, 1, 0}), false});
        transcript.writes.back().stream_id = 1;
        transcript.writes.back().delivery_event_count = transcript.events.size();
        transcript.delivery_event_count = transcript.events.size();
        auto reply = test::probe_bytes({5, 0, 3, 1, 0, 0});
        if (subscription) {
            const auto done = test::probe_bytes({0x0b, 0, 3, 8, 0, 0});
            reply.insert(reply.end(), done.begin(), done.end());
        }
        transcript.events.push_back(transport::StreamDataEvent{1, reply, true});
        ScenarioContext context;
        context.scenario_id = profile.definition.id;
        context.complete = context.stimulus_delivered = true;
        context.raw_probe = transcript;
        const auto evaluate = [&](const std::vector<ScenarioContext>& contexts) {
            return outcome_for(evaluate_draft18(checked, contexts), profile.requirement_id).state;
        };
        EXPECT_EQ(evaluate({context}), OutcomeState::Pass);
        EXPECT_EQ(evaluate({}), OutcomeState::NotRun);
        EXPECT_EQ(evaluate({context, context}), OutcomeState::NotRun);
        auto broken = context;
        --broken.raw_probe->writes.back().accepted;
        EXPECT_EQ(evaluate({broken}), OutcomeState::NotRun);
        broken = context;
        std::get<transport::StreamDataEvent>(broken.raw_probe->events.back()).stream_id = 5;
        EXPECT_EQ(evaluate({broken}), OutcomeState::NotRun);
        broken = context;
        broken.raw_probe->writes.back().fin_accepted = false;
        EXPECT_EQ(evaluate({broken}), OutcomeState::NotRun);
        broken = context;
        std::get<transport::StreamDataEvent>(broken.raw_probe->events.back()).fin = false;
        EXPECT_EQ(evaluate({broken}), OutcomeState::NotRun);
        if (subscription) {
            broken = context;
            std::get<transport::StreamDataEvent>(broken.raw_probe->events.back()).data =
                test::probe_bytes({5, 0, 3, 1, 0, 0});
            EXPECT_EQ(evaluate({broken}), OutcomeState::Fail);
            EXPECT_EQ(evaluate({context, broken}), OutcomeState::Fail);
            EXPECT_EQ(evaluate({broken, context}), OutcomeState::Fail);
        }
        auto altered = checked;
        const auto row = std::find_if(altered.requirements.begin(), altered.requirements.end(),
            [&](const auto& item) { return item.id == profile.requirement_id; });
        ASSERT_NE(row, altered.requirements.end());
        row->evaluators.push_back("missing-evaluator");
        EXPECT_EQ(outcome_for(evaluate_draft18(altered, std::span(&context, 1)), profile.requirement_id).state,
                  OutcomeState::NotRun);
    }
}

}  // namespace moq::interop::requirements
