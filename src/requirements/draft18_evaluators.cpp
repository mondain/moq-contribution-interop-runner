#include "moq/interop/requirements/draft18_evaluators.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace moq::interop::requirements {
namespace {

constexpr const char* kResponseEvaluator =
    "exactly-one-subscribe-ok-or-request-error";
constexpr const char* kFetchResponseEvaluator =
    "exactly-one-fetch-ok-or-request-error";
constexpr const char* kDuplicateSubscriptionEvaluator =
    "duplicate-subscription-rejected";
constexpr const char* kNamespaceResponseEvaluator =
    "exactly-one-namespace-subscription-response";
constexpr const char* kTracksResponseEvaluator =
    "exactly-one-track-subscription-response";

std::optional<OutcomeState> exactly_one_response(
    const ScenarioContext& context, session::RequestKind kind) {
    if (!context.complete || !context.stimulus_delivered) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> request_id;
    std::optional<transport::StreamId> stream_id;
    for (const auto& event : context.evidence) {
        if (event.kind != session::EvidenceKind::RequestObserved) continue;
        const auto* request =
            std::get_if<session::RequestObservedEvidence>(&event.data);
        if (!request || request->initiator != session::RequestInitiator::Local ||
            request->request_kind != kind) {
            continue;
        }
        if (request_id) return OutcomeState::Fail;
        request_id = request->request_id;
        stream_id = request->stream_id;
    }
    if (!request_id) return std::nullopt;

    std::size_t accepted_responses = 0;
    bool invalid_response = false;
    for (const auto& event : context.evidence) {
        if (event.kind == session::EvidenceKind::InitialResponseObserved) {
            const auto* response =
                std::get_if<session::InitialResponseEvidence>(&event.data);
            if (!response || response->responder !=
                                 session::RequestInitiator::Peer ||
                response->original_request_id != *request_id ||
                response->stream_id != *stream_id) {
                continue;
            }
            const bool valid = response->request_kind == kind &&
                (std::holds_alternative<wire::draft18::RequestErrorMessage>(
                     response->message) ||
                 (kind == session::RequestKind::Subscribe &&
                  std::holds_alternative<wire::draft18::SubscribeOkMessage>(
                      response->message)) ||
                 (kind == session::RequestKind::Fetch &&
                  std::holds_alternative<wire::draft18::FetchOkMessage>(
                      response->message)) ||
                 ((kind == session::RequestKind::SubscribeNamespace ||
                   kind == session::RequestKind::SubscribeTracks) &&
                  std::holds_alternative<wire::draft18::RequestOkMessage>(
                      response->message)));
            if (!valid) invalid_response = true;
            ++accepted_responses;
        }
        if (event.kind == session::EvidenceKind::ResponseViolation) {
            const auto* violation =
                std::get_if<session::ResponseViolationEvidence>(&event.data);
            if (violation && violation->responder ==
                                 session::RequestInitiator::Peer &&
                violation->original_request_id == *request_id &&
                violation->stream_id == *stream_id) {
                invalid_response = true;
            }
        }
    }
    return accepted_responses == 1 && !invalid_response
               ? OutcomeState::Pass
               : OutcomeState::Fail;
}

std::optional<OutcomeState> duplicate_subscription_rejected(
    const ScenarioContext& context) {
    if (!context.complete || !context.stimulus_delivered) {
        return std::nullopt;
    }
    std::vector<const session::RequestObservedEvidence*> requests;
    for (const auto& event : context.evidence) {
        const auto* request =
            std::get_if<session::RequestObservedEvidence>(&event.data);
        if (event.kind == session::EvidenceKind::RequestObserved && request &&
            request->initiator == session::RequestInitiator::Local &&
            request->request_kind == session::RequestKind::Subscribe) {
            requests.push_back(request);
        }
    }
    if (requests.size() != 2) return std::nullopt;
    const auto* first = std::get_if<wire::draft18::SubscribeMessage>(
        &requests[0]->message);
    const auto* second = std::get_if<wire::draft18::SubscribeMessage>(
        &requests[1]->message);
    if (!first || !second || first->request_id != requests[0]->request_id ||
        second->request_id != requests[1]->request_id ||
        requests[0]->request_id >= requests[1]->request_id ||
        first->track_namespace.fields != second->track_namespace.fields ||
        first->track_name.bytes != second->track_name.bytes) {
        return std::nullopt;
    }
    bool first_accepted = false;
    std::size_t second_responses = 0;
    bool second_rejected_correctly = false;
    bool second_violation = false;
    for (const auto& event : context.evidence) {
        if (event.kind == session::EvidenceKind::InitialResponseObserved) {
            const auto* response =
                std::get_if<session::InitialResponseEvidence>(&event.data);
            if (!response || response->responder !=
                                 session::RequestInitiator::Peer ||
                response->request_kind != session::RequestKind::Subscribe) {
                continue;
            }
            if (response->original_request_id == requests[0]->request_id &&
                response->stream_id == requests[0]->stream_id &&
                std::holds_alternative<wire::draft18::SubscribeOkMessage>(
                    response->message)) {
                first_accepted = true;
            }
            if (response->original_request_id == requests[1]->request_id &&
                response->stream_id == requests[1]->stream_id) {
                ++second_responses;
                const auto* error =
                    std::get_if<wire::draft18::RequestErrorMessage>(
                        &response->message);
                second_rejected_correctly = error && error->error_code == 0x19;
            }
        } else if (event.kind == session::EvidenceKind::ResponseViolation) {
            const auto* violation =
                std::get_if<session::ResponseViolationEvidence>(&event.data);
            if (violation && violation->responder ==
                                 session::RequestInitiator::Peer &&
                violation->original_request_id == requests[1]->request_id &&
                violation->stream_id == requests[1]->stream_id) {
                second_violation = true;
            }
        }
    }
    if (!first_accepted) return std::nullopt;
    return second_responses == 1 && second_rejected_correctly &&
                   !second_violation
               ? OutcomeState::Pass
               : OutcomeState::Fail;
}

const ScenarioContext* unique_context(
    std::span<const ScenarioContext> contexts, const std::string& id) {
    const ScenarioContext* result = nullptr;
    for (const auto& context : contexts) {
        if (context.scenario_id != id) continue;
        if (result) return nullptr;
        result = &context;
    }
    return result;
}

}  // namespace

