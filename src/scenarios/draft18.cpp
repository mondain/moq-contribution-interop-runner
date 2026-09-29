#include "moq/interop/scenarios/draft18.h"

#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {

bool is_subscribe_response(const session::EvidenceEvent& event,
                           std::uint64_t request_id) {
    if (event.kind != session::EvidenceKind::InitialResponseObserved) {
        return false;
    }
    const auto* response =
        std::get_if<session::InitialResponseEvidence>(&event.data);
    return response && response->responder ==
                           session::RequestInitiator::Peer &&
           response->original_request_id == request_id &&
           response->request_kind == session::RequestKind::Subscribe &&
           (std::holds_alternative<wire::draft18::SubscribeOkMessage>(
                response->message) ||
            std::holds_alternative<wire::draft18::RequestErrorMessage>(
                response->message));
}

bool is_duplicate_response(const session::EvidenceEvent& event,
                           std::uint64_t request_id) {
    if (is_subscribe_response(event, request_id)) return true;
    if (event.kind != session::EvidenceKind::ResponseViolation) return false;
    const auto* violation =
        std::get_if<session::ResponseViolationEvidence>(&event.data);
    return violation && violation->responder ==
                            session::RequestInitiator::Peer &&
           violation->original_request_id == request_id;
}

bool is_subscribe_ok(const session::EvidenceEvent& event,
                     std::uint64_t request_id) {
    if (event.kind != session::EvidenceKind::InitialResponseObserved) {
        return false;
    }
    const auto* response =
        std::get_if<session::InitialResponseEvidence>(&event.data);
    return response && response->responder == session::RequestInitiator::Peer &&
           response->original_request_id == request_id &&
           response->request_kind == session::RequestKind::Subscribe &&
           std::holds_alternative<wire::draft18::SubscribeOkMessage>(
               response->message);
}

bool is_duplicate_subscription_error(const session::EvidenceEvent& event,
                                     std::uint64_t request_id) {
    if (event.kind != session::EvidenceKind::InitialResponseObserved) {
        return false;
    }
    const auto* response =
        std::get_if<session::InitialResponseEvidence>(&event.data);
    if (!response || response->responder != session::RequestInitiator::Peer ||
        response->original_request_id != request_id ||
        response->request_kind != session::RequestKind::Subscribe) {
        return false;
    }
    const auto* error =
        std::get_if<wire::draft18::RequestErrorMessage>(&response->message);
    return error && error->error_code == 0x19;
}

bool is_fetch_response(const session::EvidenceEvent& event,
                       std::uint64_t request_id) {
    if (event.kind != session::EvidenceKind::InitialResponseObserved) {
        return false;
    }
    const auto* response =
        std::get_if<session::InitialResponseEvidence>(&event.data);
    return response && response->responder == session::RequestInitiator::Peer &&
           response->original_request_id == request_id &&
           response->request_kind == session::RequestKind::Fetch &&
           (std::holds_alternative<wire::draft18::FetchOkMessage>(
                response->message) ||
            std::holds_alternative<wire::draft18::RequestErrorMessage>(
                response->message));
}

bool is_extra_fetch_response(const session::EvidenceEvent& event,
                             std::uint64_t request_id) {
    if (is_fetch_response(event, request_id)) return true;
    if (event.kind != session::EvidenceKind::ResponseViolation) return false;
    const auto* violation =
        std::get_if<session::ResponseViolationEvidence>(&event.data);
    return violation && violation->responder ==
                            session::RequestInitiator::Peer &&
           violation->original_request_id == request_id;
}

}  // namespace

