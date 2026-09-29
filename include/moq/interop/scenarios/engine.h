#pragma once

#include "moq/interop/session/publisher_session.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace moq::interop::scenarios {

using Clock = std::chrono::steady_clock;
using EventMatcher = std::function<bool(const session::EvidenceEvent&)>;

enum class CompletionRule { ExpectedEvent, NoMatchingEventUntilDeadline };
enum class ScenarioStatus { Pending, Running, Passed, Failed, TimedOut, Stopped };

struct ScenarioStep {
    std::string id;
    std::chrono::milliseconds deadline;
    EventMatcher expected;
    EventMatcher contradictory;
    std::vector<session::SessionAction> actions;
    CompletionRule completion{CompletionRule::ExpectedEvent};
};

struct ScenarioDefinition {
    std::string id;
    std::vector<ScenarioStep> steps;
};

struct ScenarioTransition {
    ScenarioStatus status{ScenarioStatus::Pending};
    std::size_t step_index{0};
    std::vector<session::SessionAction> actions;
};

class ScenarioEngine {
public:
    explicit ScenarioEngine(ScenarioDefinition definition);

    ScenarioTransition start(Clock::time_point now);
    ScenarioTransition observe(const session::EvidenceEvent& event,
                               Clock::time_point now);
    ScenarioTransition advance(Clock::time_point now);
    ScenarioTransition stop();

    [[nodiscard]] ScenarioStatus status() const noexcept;
    [[nodiscard]] std::size_t step_index() const noexcept;

private:
    ScenarioTransition snapshot() const;
    ScenarioTransition complete_step(Clock::time_point now);

    ScenarioDefinition definition_;
    ScenarioStatus status_{ScenarioStatus::Pending};
    std::size_t step_index_{0};
    Clock::time_point deadline_{};
};

}  // namespace moq::interop::scenarios
