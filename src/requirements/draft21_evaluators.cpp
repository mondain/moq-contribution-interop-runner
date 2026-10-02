#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft21_gap_a.h"
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/draft21_response.h"
#include "moq/interop/scenarios/fetch_probe.h"
#include "moq/interop/scenarios/fetch_response.h"
#include "moq/interop/scenarios/request_response.h"
#include "moq/interop/scenarios/range_filter.h"
#include "moq/interop/scenarios/subscription_cancel.h"
#include "moq/interop/scenarios/discovery_overlap.h"
#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/fetch_group_order.h"
#include "moq/interop/scenarios/immutable_repeat.h"
#include "moq/interop/scenarios/object_repeat.h"
#include "moq/interop/scenarios/request_goaway.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

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
constexpr const char* kNoAuthorityEvaluator =
    "d21-webtransport-no-authority-option";
constexpr const char* kNoPathEvaluator = "d21-webtransport-no-path-option";
constexpr const char* kMultiplicityEvaluator =
    "d21-setup-option-duplicates-only-when-permitted";

enum class SetupMultiplicity { Valid, Invalid, Indeterminate };

SetupMultiplicity setup_multiplicity(
    const std::vector<std::uint64_t>& option_types) {
    bool unknown_duplicate = false;
    for (std::size_t index = 1; index < option_types.size(); ++index) {
        const auto type = option_types[index];
        if (type != option_types[index - 1] || type == 3u) continue;
        if (type == 1u || type == 4u || type == 5u || type == 6u ||
            type == 7u || type == 8u) {
            return SetupMultiplicity::Invalid;
        }
        // An unknown extension can define its own repetition rule.
        unknown_duplicate = true;
    }
    return unknown_duplicate ? SetupMultiplicity::Indeterminate
                             : SetupMultiplicity::Valid;
}

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

struct RawResult {
    bool passed;
    std::string evaluator;
};

