#include "moq/interop/requirements/draft21_evaluators.h"

#include <algorithm>
#include <stdexcept>

namespace moq::interop::requirements {
namespace {

constexpr const char* kScenario = "d21-publisher-request-stream-placement";
constexpr const char* kEvaluator = "d21-publisher-first-message-placement";
constexpr const char* kAllowedOpenerEvaluator =
    "d21-request-stream-first-message-allowed";
constexpr const char* kUnknownScenario = "d21-setup-unknown-options";
constexpr const char* kDuplicateScenario =
    "d21-setup-duplicate-unknown-options";
constexpr const char* kUnknownEvaluator =
    "d21-unknown-setup-options-ignored";
constexpr const char* kDuplicateEvaluator =
    "d21-duplicate-unknown-setup-options-accepted";
constexpr const char* kAuthorityScenario = "d21-server-sends-authority";
constexpr const char* kPathScenario = "d21-server-sends-path";
constexpr const char* kAuthorityEvaluator =
    "d21-server-authority-invalid-authority";
constexpr const char* kPathEvaluator = "d21-server-path-invalid-path";

bool includes(const std::vector<std::string>& values, const char* value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

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
    const auto setup_sent = std::find_if(
        context.evidence.begin(), context.evidence.end(),
        [](const scenarios::Draft21AnnouncementEvent& event) {
            return event.kind ==
                   scenarios::Draft21AnnouncementEventKind::LocalSetupSent;
        });
    const auto publish_seen = std::find_if(
        context.evidence.begin(), context.evidence.end(),
        [](const scenarios::Draft21AnnouncementEvent& event) {
            return event.kind ==
                   scenarios::Draft21AnnouncementEventKind::PublishObserved;
        });
    const bool unknown_probe_passed =
        observed && setup_sent != context.evidence.end() &&
        publish_seen != context.evidence.end() && setup_sent < publish_seen &&
        context.setup_probe != scenarios::Draft21SetupProbe::None;
    const bool publish_after_setup =
        observed && setup_sent != context.evidence.end() &&
        publish_seen != context.evidence.end() && setup_sent < publish_seen;
    const bool duplicate_probe_passed =
        unknown_probe_passed &&
        context.setup_probe ==
            scenarios::Draft21SetupProbe::DuplicateUnknownOption;
    const auto peer_closed = std::find_if(
        context.evidence.begin(), context.evidence.end(),
        [](const scenarios::Draft21AnnouncementEvent& event) {
            return event.kind ==
                   scenarios::Draft21AnnouncementEventKind::PeerClosed;
        });
    const bool peer_closed_after_setup =
        setup_sent != context.evidence.end() &&
        peer_closed != context.evidence.end() && setup_sent < peer_closed;
    const bool invalid_opener = std::any_of(
        context.evidence.begin(), context.evidence.end(),
        [](const scenarios::Draft21AnnouncementEvent& event) {
            return event.kind ==
                   scenarios::Draft21AnnouncementEventKind::InvalidRequestOpener;
        });
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& requirement : catalog.requirements) {
        OutcomeState state = OutcomeState::NotRun;
        if (requirement.applicability != Applicability::Applicable) {
            state = OutcomeState::NotApplicable;
        } else if (requirement.testability == Testability::NotTestable) {
            state = OutcomeState::NotTestable;
        } else if (requirement.id == "D21-9-1-MUST-287" ||
                   requirement.id == "D21-9-1-MUST-288") {
            const auto* scenario =
                context.setup_probe ==
                        scenarios::Draft21SetupProbe::DuplicateUnknownOption
                    ? kDuplicateScenario : kUnknownScenario;
            if (unknown_probe_passed &&
                includes(requirement.scenarios, scenario) &&
                includes(requirement.evaluators, kUnknownEvaluator)) {
                state = OutcomeState::Pass;
            }
        } else if (requirement.id == "D21-9-1-MUST-290") {
            if (duplicate_probe_passed &&
                includes(requirement.scenarios, kDuplicateScenario) &&
                includes(requirement.evaluators, kDuplicateEvaluator)) {
                state = OutcomeState::Pass;
            }
        } else if (requirement.id == "D21-9-1-1-MUST-293" ||
                   requirement.id == "D21-9-1-2-MUST-300") {
            const bool authority = requirement.id == "D21-9-1-1-MUST-293";
            const auto expected_probe =
                authority ? scenarios::Draft21SetupProbe::ServerAuthority
                          : scenarios::Draft21SetupProbe::ServerPath;
            if (context.setup_probe == expected_probe &&
                includes(requirement.scenarios,
                         authority ? kAuthorityScenario : kPathScenario) &&
                includes(requirement.evaluators,
                         authority ? kAuthorityEvaluator : kPathEvaluator)) {
                if (publish_after_setup) {
                    state = OutcomeState::Fail;
                } else if (peer_closed_after_setup &&
                           peer_closed->application_close_code) {
                    state = *peer_closed->application_close_code ==
                                    (authority ? 0x19u : 0x8u)
                                ? OutcomeState::Pass : OutcomeState::Fail;
                }
            }
        } else if (context.setup_probe == scenarios::Draft21SetupProbe::None &&
                   requirement.id == "D21-6-3-MUST-NOT-141" &&
                   requirement.testability == Testability::Testable &&
                   std::find(requirement.scenarios.begin(),
                             requirement.scenarios.end(), kScenario) !=
                       requirement.scenarios.end() &&
                   std::find(requirement.evaluators.begin(),
                             requirement.evaluators.end(),
                             kAllowedOpenerEvaluator) !=
                       requirement.evaluators.end()) {
            if (invalid_opener) state = OutcomeState::Fail;
            else if (observed) state = OutcomeState::Pass;
        } else if (context.setup_probe == scenarios::Draft21SetupProbe::None &&
                   observed && requirement.testability == Testability::Testable &&
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

std::vector<ExecutableBinding> draft21_executable_bindings() {
    return {
        {21, "D21-6-3-MUST-NOT-141", kScenario,
         kAllowedOpenerEvaluator, {"publish_observed", "response_delivered"}},
        {21, "D21-9-MUST-282", kScenario, kEvaluator,
         {"publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-287", kUnknownScenario,
         kUnknownEvaluator,
         {"local_setup_sent", "publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-287", kDuplicateScenario,
         kUnknownEvaluator,
         {"local_setup_sent", "publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-288", kUnknownScenario,
         kUnknownEvaluator,
         {"local_setup_sent", "publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-288", kDuplicateScenario,
         kUnknownEvaluator,
         {"local_setup_sent", "publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-290", kDuplicateScenario,
         kDuplicateEvaluator,
         {"local_setup_sent", "publish_observed", "response_delivered"}},
        {21, "D21-9-1-1-MUST-293", kAuthorityScenario,
         kAuthorityEvaluator, {"local_setup_sent", "peer_closed"}},
        {21, "D21-9-1-2-MUST-300", kPathScenario,
         kPathEvaluator, {"local_setup_sent", "peer_closed"}},
    };
}

}  // namespace moq::interop::requirements
