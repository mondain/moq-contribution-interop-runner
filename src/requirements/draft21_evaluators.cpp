#include "moq/interop/requirements/draft21_evaluators.h"

#include <algorithm>
#include <stdexcept>

namespace moq::interop::requirements {
namespace {

constexpr const char* kScenario = "d21-publisher-request-stream-placement";
constexpr const char* kEvaluator = "d21-publisher-first-message-placement";

bool opening_correlated(
    const scenarios::Draft21AnnouncementContext& context) {
    if (!context.complete || !context.target_publish_seen ||
        !context.response_delivered) return false;
    for (const auto& opening : context.evidence) {
        if (opening.kind !=
                scenarios::Draft21AnnouncementEventKind::PublishObserved ||
            !opening.stream_id || !opening.request_id ||
            (*opening.stream_id & 3u) != 0u) continue;
        const auto accepted = std::find_if(
            context.evidence.begin(), context.evidence.end(),
            [&](const scenarios::Draft21AnnouncementEvent& event) {
                return event.kind ==
                           scenarios::Draft21AnnouncementEventKind::ResponseDelivered &&
                       event.stream_id == opening.stream_id &&
                       event.request_id == opening.request_id;
            });
        if (accepted != context.evidence.end()) return true;
    }
    return false;
}

}  // namespace

std::vector<Outcome> evaluate_draft21_announcement(
    const RequirementCatalog& catalog,
    const scenarios::Draft21AnnouncementContext& context) {
    if (catalog.draft != 21 || !catalog.complete) {
        throw std::invalid_argument("a complete draft-21 catalog is required");
    }
    const bool observed = opening_correlated(context);
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& requirement : catalog.requirements) {
        OutcomeState state = OutcomeState::NotRun;
        if (requirement.applicability != Applicability::Applicable) {
            state = OutcomeState::NotApplicable;
        } else if (requirement.testability == Testability::NotTestable) {
            state = OutcomeState::NotTestable;
        } else if (observed && requirement.testability == Testability::Testable &&
                   requirement.scenarios.size() == 1 &&
                   requirement.scenarios.front() == kScenario &&
                   requirement.evaluators.size() == 1 &&
                   requirement.evaluators.front() == kEvaluator) {
            state = OutcomeState::Pass;
        }
        outcomes.push_back({requirement.id, state});
    }
    return outcomes;
}

}  // namespace moq::interop::requirements
