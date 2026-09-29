#include "moq/interop/scenarios/engine.h"
#include "moq/interop/scenarios/draft18.h"

#include <gtest/gtest.h>

#include <chrono>
#include <variant>

namespace moq::interop::scenarios {
namespace {

using namespace std::chrono_literals;

session::EvidenceEvent evidence(session::EvidenceKind kind) {
    return {0, kind, session::MarkerEvidence{}};
}

TEST(Draft18ScenarioEngine, EvidenceAdvancesStepsAndIssuesNextActionOnce) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine({
        "publish-then-object",
        {
            ScenarioStep{"publish", 100ms,
                         [](const session::EvidenceEvent& event) {
                             return event.kind == session::EvidenceKind::RequestObserved;
                         }, {}, {}},
            ScenarioStep{"object", 200ms,
                         [](const session::EvidenceEvent& event) {
                             return event.kind == session::EvidenceKind::ObjectObserved;
                         }, {}, {session::StopSendingAction{6, 3}}},
        }});
    EXPECT_EQ(engine.start(start_time).status, ScenarioStatus::Running);
    EXPECT_EQ(engine.observe(evidence(session::EvidenceKind::RequestObserved),
                             start_time + 20ms).actions.size(), 1u);
    EXPECT_EQ(engine.observe(evidence(session::EvidenceKind::ObjectObserved),
                             start_time + 30ms).status, ScenarioStatus::Passed);
    EXPECT_TRUE(engine.observe(evidence(session::EvidenceKind::ObjectObserved),
                               start_time + 40ms).actions.empty());
}

TEST(Draft18ScenarioEngine, DeadlineIsExclusiveForExpectedEvidence) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine({"setup", {
        ScenarioStep{"await-setup", 100ms,
                     [](const session::EvidenceEvent& event) {
                         return event.kind == session::EvidenceKind::PeerSetupReceived;
                     }, {}, {}}
    }});
    engine.start(start_time);
    EXPECT_EQ(engine.observe(evidence(session::EvidenceKind::PeerSetupReceived),
                             start_time + 100ms).status,
              ScenarioStatus::TimedOut);
}

TEST(Draft18ScenarioEngine, ContradictoryEvidenceFailsAndRemainsTerminal) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine({"setup", {
        ScenarioStep{"await-setup", 100ms,
                     [](const session::EvidenceEvent& event) {
                         return event.kind == session::EvidenceKind::PeerSetupReceived;
                     },
                     [](const session::EvidenceEvent& event) {
                         return event.kind == session::EvidenceKind::ProtocolViolation;
                     }, {}}
    }});
    engine.start(start_time);
    EXPECT_EQ(engine.observe(evidence(session::EvidenceKind::ProtocolViolation),
                             start_time + 1ms).status,
              ScenarioStatus::Failed);
    EXPECT_EQ(engine.observe(evidence(session::EvidenceKind::PeerSetupReceived),
                             start_time + 2ms).status,
              ScenarioStatus::Failed);
}

TEST(Draft18ScenarioEngine, QuietWindowPassesOnlyAfterDeadline) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine({"forward-zero", {
        ScenarioStep{"no-object", 100ms,
                     [](const session::EvidenceEvent& event) {
                         return event.kind == session::EvidenceKind::ObjectObserved;
                     }, {}, {}, CompletionRule::NoMatchingEventUntilDeadline}
    }});
    engine.start(start_time);
    EXPECT_EQ(engine.advance(start_time + 99ms).status,
              ScenarioStatus::Running);
    EXPECT_EQ(engine.advance(start_time + 100ms).status,
              ScenarioStatus::Passed);
}