std::optional<RawResult> raw_result(
    const Requirement& row, const scenarios::RawProbeTranscript& transcript,
    const std::vector<scenarios::Draft21CloseProbe>& closes,
    const std::vector<scenarios::RequestProbeProfile>& requests,
    const std::vector<scenarios::Draft21PeerCloseProbe>& peers,
    const std::vector<scenarios::Draft21ResponseProbe>& responses,
    const std::vector<scenarios::FetchProbe>& fetches,
    const std::vector<scenarios::SubscriptionCancelProbe>& cancellations,
    const std::vector<scenarios::FetchResponseProbe>& fetch_responses,
    const std::vector<scenarios::RequestResponseProbe>& request_responses,
    const std::vector<scenarios::RangeFilterProbe>& ranges,
    const std::vector<scenarios::DiscoveryOverlapProbe>& overlaps,
    const std::vector<scenarios::FetchFirstObjectProbe>& first_fetches,
    const std::vector<scenarios::FetchGroupOrderProbe>& group_orders,
    const std::vector<scenarios::RequestGoawayProbe>& goaways,
    const std::vector<scenarios::ImmutableRepeatProbe>& immutable_profiles,
    const std::vector<scenarios::ObjectRepeatProbe>& object_profiles) {
    for (const auto& probe : closes) {
        if (row.id == probe.requirement_id &&
            transcript.scenario_id == probe.definition.id &&
            includes(row.scenarios, probe.definition.id.c_str()) &&
            includes(row.evaluators, probe.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_draft21_close_probe(transcript, probe);
            if (result) return RawResult{*result, probe.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : peers) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) && includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_raw_probe_close(transcript, profile.definition, profile.expected_close);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : requests) {
        if (row.id == profile.requirement_id &&
            transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_raw_probe_request_error(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : responses) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_draft21_response_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : fetches) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_fetch_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : immutable_profiles) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_immutable_repeat_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : object_profiles) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_object_repeat_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : goaways) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_request_goaway_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : group_orders) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_fetch_group_order_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : first_fetches) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_fetch_first_object_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : overlaps) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_discovery_overlap_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : ranges) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_range_filter_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : request_responses) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_request_response_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : fetch_responses) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_fetch_response_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    for (const auto& profile : cancellations) {
        if (row.id == profile.requirement_id && transcript.scenario_id == profile.definition.id &&
            includes(row.scenarios, profile.definition.id.c_str()) &&
            includes(row.evaluators, profile.evaluator_id.c_str())) {
            const auto result = scenarios::evaluate_subscription_cancel_probe(transcript, profile);
            if (result) return RawResult{*result, profile.evaluator_id};
            return std::nullopt;
        }
    }
    return std::nullopt;
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
    const bool peer_setup_received = std::any_of(
        context.evidence.begin(), context.evidence.end(),
        [](const scenarios::Draft21AnnouncementEvent& event) {
            return event.kind ==
                   scenarios::Draft21AnnouncementEventKind::PeerSetupReceived;
        });
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& requirement : catalog.requirements) {
        OutcomeState state = OutcomeState::NotRun;
        if (requirement.applicability != Applicability::Applicable) {
            state = OutcomeState::NotApplicable;
        } else if (requirement.testability == Testability::NotTestable) {
            state = OutcomeState::NotTestable;
        } else if (const auto gap = draft21_gap_a_announcement_state(requirement, context)) {
            state = *gap;
        } else if (requirement.id == "D21-9-1-MUST-NOT-289") {
            if (context.setup_probe == scenarios::Draft21SetupProbe::None &&
                peer_setup_received && includes(requirement.scenarios, kScenario) &&
                includes(requirement.evaluators, kMultiplicityEvaluator)) {
                const auto multiplicity =
                    setup_multiplicity(context.peer_setup_option_types);
                if (multiplicity == SetupMultiplicity::Invalid) {
                    state = OutcomeState::Fail;
                } else if (multiplicity == SetupMultiplicity::Valid && observed) {
                    state = OutcomeState::Pass;
                }
            }
        } else if (requirement.id == "D21-9-1-1-MUST-NOT-292" ||
                   requirement.id == "D21-9-1-2-MUST-NOT-299") {
            const bool authority =
                requirement.id == "D21-9-1-1-MUST-NOT-292";
            if (context.webtransport && peer_setup_received &&
                includes(requirement.scenarios, kScenario) &&
                includes(requirement.evaluators,
                         authority ? kNoAuthorityEvaluator : kNoPathEvaluator)) {
                const auto option_type = authority ? 5u : 1u;
                if (std::find(context.peer_setup_option_types.begin(),
                              context.peer_setup_option_types.end(),
                              option_type) !=
                    context.peer_setup_option_types.end()) {
                    state = OutcomeState::Fail;
                } else if (observed &&
                           context.setup_probe ==
                               scenarios::Draft21SetupProbe::None) {
                    state = OutcomeState::Pass;
                }
            }
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
                   requirement.id == "D21-9-1-1-MUST-294" ||
                   requirement.id == "D21-9-1-2-MUST-300" ||
                   requirement.id == "D21-9-1-2-MUST-301") {
            const bool webtransport_only =
                requirement.id == "D21-9-1-1-MUST-294" ||
                requirement.id == "D21-9-1-2-MUST-301";
            const bool authority =
                requirement.id == "D21-9-1-1-MUST-293" ||
                requirement.id == "D21-9-1-1-MUST-294";
            const auto expected_probe =
                authority ? scenarios::Draft21SetupProbe::ServerAuthority
                          : scenarios::Draft21SetupProbe::ServerPath;
            if ((!webtransport_only || context.webtransport) &&
                context.setup_probe == expected_probe &&
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
    std::vector<ExecutableBinding> bindings{
        {21, "D21-6-3-MUST-NOT-141", kScenario,
         kAllowedOpenerEvaluator, {"publish_observed", "response_delivered"}},
        {21, "D21-9-MUST-282", kScenario, kEvaluator,
         {"publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-NOT-289", kScenario,
         kMultiplicityEvaluator, {"peer_setup_received"}},
        {21, "D21-9-1-1-MUST-NOT-292", kScenario,
         kNoAuthorityEvaluator, {"peer_setup_received"}},
        {21, "D21-9-1-2-MUST-NOT-299", kScenario,
         kNoPathEvaluator, {"peer_setup_received"}},
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
        {21, "D21-9-1-1-MUST-294", kAuthorityScenario,
         kAuthorityEvaluator, {"local_setup_sent", "peer_closed"}},
        {21, "D21-9-1-2-MUST-300", kPathScenario,
         kPathEvaluator, {"local_setup_sent", "peer_closed"}},
        {21, "D21-9-1-2-MUST-301", kPathScenario,
         kPathEvaluator, {"local_setup_sent", "peer_closed"}},
    };
    for (const auto& probe : scenarios::draft21_close_probes()) {
        std::vector<std::string> evidence{"raw_probe_stimulus", "peer_close"};
        if (std::any_of(probe.definition.writes.begin(), probe.definition.writes.end(), [](const auto& write) {
                return write.reuse_write_stream.has_value();
            }))
            evidence.push_back("raw_probe_transport_event");
        bindings.push_back({21, probe.requirement_id, probe.definition.id,
            probe.evaluator_id, std::move(evidence)});
    }
    for (const auto& profile : scenarios::draft21_request_profiles()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id,
                           profile.evaluator_id, {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_peer_close_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event", "peer_close"}});
    }
    for (const auto& profile : scenarios::draft21_response_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_fetch_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_range_filter_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_request_response_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_fetch_response_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_immutable_repeat_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                            {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_object_repeat_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                            {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_request_goaway_probes()) {
        std::vector<std::string> evidence{"raw_probe_stimulus", "raw_probe_transport_event"};
        if (profile.duplicate) evidence.push_back("peer_close");
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                            std::move(evidence)});
    }
    for (const auto& profile : scenarios::draft21_fetch_group_order_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                            {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_fetch_first_object_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_discovery_overlap_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                          {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    for (const auto& profile : scenarios::draft21_subscription_cancel_probes()) {
        bindings.push_back({21, profile.requirement_id, profile.definition.id, profile.evaluator_id,
                           {"raw_probe_stimulus", "raw_probe_transport_event"}});
    }
    // Slice A completeness-gap bindings (draft21_gap_a.cpp).
    for (auto& binding : draft21_gap_a_bindings()) bindings.push_back(std::move(binding));
    return bindings;
}

std::vector<Outcome> evaluate_draft21_raw_probes(
    const RequirementCatalog& catalog,
    std::span<const scenarios::RawProbeTranscript> transcripts) {
    if (catalog.draft != 21 || !catalog.complete)
        throw std::invalid_argument("a complete draft-21 catalog is required");
    const auto closes = scenarios::draft21_close_probes();
    const auto requests = scenarios::draft21_request_profiles();
    const auto peers = scenarios::draft21_peer_close_probes();
    const auto responses = scenarios::draft21_response_probes();
    const auto fetches = scenarios::draft21_fetch_probes();
    const auto cancellations = scenarios::draft21_subscription_cancel_probes();
    const auto fetch_responses = scenarios::draft21_fetch_response_probes();
    const auto request_responses = scenarios::draft21_request_response_probes();
    const auto ranges = scenarios::draft21_range_filter_probes();
    const auto overlaps = scenarios::draft21_discovery_overlap_probes();
    const auto first_fetches = scenarios::draft21_fetch_first_object_probes();
    const auto group_orders = scenarios::draft21_fetch_group_order_probes();
    const auto goaways = scenarios::draft21_request_goaway_probes();
    const auto immutable_profiles = scenarios::draft21_immutable_repeat_probes();
    const auto object_profiles = scenarios::draft21_object_repeat_probes();
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& row : catalog.requirements) {
        auto state = OutcomeState::NotRun;
        if (row.applicability != Applicability::Applicable) {
            state = OutcomeState::NotApplicable;
        } else if (row.testability == Testability::NotTestable) {
            state = OutcomeState::NotTestable;
        } else {
            std::map<std::string, std::size_t> contexts;
            std::map<std::string, std::size_t> successful;
            std::set<std::string> exercised;
            for (const auto& transcript : transcripts) {
                if (includes(row.scenarios, transcript.scenario_id.c_str()))
                    ++contexts[transcript.scenario_id];
                const auto result = raw_result(row, transcript, closes, requests, peers, responses, fetches, cancellations, fetch_responses, request_responses, ranges, overlaps, first_fetches, group_orders, goaways, immutable_profiles, object_profiles);
                if (!result) continue;
                if (!result->passed) {
                    state = OutcomeState::Fail;
                    break;
                }
                ++successful[transcript.scenario_id];
                exercised.insert(result->evaluator);
            }
            if (state != OutcomeState::Fail && !row.scenarios.empty() && !row.evaluators.empty() &&
                std::all_of(row.scenarios.begin(), row.scenarios.end(),
                    [&](const auto& scenario) {
                        return contexts[scenario] == 1 && successful[scenario] == 1;
                    }) && std::all_of(row.evaluators.begin(), row.evaluators.end(),
                    [&](const auto& evaluator) { return exercised.contains(evaluator); })) {
                state = OutcomeState::Pass;
            }
        }
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

std::vector<Outcome> evaluate_draft21_close_probe(
    const RequirementCatalog& catalog,
    const scenarios::RawProbeTranscript& transcript) {
    return evaluate_draft21_raw_probes(catalog, {&transcript, 1});
}

std::vector<Outcome> evaluate_draft21_request_probe(
    const RequirementCatalog& catalog, const scenarios::RawProbeTranscript& transcript) {
    return evaluate_draft21_raw_probes(catalog, {&transcript, 1});
}

}  // namespace moq::interop::requirements
