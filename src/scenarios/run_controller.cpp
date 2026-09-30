#include "moq/interop/scenarios/run_controller.h"

#include <algorithm>
#include <utility>

namespace moq::interop::scenarios {
namespace {

constexpr std::size_t kPollEvents = 256;
constexpr std::size_t kMaximumContextEvidence = 4096;

bool final_status(ScenarioStatus status) {
    return status == ScenarioStatus::Passed ||
           status == ScenarioStatus::Failed ||
           status == ScenarioStatus::TimedOut;
}

}  // namespace

Draft18RunController::Draft18RunController(
    transport::SessionTransport& transport, ScenarioDefinition definition)
    : transport_(transport),
      dispatcher_(transport_, session_),
      engine_(definition) {
    context_.scenario_id = std::move(definition.id);
}

void Draft18RunController::fail_harness() {
    harness_failed_ = true;
    engine_.stop();
}

void Draft18RunController::dispatch(const ScenarioTransition& transition,
                                    Clock::time_point now) {
    for (const auto& action : transition.actions) {
        const auto* request = std::get_if<OpenRequestAction>(&action);
        if (!request || dispatcher_.has_pending()) {
            fail_harness();
            return;
        }
        scenario_action_pending_ = true;
        const auto sent = dispatcher_.submit(*request);
        if (sent.state == DispatchState::Failed) {
            fail_harness();
            return;
        }
        if (sent.state == DispatchState::Complete) {
            context_.stimulus_delivered = sent.stimulus_delivered;
            scenario_action_pending_ = false;
            engine_.actions_delivered(now);
        }
    }
}

void Draft18RunController::execute_session_actions(
    const session::SessionTransition& transition) {
    for (const auto& action : transition.actions) {
        if (const auto* send =
                std::get_if<session::SendMessageAction>(&action)) {
            queued_messages_.push_back(*send);
            continue;
        }
        transport::OperationResult result;
        if (const auto* close =
                std::get_if<session::CloseSessionAction>(&action)) {
            result = transport_.close(close->application_error, close->reason);
        } else if (const auto* reset =
                       std::get_if<session::ResetStreamAction>(&action)) {
            result = transport_.reset(reset->stream_id,
                                      reset->application_error);
        } else if (const auto* stop =
                       std::get_if<session::StopSendingAction>(&action)) {
            result = transport_.stop_sending(stop->stream_id,
                                             stop->application_error);
        } else {
            fail_harness();
            return;
        }
        if (result.status != transport::TransportStatus::Success &&
            result.status != transport::TransportStatus::ConnectionClosed) {
            fail_harness();
            return;
        }
    }
    send_queued_messages();
}

void Draft18RunController::send_queued_messages() {
    while (!harness_failed_ && !dispatcher_.has_pending() &&
           !queued_messages_.empty()) {
        auto message = std::move(queued_messages_.front());
        queued_messages_.pop_front();
        if (dispatcher_.submit(message).state == DispatchState::Failed) {
            fail_harness();
            return;
        }
    }
}

void Draft18RunController::record_evidence(Clock::time_point now) {
    while (true) {
        auto events = session_.take_evidence(kPollEvents);
        if (events.empty()) return;
        if (events.size() > kMaximumContextEvidence -
                                context_.evidence.size()) {
            fail_harness();
            return;
        }
        for (auto& event : events) {
            context_.evidence.push_back(std::move(event));
            const auto& observed = context_.evidence.back();
            if (observed.kind == session::EvidenceKind::RequestObserved) {
                const auto* request = std::get_if<session::RequestObservedEvidence>(
                    &observed.data);
                if (request && request->initiator == session::RequestInitiator::Peer) {
                    const auto* ns = std::get_if<wire::draft18::PublishNamespaceMessage>(
                        &request->message);
                    // Only the parameter-free, parsed announcement is accepted
                    // automatically. A token-bearing announcement needs an
                    // authorization decision, not an implicit acceptance.
                    if (ns && ns->parameters.empty()) {
                        const bool forbidden_dot = !ns->track_namespace.fields.empty() &&
                            ns->track_namespace.fields.front() ==
                                std::vector<std::byte>{std::byte{'.'}};
                        if (forbidden_dot) {
                            queued_messages_.push_back(session::SendMessageAction{
                                request->stream_id,
                                wire::draft18::RequestErrorMessage{
                                    0x10, 0, {}, std::nullopt}, true});
                        } else {
                            queued_messages_.push_back(session::SendMessageAction{
                                request->stream_id,
                                wire::draft18::RequestOkMessage{}, false});
                        }
                    }
                }
            }
            if (engine_.status() == ScenarioStatus::Running &&
                !scenario_action_pending_) {
                dispatch(engine_.observe(context_.evidence.back(), now), now);
            }
        }
        if (events.size() < kPollEvents) return;
    }
}

ControllerSnapshot Draft18RunController::poll(Clock::time_point now) {
    if (harness_failed_) {
        return {engine_.status(), engine_.step_index(), true};
    }
    for (const auto& event : transport_.poll(kPollEvents)) {
        const auto transition = session_.on_event(event);
        execute_session_actions(transition);
        if (std::holds_alternative<transport::ConnectionEstablishedEvent>(
                event) && !setup_started_) {
            setup_started_ = true;
            const auto result = dispatcher_.submit_setup();
            if (result.state == DispatchState::Failed) fail_harness();
        }
        record_evidence(now);
        if (harness_failed_) break;
    }
    if (!harness_failed_ && dispatcher_.has_pending()) {
        const auto result = dispatcher_.flush();
        if (result.state == DispatchState::Failed) {
            if (result.transport_error ==
                    transport::TransportStatus::ConnectionClosed &&
                (session_.phase() == session::SessionPhase::Closing ||
                 session_.phase() == session::SessionPhase::Closed)) {
                scenario_action_pending_ = false;
                queued_messages_.clear();
                engine_.stop();
            } else {
                fail_harness();
            }
        } else if (result.state == DispatchState::Complete &&
                   scenario_action_pending_) {
            context_.stimulus_delivered = result.stimulus_delivered;
            scenario_action_pending_ = false;
            engine_.actions_delivered(now);
        }
        record_evidence(now);
        send_queued_messages();
    }
    if (!harness_failed_ && engine_.status() == ScenarioStatus::Pending &&
        session_.phase() == session::SessionPhase::Active) {
        dispatch(engine_.start(now), now);
        record_evidence(now);
    }
    if (!harness_failed_ && !scenario_action_pending_) {
        dispatch(engine_.advance(now), now);
    }
    if (!harness_failed_) send_queued_messages();
    context_.complete = !harness_failed_ &&
                        context_.stimulus_delivered &&
                        final_status(engine_.status());
    return {engine_.status(), engine_.step_index(), harness_failed_};
}

const requirements::ScenarioContext& Draft18RunController::context() const noexcept {
    return context_;
}

}  // namespace moq::interop::scenarios