TEST(Draft18ScenarioEngine, OperatorStopIsTerminal) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine({"setup", {
        ScenarioStep{"await-setup", 100ms,
                     [](const session::EvidenceEvent& event) {
                         return event.kind == session::EvidenceKind::PeerSetupReceived;
                     }, {}, {}}
    }});
    engine.start(start_time);
    EXPECT_EQ(engine.stop().status, ScenarioStatus::Stopped);
    EXPECT_EQ(engine.advance(start_time + 100ms).status,
              ScenarioStatus::Stopped);
}

TEST(Draft18ScenarioEngine, SubscribeScenarioOpensRequestAndWaitsForDuplicateWindow) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine(subscribe_to_publisher_track(
        {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1, 100ms, 50ms));
    const auto started = engine.start(start_time);
    ASSERT_EQ(started.actions.size(), 1u);
    const auto* open = std::get_if<OpenRequestAction>(&started.actions[0]);
    ASSERT_NE(open, nullptr);
    const auto* subscribe =
        std::get_if<wire::draft18::SubscribeMessage>(&open->message);
    ASSERT_NE(subscribe, nullptr);
    EXPECT_EQ(subscribe->request_id, 1u);

    const session::EvidenceEvent wrong{
        1, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 3,
            session::RequestKind::Subscribe, 1,
            wire::draft18::SubscribeOkMessage{7, {}, {}}}};
    EXPECT_EQ(engine.observe(wrong, start_time + 10ms).step_index, 0u);

    const session::EvidenceEvent response{
        2, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 1,
            session::RequestKind::Subscribe, 1,
            wire::draft18::SubscribeOkMessage{7, {}, {}}}};
    EXPECT_EQ(engine.observe(response, start_time + 20ms).step_index, 1u);
    EXPECT_EQ(engine.advance(start_time + 70ms).status,
              ScenarioStatus::Passed);
}

TEST(Draft18ScenarioEngine, SubscribeScenarioRejectsPeerParityRequestId) {
    EXPECT_THROW(subscribe_to_publisher_track(
                     {{{std::byte{'n'}}}}, {{std::byte{'x'}}},
                     0, 100ms, 50ms), std::invalid_argument);
}

TEST(Draft18ScenarioEngine, DuplicateSubscriptionUsesSameTrackAndSecondRequest) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine(subscribe_again_to_established_publisher_track(
        {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1, 3, 100ms, 50ms));
    const auto started = engine.start(start_time);
    ASSERT_EQ(started.actions.size(), 1u);
    const auto* first = std::get_if<OpenRequestAction>(&started.actions[0]);
    ASSERT_NE(first, nullptr);
    const auto* first_subscribe =
        std::get_if<wire::draft18::SubscribeMessage>(&first->message);
    ASSERT_NE(first_subscribe, nullptr);
    EXPECT_EQ(first_subscribe->request_id, 1u);

    const session::EvidenceEvent accepted{
        1, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 1,
            session::RequestKind::Subscribe, 1,
            wire::draft18::SubscribeOkMessage{7, {}, {}}}};
    const auto next = engine.observe(accepted, start_time + 10ms);
    ASSERT_EQ(next.actions.size(), 1u);
    const auto* second = std::get_if<OpenRequestAction>(&next.actions[0]);
    ASSERT_NE(second, nullptr);
    const auto* second_subscribe =
        std::get_if<wire::draft18::SubscribeMessage>(&second->message);
    ASSERT_NE(second_subscribe, nullptr);
    EXPECT_EQ(second_subscribe->request_id, 3u);
    EXPECT_EQ(second_subscribe->track_namespace.fields,
              first_subscribe->track_namespace.fields);
    EXPECT_EQ(second_subscribe->track_name.bytes,
              first_subscribe->track_name.bytes);

    const session::EvidenceEvent rejected{
        2, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 3,
            session::RequestKind::Subscribe, 3,
            wire::draft18::RequestErrorMessage{0x19, 0, {}, std::nullopt}}};
    EXPECT_EQ(engine.observe(rejected, start_time + 20ms).step_index, 2u);
    EXPECT_EQ(engine.observe(rejected, start_time + 30ms).status,
              ScenarioStatus::Failed);
}