std::vector<Outcome> evaluate_draft18(
    const RequirementCatalog& catalog,
    std::span<const ScenarioContext> scenarios) {
    if (catalog.draft != 18 || !catalog.complete) {
        throw std::invalid_argument("a complete draft-18 catalog is required");
    }
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& requirement : catalog.requirements) {
        OutcomeState state = OutcomeState::NotRun;
        if (requirement.applicability != Applicability::Applicable) {
            state = OutcomeState::NotApplicable;
        } else if (requirement.testability == Testability::NotTestable) {
            state = OutcomeState::NotTestable;
        } else if (requirement.testability == Testability::Testable &&
                   requirement.scenarios.size() == 1 &&
                   requirement.scenarios.front() ==
                       "subscribe-to-publisher-track" &&
                   requirement.evaluators.size() == 1 &&
                   requirement.evaluators.front() == kResponseEvaluator) {
            const auto* context = unique_context(
                scenarios, requirement.scenarios.front());
            if (context) {
                state = exactly_one_response(
                            *context, session::RequestKind::Subscribe)
                            .value_or(OutcomeState::NotRun);
            }
        } else if (requirement.testability == Testability::Testable &&
                   requirement.scenarios.size() == 1 &&
                   requirement.scenarios.front() ==
                       "fetch-publisher-track-range" &&
                   requirement.evaluators.size() == 1 &&
                   requirement.evaluators.front() ==
                       kFetchResponseEvaluator) {
            const auto* context = unique_context(
                scenarios, requirement.scenarios.front());
            if (context) {
                state = exactly_one_response(
                            *context, session::RequestKind::Fetch)
                            .value_or(OutcomeState::NotRun);
            }
        } else if (requirement.testability == Testability::Testable &&
                   requirement.scenarios.size() == 1 &&
                   requirement.scenarios.front() ==
                       "subscribe-namespace-at-publisher" &&
                   requirement.evaluators.size() == 1 &&
                   requirement.evaluators.front() ==
                       kNamespaceResponseEvaluator) {
            const auto* context = unique_context(
                scenarios, requirement.scenarios.front());
            if (context) {
                state = exactly_one_response(
                            *context, session::RequestKind::SubscribeNamespace)
                            .value_or(OutcomeState::NotRun);
            }
        } else if (requirement.testability == Testability::Testable &&
                   requirement.scenarios.size() == 1 &&
                   requirement.scenarios.front() ==
                       "subscribe-tracks-at-publisher" &&
                   requirement.evaluators.size() == 1 &&
                   requirement.evaluators.front() ==
                       kTracksResponseEvaluator) {
            const auto* context = unique_context(
                scenarios, requirement.scenarios.front());
            if (context) {
                state = exactly_one_response(
                            *context, session::RequestKind::SubscribeTracks)
                            .value_or(OutcomeState::NotRun);
            }
        } else if (requirement.testability == Testability::Testable &&
                   requirement.scenarios.size() == 1 &&
                   requirement.scenarios.front() ==
                       "subscribe-again-to-established-publisher-track" &&
                   requirement.evaluators.size() == 1 &&
                   requirement.evaluators.front() ==
                       kDuplicateSubscriptionEvaluator) {
            const auto* context = unique_context(
                scenarios, requirement.scenarios.front());
            if (context) {
                state = duplicate_subscription_rejected(*context)
                            .value_or(OutcomeState::NotRun);
            }
        }
        outcomes.push_back({requirement.id, state});
    }
    return outcomes;
}

}  // namespace moq::interop::requirements