ScenarioDefinition subscribe_to_publisher_track(
    wire::draft18::TrackNamespace track_namespace,
    wire::draft18::TrackName track_name,
    std::uint64_t request_id,
    std::chrono::milliseconds response_deadline,
    std::chrono::milliseconds duplicate_window) {
    if ((request_id & 1u) == 0u) {
        throw std::invalid_argument("local draft-18 Request ID must be odd");
    }
    ScenarioDefinition definition;
    definition.id = "subscribe-to-publisher-track";
    definition.steps.push_back(ScenarioStep{
        "await-subscribe-response", response_deadline,
        [request_id](const session::EvidenceEvent& event) {
            return is_subscribe_response(event, request_id);
        }, {},
        {OpenRequestAction{wire::draft18::SubscribeMessage{
            request_id, std::move(track_namespace),
            std::move(track_name), {}}}}});
    definition.steps.push_back(ScenarioStep{
        "reject-duplicate-response", duplicate_window,
        [request_id](const session::EvidenceEvent& event) {
            return is_duplicate_response(event, request_id);
        }, {}, {}, CompletionRule::NoMatchingEventUntilDeadline});
    return definition;
}

ScenarioDefinition subscribe_again_to_established_publisher_track(
    wire::draft18::TrackNamespace track_namespace,
    wire::draft18::TrackName track_name,
    std::uint64_t first_request_id, std::uint64_t second_request_id,
    std::chrono::milliseconds response_deadline,
    std::chrono::milliseconds duplicate_window) {
    if ((first_request_id & 1u) == 0u ||
        (second_request_id & 1u) == 0u ||
        second_request_id <= first_request_id) {
        throw std::invalid_argument("draft-18 request IDs must be increasing odd values");
    }
    ScenarioDefinition definition;
    definition.id = "subscribe-again-to-established-publisher-track";
    definition.steps.push_back(ScenarioStep{
        "await-first-subscribe-ok", response_deadline,
        [first_request_id](const session::EvidenceEvent& event) {
            return is_subscribe_ok(event, first_request_id);
        }, {},
        {OpenRequestAction{wire::draft18::SubscribeMessage{
            first_request_id, track_namespace, track_name, {}}}}});
    definition.steps.push_back(ScenarioStep{
        "await-duplicate-subscription-error", response_deadline,
        [second_request_id](const session::EvidenceEvent& event) {
            return is_duplicate_subscription_error(event, second_request_id);
        },
        [second_request_id](const session::EvidenceEvent& event) {
            return is_duplicate_response(event, second_request_id) &&
                   !is_duplicate_subscription_error(event, second_request_id);
        },
        {OpenRequestAction{wire::draft18::SubscribeMessage{
            second_request_id, std::move(track_namespace),
            std::move(track_name), {}}}}});
    definition.steps.push_back(ScenarioStep{
        "reject-extra-duplicate-response", duplicate_window,
        [second_request_id](const session::EvidenceEvent& event) {
            return is_duplicate_response(event, second_request_id);
        }, {}, {}, CompletionRule::NoMatchingEventUntilDeadline});
    return definition;
}

ScenarioDefinition fetch_publisher_track_range(
    wire::draft18::TrackNamespace track_namespace,
    wire::draft18::TrackName track_name, std::uint64_t request_id,
    wire::draft18::Location start, wire::draft18::Location end,
    std::chrono::milliseconds response_deadline,
    std::chrono::milliseconds duplicate_window) {
    if ((request_id & 1u) == 0u || end.group < start.group ||
        (end.group == start.group && end.object < start.object)) {
        throw std::invalid_argument("invalid draft-18 FETCH request");
    }
    ScenarioDefinition definition;
    definition.id = "fetch-publisher-track-range";
    definition.steps.push_back(ScenarioStep{
        "await-fetch-response", response_deadline,
        [request_id](const session::EvidenceEvent& event) {
            return is_fetch_response(event, request_id);
        }, {},
        {OpenRequestAction{wire::draft18::FetchMessage{
            request_id,
            wire::draft18::StandaloneFetch{
                std::move(track_namespace), std::move(track_name),
                start, end},
            {}}}}});
    definition.steps.push_back(ScenarioStep{
        "reject-extra-fetch-response", duplicate_window,
        [request_id](const session::EvidenceEvent& event) {
            return is_extra_fetch_response(event, request_id);
        }, {}, {}, CompletionRule::NoMatchingEventUntilDeadline});
    return definition;
}

}  // namespace moq::interop::scenarios
