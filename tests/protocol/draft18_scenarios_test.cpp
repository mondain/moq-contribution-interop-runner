#include "moq/interop/scenarios/engine.h"

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

}  // namespace
}  // namespace moq::interop::scenarios
