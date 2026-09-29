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

}  // namespace moq::interop::scenarios