TEST(Draft18ScenarioEngine, DuplicateSubscriptionWrongErrorFailsImmediately) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine(subscribe_again_to_established_publisher_track(
        {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1, 3, 100ms, 50ms));
    engine.start(start_time);
    const session::EvidenceEvent accepted{
        1, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 1,
            session::RequestKind::Subscribe, 1,
            wire::draft18::SubscribeOkMessage{7, {}, {}}}};
    engine.observe(accepted, start_time + 10ms);
    const session::EvidenceEvent wrong_error{
        2, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 3,
            session::RequestKind::Subscribe, 3,
            wire::draft18::RequestErrorMessage{0x11, 0, {}, std::nullopt}}};
    EXPECT_EQ(engine.observe(wrong_error, start_time + 20ms).status,
              ScenarioStatus::Failed);
}

TEST(Draft18ScenarioEngine, DuplicateSubscriptionPassesAfterQuietWindow) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine(subscribe_again_to_established_publisher_track(
        {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1, 3, 100ms, 50ms));
    engine.start(start_time);
    const session::EvidenceEvent accepted{
        1, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 1,
            session::RequestKind::Subscribe, 1,
            wire::draft18::SubscribeOkMessage{7, {}, {}}}};
    engine.observe(accepted, start_time + 10ms);
    const session::EvidenceEvent rejected{
        2, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 3,
            session::RequestKind::Subscribe, 3,
            wire::draft18::RequestErrorMessage{0x19, 0, {}, std::nullopt}}};
    engine.observe(rejected, start_time + 20ms);
    EXPECT_EQ(engine.advance(start_time + 70ms).status,
              ScenarioStatus::Passed);
}

