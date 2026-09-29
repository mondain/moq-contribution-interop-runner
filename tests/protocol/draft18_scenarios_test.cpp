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

}  // namespace
}  // namespace moq::interop::scenarios
