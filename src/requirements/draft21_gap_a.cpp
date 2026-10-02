#include "moq/interop/requirements/draft21_gap_a.h"

#include <algorithm>
#include <string>

namespace moq::interop::requirements {
namespace {

using scenarios::Draft21AnnouncementContext;
using scenarios::Draft21AnnouncementEvent;
using scenarios::Draft21AnnouncementEventKind;

bool listed(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

const scenarios::Draft21SetupOptionValue* option(
    const Draft21AnnouncementContext& context, std::uint64_t type) {
    for (const auto& value : context.peer_setup_options)
        if (value.type == type) return &value;
    return nullptr;
}

bool has_peer_setup(const Draft21AnnouncementContext& context) {
    return std::any_of(context.evidence.begin(), context.evidence.end(),
        [](const Draft21AnnouncementEvent& event) {
            return event.kind == Draft21AnnouncementEventKind::PeerSetupReceived;
        });
}

// A completed PUBLISH exchange on a publisher-opened request stream: the same
// correlation the original announcement evaluation requires for a pass.
bool exchange_observed(const Draft21AnnouncementContext& context) {
    if (!context.complete || !context.target_publish_seen || !context.response_delivered)
        return false;
    for (const auto& opening : context.evidence) {
        if (opening.kind != Draft21AnnouncementEventKind::PublishObserved ||
            !opening.stream_id || !opening.request_id ||
            (*opening.stream_id & 3u) != 0u) continue;
        const auto accepted = std::find_if(context.evidence.begin(), context.evidence.end(),
            [&](const Draft21AnnouncementEvent& event) {
                return event.kind == Draft21AnnouncementEventKind::ResponseDelivered &&
                       event.stream_id == opening.stream_id &&
                       event.request_id == opening.request_id;
            });
        if (accepted != context.evidence.end()) return true;
    }
    return false;
}

std::string text(const std::vector<std::byte>& bytes) {
    std::string result;
    for (const auto byte : bytes) result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    return result;
}

// Sections 9.1.1 and 9.1.2: a native-QUIC client connecting with a moqt URI
// sets AUTHORITY and PATH from that URI. Only a driven run knows the URI.
OutcomeState uri_option(const Draft21AnnouncementContext& context, std::uint64_t type,
                        const std::string& expected) {
    if (context.webtransport || !context.expected_uri || !has_peer_setup(context))
        return OutcomeState::NotRun;
    const auto* found = option(context, type);
    if (!found || !found->is_bytes || text(found->bytes) != expected) return OutcomeState::Fail;
    return exchange_observed(context) ? OutcomeState::Pass : OutcomeState::NotRun;
}

using Namespace = std::vector<std::vector<std::byte>>;

bool field_is(const Namespace& name_space, std::string_view value) {
    if (name_space.empty() || name_space.front().size() != value.size()) return false;
    for (std::size_t index = 0; index < value.size(); ++index)
        if (name_space.front()[index] != static_cast<std::byte>(static_cast<unsigned char>(value[index])))
            return false;
    return true;
}

bool begins_with_period(const Namespace& name_space) {
    return !name_space.empty() && !name_space.front().empty() &&
           name_space.front().front() == std::byte{'.'};
}

enum class Publication { Track, Namespace };

// Scores a "must not publish under namespace X" row. A publication observed
// on the wire is a failure. A pass needs a fully set up session that stayed
// alive through the whole observation window without such a publication; a
// publisher that never connected or closed early proves nothing.
template <class Forbidden>
OutcomeState absence_of_publication(const Draft21AnnouncementContext& context,
                                    Forbidden forbidden) {
    if (!has_peer_setup(context)) return OutcomeState::NotRun;
    for (const auto& event : context.evidence) {
        const bool track = event.kind == Draft21AnnouncementEventKind::PublishObserved;
        const bool name_space = event.kind == Draft21AnnouncementEventKind::NamespaceObserved;
        if (!track && !name_space) continue;
        if (forbidden(track ? Publication::Track : Publication::Namespace, event.track_namespace))
            return OutcomeState::Fail;
    }
    return context.window_elapsed ? OutcomeState::Pass : OutcomeState::NotRun;
}

bool malformed_detail_contains(const Draft21AnnouncementContext& context,
                               std::string_view needle) {
    return std::any_of(context.evidence.begin(), context.evidence.end(),
        [&](const Draft21AnnouncementEvent& event) {
            return event.kind == Draft21AnnouncementEventKind::MalformedPublisherMessage &&
                   event.detail.find(needle) != std::string::npos;
        });
}

}  // namespace

std::optional<OutcomeState> draft21_gap_a_announcement_state(
    const Requirement& row, const Draft21AnnouncementContext& context) {
    const auto& id = row.id;
    if (context.setup_probe != scenarios::Draft21SetupProbe::None) return std::nullopt;
    const auto owns = [&](std::string_view scenario, std::string_view evaluator) {
        return listed(row.scenarios, scenario) && listed(row.evaluators, evaluator) &&
               context.scenario_id == scenario;
    };
    if (id == "D21-9-1-1-MUST-296") {
        if (!owns("d21-native-publisher-uri-options", "d21-authority-matches-connection-uri"))
            return std::nullopt;
        return uri_option(context, 5, context.expected_uri ? context.expected_uri->authority : "");
    }
    if (id == "D21-9-1-2-MUST-303") {
        if (!owns("d21-native-publisher-uri-options", "d21-path-matches-uri-path-abempty"))
            return std::nullopt;
        // The default scenario URI carries no query, so PATH is the whole path.
        return uri_option(context, 1, context.expected_uri ? context.expected_uri->path_and_query : "");
    }
    if (id == "D21-9-1-2-MUST-304") {
        if (!owns("d21-native-publisher-uri-query", "d21-path-query-concatenation") &&
            !owns("d21-native-publisher-empty-query", "d21-path-query-concatenation"))
            return std::nullopt;
        // The literal separator is required even when the query is empty.
        if (!context.expected_uri ||
            context.expected_uri->path_and_query.find('?') == std::string::npos)
            return OutcomeState::NotRun;
        return uri_option(context, 1, context.expected_uri->path_and_query);
    }
    if (id == "D21-6-3-2-MUST-150") {
        if (owns("d21-native-quic-required-setup-options", "d21-required-version-setup-options")) {
            // Native QUIC with a moqt URI: AUTHORITY and PATH are both required.
            if (context.webtransport || !context.expected_uri || !has_peer_setup(context))
                return OutcomeState::NotRun;
            if (!option(context, 5) || !option(context, 1)) return OutcomeState::Fail;
            return exchange_observed(context) ? OutcomeState::Pass : OutcomeState::NotRun;
        }
        if (owns("d21-webtransport-required-setup-options", "d21-required-version-setup-options")) {
            // Section 9.1 requires no Setup Option over WebTransport; a
            // forbidden AUTHORITY or PATH is scored by its own rows.
            if (!context.webtransport || !has_peer_setup(context) || option(context, 5) ||
                option(context, 1))
                return OutcomeState::NotRun;
            return exchange_observed(context) ? OutcomeState::Pass : OutcomeState::NotRun;
        }
        return std::nullopt;
    }
    if (id == "D21-2-4-2-MUST-NOT-026") {
        if (!owns("d21-attempt-unregistered-period-namespace-publication",
                  "d21-no-unregistered-reserved-namespace-publication")) return std::nullopt;
        // ".session" is the one namespace this draft registers (Section 6.5).
        return absence_of_publication(context, [](Publication, const Namespace& name_space) {
            return begins_with_period(name_space) && !field_is(name_space, ".session");
        });
    }
    if (id == "D21-2-4-2-MUST-NOT-028") {
        if (!owns("d21-attempt-single-period-namespace-use",
                  "d21-no-originated-use-of-single-period-namespace")) return std::nullopt;
        return absence_of_publication(context, [](Publication, const Namespace& name_space) {
            return field_is(name_space, ".");
        });
    }
    if (id == "D21-2-4-2-MUST-NOT-029") {
        if (!owns("d21-attempt-single-period-track-publication",
                  "d21-no-tracks-under-single-period")) return std::nullopt;
        return absence_of_publication(context, [](Publication kind, const Namespace& name_space) {
            return kind == Publication::Track && field_is(name_space, ".");
        });
    }
    if (id == "D21-2-4-2-MUST-NOT-030") {
        if (!owns("d21-attempt-single-period-namespace-publication",
                  "d21-no-namespaces-under-single-period")) return std::nullopt;
        return absence_of_publication(context, [](Publication kind, const Namespace& name_space) {
            return kind == Publication::Namespace && field_is(name_space, ".");
        });
    }
    if (id == "D21-6-5-MUST-NOT-166") {
        if (!owns("d21-application-track-publication-under-session",
                  "d21-no-application-track-publication-under-session")) return std::nullopt;
        return absence_of_publication(context, [](Publication kind, const Namespace& name_space) {
            return kind == Publication::Track && field_is(name_space, ".session");
        });
    }
    if (id == "D21-6-5-MUST-NOT-167") {
        if (!owns("d21-application-namespace-publication-under-session",
                  "d21-no-application-namespace-publication-under-session")) return std::nullopt;
        return absence_of_publication(context, [](Publication kind, const Namespace& name_space) {
            return kind == Publication::Namespace && field_is(name_space, ".session");
        });
    }
    if (id == "D21-8-3-MUST-NOT-230") {
        if (!owns("d21-publisher-key-value-type-deltas", "d21-emitted-key-value-types-within-uint64"))
            return std::nullopt;
        // SETUP options, PUBLISH parameters and Track Properties are all
        // Key-Value-Pairs lists whose decoder checks the running type sum.
        if (malformed_detail_contains(context, "type overflow")) return OutcomeState::Fail;
        return has_peer_setup(context) && exchange_observed(context) ? OutcomeState::Pass
                                                                     : OutcomeState::NotRun;
    }
    if (id == "D21-8-7-MUST-250") {
        if (!owns("d21-publisher-emitted-namespace-fields", "d21-emitted-namespace-fields-nonempty"))
            return std::nullopt;
        if (malformed_detail_contains(context, "empty draft-21 namespace field"))
            return OutcomeState::Fail;
        // A pass needs at least one decoded namespace field to have been emitted.
        const bool decoded_field = std::any_of(context.evidence.begin(), context.evidence.end(),
            [](const Draft21AnnouncementEvent& event) {
                return event.kind == Draft21AnnouncementEventKind::PublishObserved &&
                       !event.track_namespace.empty();
            });
        return decoded_field && has_peer_setup(context) ? OutcomeState::Pass
                                                        : OutcomeState::NotRun;
    }
    if (id == "D21-7-5-MUST-206") {
        if (!owns("d21-publisher-namespace-routing-announcement",
                  "d21-explicit-namespace-publication-for-routing")) return std::nullopt;
        // Only an explicit PUBLISH_NAMESPACE for the configured namespace
        // satisfies the duty; a bare PUBLISH never does. Absence is not
        // scored as a failure because the wire cannot show that the publisher
        // intended to request routing.
        return context.namespace_announced && has_peer_setup(context) ? OutcomeState::Pass
                                                                      : OutcomeState::NotRun;
    }
    return std::nullopt;
}


bool draft21_gap_a_scenarios_are_alternatives(const Requirement& row) {
    // Native vs WebTransport (6.2), and a publisher's own Track Alias assignment
    // (3.1): only one of the two scenarios of those rows can occur in one run.
    return row.id == "D21-6-2-MUST-139" || row.id == "D21-6-2-MUST-140" ||
           row.id == "D21-3-1-MUST-041";
}

std::optional<Draft21GapARawResult> draft21_gap_a_raw_result(
    const Requirement& row, const scenarios::RawProbeTranscript& transcript,
    const std::vector<scenarios::Draft21GapProbe>& probes,
    const std::vector<scenarios::Draft21TokenProbe>& token_probes) {
    for (const auto& probe : token_probes) {
        if (row.id != probe.requirement_id || transcript.scenario_id != probe.definition.id ||
            !listed(row.scenarios, probe.definition.id) || !listed(row.evaluators, probe.evaluator_id))
            continue;
        const auto verdict = scenarios::evaluate_draft21_gap_a_token_probe(transcript, probe);
        if (!verdict) return std::nullopt;
        return Draft21GapARawResult{*verdict, probe.evaluator_id};
    }
    for (const auto& probe : probes) {
        if (row.id != probe.requirement_id || transcript.scenario_id != probe.definition.id ||
            !listed(row.scenarios, probe.definition.id) || !listed(row.evaluators, probe.evaluator_id))
            continue;
        const auto verdict = scenarios::evaluate_draft21_gap_a_probe(transcript, probe);
        if (!verdict) return std::nullopt;
        return Draft21GapARawResult{*verdict, probe.evaluator_id};
    }
    return std::nullopt;
}

std::vector<ExecutableBinding> draft21_gap_a_bindings() {
    std::vector<ExecutableBinding> bindings{
        {21, "D21-6-3-MUST-NOT-141", "d21-publisher-request-stream-openers",
         "d21-request-stream-first-message-allowed",
         {"publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-NOT-289", "d21-publisher-setup-option-multiplicity",
         "d21-setup-option-duplicates-only-when-permitted",
         {"peer_setup_received"}},
        {21, "D21-9-1-1-MUST-NOT-292", "d21-webtransport-publisher-setup",
         "d21-webtransport-no-authority-option", {"peer_setup_received"}},
        {21, "D21-9-1-2-MUST-NOT-299", "d21-webtransport-publisher-setup",
         "d21-webtransport-no-path-option", {"peer_setup_received"}},
        {21, "D21-9-1-1-MUST-294", "d21-webtransport-server-sends-authority",
         "d21-webtransport-authority-invalid-authority",
         {"local_setup_sent", "peer_closed"}},
        {21, "D21-9-1-2-MUST-301", "d21-webtransport-server-sends-path",
         "d21-webtransport-path-invalid-path",
         {"local_setup_sent", "peer_closed"}},
        {21, "D21-9-1-1-MUST-296", "d21-native-publisher-uri-options",
         "d21-authority-matches-connection-uri", {"peer_setup_received"}},
        {21, "D21-9-1-2-MUST-303", "d21-native-publisher-uri-options",
         "d21-path-matches-uri-path-abempty", {"peer_setup_received"}},
        {21, "D21-9-1-2-MUST-304", "d21-native-publisher-uri-query",
         "d21-path-query-concatenation", {"peer_setup_received"}},
        {21, "D21-9-1-2-MUST-304", "d21-native-publisher-empty-query",
         "d21-path-query-concatenation", {"peer_setup_received"}},
        {21, "D21-6-3-2-MUST-150", "d21-native-quic-required-setup-options",
         "d21-required-version-setup-options", {"peer_setup_received"}},
        {21, "D21-6-3-2-MUST-150", "d21-webtransport-required-setup-options",
         "d21-required-version-setup-options", {"peer_setup_received"}},
        {21, "D21-2-4-2-MUST-NOT-026", "d21-attempt-unregistered-period-namespace-publication",
         "d21-no-unregistered-reserved-namespace-publication",
         {"local_setup_sent", "peer_setup_received"}},
        {21, "D21-2-4-2-MUST-NOT-028", "d21-attempt-single-period-namespace-use",
         "d21-no-originated-use-of-single-period-namespace",
         {"local_setup_sent", "peer_setup_received"}},
        {21, "D21-2-4-2-MUST-NOT-029", "d21-attempt-single-period-track-publication",
         "d21-no-tracks-under-single-period",
         {"local_setup_sent", "peer_setup_received"}},
        {21, "D21-2-4-2-MUST-NOT-030", "d21-attempt-single-period-namespace-publication",
         "d21-no-namespaces-under-single-period",
         {"local_setup_sent", "peer_setup_received"}},
        {21, "D21-6-5-MUST-NOT-166", "d21-application-track-publication-under-session",
         "d21-no-application-track-publication-under-session",
         {"local_setup_sent", "peer_setup_received"}},
        {21, "D21-6-5-MUST-NOT-167", "d21-application-namespace-publication-under-session",
         "d21-no-application-namespace-publication-under-session",
         {"local_setup_sent", "peer_setup_received"}},
        {21, "D21-8-3-MUST-NOT-230", "d21-publisher-key-value-type-deltas",
         "d21-emitted-key-value-types-within-uint64",
         {"peer_setup_received", "publish_observed", "response_delivered"}},
        {21, "D21-8-7-MUST-250", "d21-publisher-emitted-namespace-fields",
         "d21-emitted-namespace-fields-nonempty",
         {"peer_setup_received", "publish_observed"}},
        {21, "D21-7-5-MUST-206", "d21-publisher-namespace-routing-announcement",
         "d21-explicit-namespace-publication-for-routing",
         {"peer_setup_received", "namespace_observed", "namespace_response_delivered"}},
    };
    for (const auto& probe : scenarios::draft21_gap_a_probes())
        bindings.push_back({21, probe.requirement_id, probe.definition.id, probe.evaluator_id,
                            {"raw_probe_stimulus", "raw_probe_transport_event"}});
    for (const auto& probe : scenarios::draft21_gap_a_token_probes())
        bindings.push_back({21, probe.requirement_id, probe.definition.id, probe.evaluator_id,
                            {"raw_probe_stimulus", "raw_probe_transport_event"}});
    return bindings;
}

}  // namespace moq::interop::requirements