TEST(Draft18ScenarioEngine, DuplicateSubscriptionRejectsInvalidRequestIds) {
    EXPECT_THROW(subscribe_again_to_established_publisher_track(
                     {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1, 1, 100ms,
                     50ms),
                 std::invalid_argument);
    EXPECT_THROW(subscribe_again_to_established_publisher_track(
                     {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1, 2, 100ms,
                     50ms),
                 std::invalid_argument);
}

TEST(Draft18ScenarioEngine, FetchScenarioSendsStandaloneRangeAndWaitsForResponse) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine(fetch_publisher_track_range(
        {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
        {0, 0}, {0, 1}, 100ms, 50ms));
    const auto started = engine.start(start_time);
    ASSERT_EQ(started.actions.size(), 1u);
    const auto* open = std::get_if<OpenRequestAction>(&started.actions[0]);
    ASSERT_NE(open, nullptr);
    const auto* fetch = std::get_if<wire::draft18::FetchMessage>(&open->message);
    ASSERT_NE(fetch, nullptr);
    EXPECT_EQ(fetch->request_id, 1u);
    const auto* standalone =
        std::get_if<wire::draft18::StandaloneFetch>(&fetch->fetch);
    ASSERT_NE(standalone, nullptr);
    EXPECT_EQ(standalone->track_namespace.fields,
              std::vector<std::vector<std::byte>>{{std::byte{'n'}}});
    EXPECT_EQ(standalone->track_name.bytes,
              std::vector<std::byte>{std::byte{'x'}});
    EXPECT_EQ(standalone->start, (wire::draft18::Location{0, 0}));
    EXPECT_EQ(standalone->end, (wire::draft18::Location{0, 1}));

    const session::EvidenceEvent rejected{
        1, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 1,
            session::RequestKind::Fetch, 1,
            wire::draft18::RequestErrorMessage{0x11, 0, {}, std::nullopt}}};
    EXPECT_EQ(engine.observe(rejected, start_time + 10ms).step_index, 1u);
    EXPECT_EQ(engine.advance(start_time + 60ms).status,
              ScenarioStatus::Passed);
}

TEST(Draft18ScenarioEngine, FetchScenarioAcceptsFetchOkAndRejectsDuplicate) {
    const auto start_time = Clock::time_point{};
    ScenarioEngine engine(fetch_publisher_track_range(
        {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
        {0, 0}, {0, 1}, 100ms, 50ms));
    engine.start(start_time);
    const session::EvidenceEvent accepted{
        1, session::EvidenceKind::InitialResponseObserved,
        session::InitialResponseEvidence{
            session::RequestInitiator::Peer, 1,
            session::RequestKind::Fetch, 1,
            wire::draft18::FetchOkMessage{0, {0, 1}, {}, {}}}};
    EXPECT_EQ(engine.observe(accepted, start_time + 10ms).step_index, 1u);
    EXPECT_EQ(engine.observe(accepted, start_time + 20ms).status,
              ScenarioStatus::Failed);
}

TEST(Draft18ScenarioEngine, FetchScenarioRejectsPeerParityRequestId) {
    EXPECT_THROW(fetch_publisher_track_range(
                     {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 0,
                     {0, 0}, {0, 1}, 100ms, 50ms), std::invalid_argument);
}

TEST(Draft18ScenarioEngine, DiscoveryScenariosSendDistinctRequestsAndDetectDuplicates) {
    const auto start_time = Clock::time_point{};
    for (const bool tracks : {false, true}) {
        ScenarioEngine engine(tracks
            ? subscribe_tracks_at_publisher({{{std::byte{'n'}}}}, 1, 100ms, 50ms)
            : subscribe_namespace_at_publisher({{{std::byte{'n'}}}}, 1, 100ms, 50ms));
        const auto started = engine.start(start_time);
        ASSERT_EQ(started.actions.size(), 1u);
        const auto* open = std::get_if<OpenRequestAction>(&started.actions[0]);
        ASSERT_NE(open, nullptr);
        if (tracks) {
            const auto* request = std::get_if<wire::draft18::SubscribeTracksMessage>(&open->message);
            ASSERT_NE(request, nullptr);
            EXPECT_EQ(request->request_id, 1u);
            EXPECT_EQ(request->track_namespace_prefix.fields,
                      std::vector<std::vector<std::byte>>{{std::byte{'n'}}});
        } else {
            const auto* request = std::get_if<wire::draft18::SubscribeNamespaceMessage>(&open->message);
            ASSERT_NE(request, nullptr);
            EXPECT_EQ(request->request_id, 1u);
            EXPECT_EQ(request->track_namespace_prefix.fields,
                      std::vector<std::vector<std::byte>>{{std::byte{'n'}}});
        }
        const session::EvidenceEvent response{
            1, session::EvidenceKind::InitialResponseObserved,
            session::InitialResponseEvidence{
                session::RequestInitiator::Peer, 1,
                tracks ? session::RequestKind::SubscribeTracks
                       : session::RequestKind::SubscribeNamespace,
                1, wire::draft18::RequestOkMessage{{}, {}}}};
        EXPECT_EQ(engine.observe(response, start_time + 10ms).step_index, 1u);
        const session::EvidenceEvent duplicate{
            2, session::EvidenceKind::ResponseViolation,
            session::ResponseViolationEvidence{
                session::RequestInitiator::Peer, 1, 1,
                wire::draft18::RequestOkMessage{{}, {}}}};
        EXPECT_EQ(engine.observe(duplicate, start_time + 20ms).status,
                  ScenarioStatus::Failed);
    }
}

TEST(Draft18ScenarioEngine, DiscoveryScenariosRejectInvalidRequestIds) {
    EXPECT_THROW(subscribe_namespace_at_publisher({}, 0, 100ms, 50ms),
                 std::invalid_argument);
    EXPECT_THROW(subscribe_tracks_at_publisher({}, 2, 100ms, 50ms),
                 std::invalid_argument);
}

}  // namespace
}  // namespace moq::interop::scenarios
