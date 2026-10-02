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
    return std::nullopt;
}


std::vector<ExecutableBinding> draft21_gap_a_bindings() {
    return {
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
    };
}

}  // namespace moq::interop::requirements
