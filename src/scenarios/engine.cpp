#include "moq/interop/scenarios/engine.h"

#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {

ScenarioEngine::ScenarioEngine(ScenarioDefinition definition)
    : definition_(std::move(definition)) {
    if (definition_.id.empty() || definition_.steps.empty()) {
        throw std::invalid_argument("scenario requires an ID and steps");
    }
    for (const auto& step : definition_.steps) {
        if (step.id.empty() || step.deadline.count() <= 0 || !step.expected) {
            throw std::invalid_argument("scenario step is incomplete");
        }
    }
}

ScenarioTransition ScenarioEngine::snapshot() const {
    return {status_, step_index_, {}};
}

ScenarioTransition ScenarioEngine::start(Clock::time_point now) {
    if (status_ != ScenarioStatus::Pending) return snapshot();
    status_ = ScenarioStatus::Running;
    deadline_ = now + definition_.steps[0].deadline;
    return {status_, step_index_, definition_.steps[0].actions};
}

ScenarioTransition ScenarioEngine::complete_step(Clock::time_point now) {
    ++step_index_;
    if (step_index_ == definition_.steps.size()) {
        status_ = ScenarioStatus::Passed;
        return snapshot();
    }
    deadline_ = now + definition_.steps[step_index_].deadline;
    return {status_, step_index_, definition_.steps[step_index_].actions};
}

ScenarioTransition ScenarioEngine::observe(
    const session::EvidenceEvent& event, Clock::time_point now) {
    if (status_ != ScenarioStatus::Running) return snapshot();
    if (now >= deadline_) return advance(now);
    const auto& step = definition_.steps[step_index_];
    if (step.contradictory && step.contradictory(event)) {
        status_ = ScenarioStatus::Failed;
        return snapshot();
    }
    if (step.expected(event)) {
        if (step.completion == CompletionRule::NoMatchingEventUntilDeadline) {
            status_ = ScenarioStatus::Failed;
            return snapshot();
        }
        return complete_step(now);
    }
    return snapshot();
}

ScenarioTransition ScenarioEngine::advance(Clock::time_point now) {
    if (status_ != ScenarioStatus::Running || now < deadline_) {
        return snapshot();
    }
    if (definition_.steps[step_index_].completion ==
        CompletionRule::NoMatchingEventUntilDeadline) {
        return complete_step(now);
    }
    status_ = ScenarioStatus::TimedOut;
    return snapshot();
}

ScenarioTransition ScenarioEngine::stop() {
    if (status_ == ScenarioStatus::Running ||
        status_ == ScenarioStatus::Pending) {
        status_ = ScenarioStatus::Stopped;
    }
    return snapshot();
}

ScenarioStatus ScenarioEngine::status() const noexcept { return status_; }
std::size_t ScenarioEngine::step_index() const noexcept { return step_index_; }

}  // namespace moq::interop::scenarios
