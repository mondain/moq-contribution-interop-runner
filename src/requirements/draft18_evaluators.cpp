#include "moq/interop/requirements/draft18_evaluators.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>

namespace moq::interop::requirements {
namespace {

constexpr const char* kSubscribeScenario = "subscribe-to-publisher-track";
constexpr const char* kDuplicateScenario =
    "subscribe-again-to-established-publisher-track";
constexpr const char* kFetchScenario = "fetch-publisher-track-range";
constexpr const char* kNamespaceScenario = "subscribe-namespace-at-publisher";
constexpr const char* kTracksScenario = "subscribe-tracks-at-publisher";
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
constexpr const char* kSetupMultiplicityEvaluator =
    "setup-option-types-unique-except-defined-repeatable-options";
constexpr const char* kNoAuthorityEvaluator = "setup-omits-authority";
constexpr const char* kNoPathEvaluator = "setup-omits-path";
constexpr const char* kRequestOpenerEvaluator =
    "request-stream-starts-with-allowed-request-type";

enum class SetupMultiplicity { Valid, Invalid, Indeterminate };

SetupMultiplicity setup_multiplicity(
    const wire::draft18::SetupMessage& setup) {
    bool unknown_duplicate = false;
    for (std::size_t index = 1; index < setup.options.size(); ++index) {
        const auto type = setup.options[index].type;
        if (type != setup.options[index - 1].type || type == 3u) continue;
        if (type == 1u || type == 4u || type == 5u || type == 7u) {
            return SetupMultiplicity::Invalid;
        }
        // An unknown extension can define its own repetition rule.
        unknown_duplicate = true;
    }
    return unknown_duplicate ? SetupMultiplicity::Indeterminate
                             : SetupMultiplicity::Valid;
}

const session::SetupEvidence* peer_setup(const ScenarioContext& context) {
    for (const auto& event : context.evidence) {
        if (event.kind != session::EvidenceKind::PeerSetupReceived) continue;
        if (const auto* setup = std::get_if<session::SetupEvidence>(&event.data))
            return setup;
    }
    return nullptr;
}

std::optional<OutcomeState> publisher_request_openers(
    const ScenarioContext& context) {
    constexpr std::array<std::uint64_t, 7> allowed{
        0x0d, 0x03, 0x1d, 0x16, 0x06, 0x50, 0x51};
    bool observed_opener = false;
    bool unresolved_stream = false;
    for (const auto& event : context.evidence) {
        if (event.kind != session::EvidenceKind::PeerStreamClassified) continue;
        const auto* stream = std::get_if<session::StreamEvidence>(&event.data);
        if (!stream || stream->stream_kind != session::PeerStreamKind::Request) continue;
        const bool valid_opener = std::any_of(
            context.evidence.begin(), context.evidence.end(),
            [stream](const session::EvidenceEvent& observed) {
                const auto* request =
                    std::get_if<session::RequestObservedEvidence>(&observed.data);
                return observed.kind == session::EvidenceKind::RequestObserved &&
                       request && request->initiator ==
                                      session::RequestInitiator::Peer &&
                       request->stream_id == stream->stream_id;
            });
        if (valid_opener) {
            observed_opener = true;
            continue;
        }
        const auto violation = std::find_if(
            context.evidence.begin(), context.evidence.end(),
            [stream](const session::EvidenceEvent& observed) {
                const auto* value =
                    std::get_if<session::ProtocolViolationEvidence>(&observed.data);
                return observed.kind == session::EvidenceKind::ProtocolViolation &&
                       value && value->stream_id == stream->stream_id &&
                       value->opener_message_type.has_value();
            });
        if (violation != context.evidence.end()) {
            const auto type = *std::get<session::ProtocolViolationEvidence>(
                violation->data).opener_message_type;
            if (std::find(allowed.begin(), allowed.end(), type) == allowed.end()) {
                return OutcomeState::Fail;
            }
        }
        unresolved_stream = true;
    }
    if (!observed_opener || unresolved_stream) return std::nullopt;
    return context.complete && context.stimulus_delivered
               ? std::optional<OutcomeState>{OutcomeState::Pass}
               : std::nullopt;
}

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
        } else if (requirement.id == "D18-3-3-MUST-NOT-001" &&
                   std::find(requirement.scenarios.begin(),
                             requirement.scenarios.end(), kSubscribeScenario) !=
                       requirement.scenarios.end() &&
                   std::find(requirement.evaluators.begin(),
                             requirement.evaluators.end(),
                             kRequestOpenerEvaluator) !=
                       requirement.evaluators.end()) {
            const auto* context = unique_context(scenarios, kSubscribeScenario);
            if (context) {
                state = publisher_request_openers(*context)
                            .value_or(OutcomeState::NotRun);
            }
        } else if (requirement.id == "D18-10-3-MUST-NOT-001" &&
                   std::find(requirement.scenarios.begin(),
                             requirement.scenarios.end(), kSubscribeScenario) !=
                       requirement.scenarios.end() &&
                   std::find(requirement.evaluators.begin(),
                             requirement.evaluators.end(),
                             kSetupMultiplicityEvaluator) !=
                       requirement.evaluators.end()) {
            const auto* context = unique_context(scenarios, kSubscribeScenario);
            if (context) {
                const auto* setup = peer_setup(*context);
                if (setup) {
                    const auto multiplicity = setup_multiplicity(setup->setup);
                    if (multiplicity == SetupMultiplicity::Invalid) {
                        state = OutcomeState::Fail;
                    } else if (multiplicity == SetupMultiplicity::Valid &&
                               exactly_one_response(
                                   *context, session::RequestKind::Subscribe) ==
                                   OutcomeState::Pass) {
                        state = OutcomeState::Pass;
                    }
                }
            }
        } else if (requirement.id == "D18-10-3-1-1-MUST-NOT-002" ||
                   requirement.id == "D18-10-3-1-2-MUST-NOT-002") {
            const bool authority =
                requirement.id == "D18-10-3-1-1-MUST-NOT-002";
            if (std::find(requirement.scenarios.begin(),
                          requirement.scenarios.end(), kSubscribeScenario) !=
                    requirement.scenarios.end() &&
                std::find(requirement.evaluators.begin(),
                          requirement.evaluators.end(),
                          authority ? kNoAuthorityEvaluator : kNoPathEvaluator) !=
                    requirement.evaluators.end()) {
                const auto* context = unique_context(scenarios, kSubscribeScenario);
                if (context && context->webtransport) {
                    const auto* setup = peer_setup(*context);
                    if (setup) {
                        const auto type = authority ? 5u : 1u;
                        const bool forbidden = std::any_of(
                            setup->setup.options.begin(), setup->setup.options.end(),
                            [type](const auto& option) {
                                return option.type == type;
                            });
                        if (forbidden) state = OutcomeState::Fail;
                        else if (exactly_one_response(
                                     *context, session::RequestKind::Subscribe) ==
                                 OutcomeState::Pass)
                            state = OutcomeState::Pass;
                    }
                }
            }
        } else if (requirement.testability == Testability::Testable &&
                   requirement.scenarios.size() == 1 &&
                   requirement.scenarios.front() ==
                       kSubscribeScenario &&
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
                       kFetchScenario &&
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
                       kNamespaceScenario &&
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
                       kTracksScenario &&
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
                       kDuplicateScenario &&
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

std::vector<ExecutableBinding> draft18_executable_bindings() {
    return {
        {18, "D18-3-3-MUST-NOT-001", kSubscribeScenario,
         kRequestOpenerEvaluator,
         {"peer_stream_classified", "request_observed"}},
        {18, "D18-10-3-MUST-NOT-001", kSubscribeScenario,
         kSetupMultiplicityEvaluator, {"peer_setup_received"}},
        {18, "D18-10-3-1-1-MUST-NOT-002", kSubscribeScenario,
         kNoAuthorityEvaluator, {"peer_setup_received"}},
        {18, "D18-10-3-1-2-MUST-NOT-002", kSubscribeScenario,
         kNoPathEvaluator, {"peer_setup_received"}},
        {18, "D18-5-1-MUST-001", kSubscribeScenario, kResponseEvaluator,
         {"request_observed", "initial_response_observed"}},
        {18, "D18-5-1-MUST-004", kDuplicateScenario,
         kDuplicateSubscriptionEvaluator,
         {"request_observed", "initial_response_observed"}},
        {18, "D18-5-2-MUST-001", kFetchScenario, kFetchResponseEvaluator,
         {"request_observed", "initial_response_observed"}},
        {18, "D18-6-1-MUST-001", kNamespaceScenario, kNamespaceResponseEvaluator,
         {"request_observed", "initial_response_observed"}},
        {18, "D18-6-1-MUST-003", kTracksScenario, kTracksResponseEvaluator,
         {"request_observed", "initial_response_observed"}},
    };
}

}  // namespace moq::interop::requirements
