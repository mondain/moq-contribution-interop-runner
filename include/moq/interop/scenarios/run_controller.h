#pragma once

#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/scenarios/action_dispatcher.h"

#include <deque>

namespace moq::interop::scenarios {

struct ControllerSnapshot {
    ScenarioStatus status{ScenarioStatus::Pending};
    std::size_t step_index{0};
    bool harness_failed{false};
};

class Draft18RunController {
public:
    Draft18RunController(transport::SessionTransport& transport,
                         ScenarioDefinition definition);

    ControllerSnapshot poll(Clock::time_point now);
    [[nodiscard]] const requirements::ScenarioContext& context() const noexcept;

private:
    void dispatch(const ScenarioTransition& transition,
                  Clock::time_point now);
    void execute_session_actions(const session::SessionTransition& transition);
    void send_queued_messages();
    void record_evidence(Clock::time_point now);
    void fail_harness();

    transport::SessionTransport& transport_;
    session::PublisherSession session_;
    ActionDispatcher dispatcher_;
    ScenarioEngine engine_;
    requirements::ScenarioContext context_;
    std::deque<session::SendMessageAction> queued_messages_;
    bool setup_started_{false};
    bool scenario_action_pending_{false};
    bool harness_failed_{false};
};

}  // namespace moq::interop::scenarios
