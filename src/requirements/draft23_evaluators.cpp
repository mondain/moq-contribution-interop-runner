#include "moq/interop/requirements/draft23_evaluators.h"

#include "moq/interop/session/draft23_probes.h"
#include "moq/interop/wire/cursor.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <variant>

namespace moq::interop::requirements {
namespace {

constexpr std::string_view kScenarios[] = {"d23-publisher-smoke", "d23-direct-subscribe-smoke"};

constexpr std::uint64_t kPathOption = 1;
constexpr std::uint64_t kAuthorityOption = 5;
constexpr std::uint64_t kMaxControlMessageSizeOption = 9;

struct Row {
    std::string_view requirement;
    std::string_view evaluator;
    std::string_view evidence = "peer_setup_received";
};
constexpr Row kMultiplicity{"D23-9-1-MUST-NOT-265", "d23-setup-option-multiplicity"};
constexpr Row kMaxSize{"D23-9-1-8-MUST-288", "d23-setup-max-control-message-size"};
constexpr Row kAuthority{"D23-9-1-1-MUST-269", "d23-setup-native-authority"};
constexpr Row kPath{"D23-9-1-2-MUST-273", "d23-setup-native-path"};
constexpr Row kQuery{"D23-9-1-2-MUST-274", "d23-setup-native-path-query"};
constexpr Row kBitFour{"D23-11-3-1-MUST-445", "d23-subgroup-header-bit4", "peer_stream_classified"};
constexpr Row kOneStream{"D23-2-2-MUST-NOT-020", "d23-subgroup-single-stream", "peer_stream_classified"};
constexpr Row kLength{"D23-9-MUST-NOT-259", "d23-control-message-length", "peer_setup_received,request_observed"};
constexpr Row kRequestId{"D23-6-4-2-1-MUST-NOT-147", "d23-request-id-unique", "request_observed"};
constexpr Row kSingleResponse{"D23-3-1-MUST-036", "d23-subscribe-single-response", "request_observed"};
constexpr Row kFinRequired{"D23-6-4-2-2-MUST-NOT-150", "d23-request-stream-fin-after-required", "request_observed"};
constexpr Row kResponseBeforeFin{"D23-6-4-2-2-MUST-151", "d23-subscribe-response-before-fin", "request_observed"};
constexpr Row kDoneBeforeFin{"D23-6-4-2-2-MUST-152", "d23-publish-done-before-fin", "request_observed"};
constexpr Row kStreamCount{"D23-9-9-MUST-342", "d23-publish-done-stream-count", "request_observed,peer_stream_classified"};
// Negative probes: every row, scenario, evaluator id and expected outcome comes from session::draft23_probes().
const session::Draft23Probe* probe_for(std::string_view requirement) {
    for (const auto& probe : session::draft23_probes())
        for (const auto row : session::draft23_probe_rows(probe)) if (row == requirement) return &probe;
    return nullptr;
}
// Rows judged on the REQUEST_ERROR reply of every reply probe, not on one probe of their own.
constexpr Row kReplyRows[] = {
    {"D23-9-4-2-MUST-NOT-311", "d23-error-details-no-repeat", "request_observed"},
    {"D23-9-4-2-1-MUST-313", "d23-redirect-detail-present", "request_observed"},
    {"D23-6-4-2-3-SHOULD-157", "d23-reject-with-error-and-fin", "request_observed"},
};
constexpr std::uint64_t kRedirectCode = 0x34;
constexpr std::uint64_t kRedirectDetail = 1;
constexpr Row kTrackName{"D23-2-4-1-MUST-028", "d23-track-name-not-empty", "request_observed"};
constexpr Row kNamespaceField{"D23-8-7-MUST-232", "d23-namespace-field-not-empty", "request_observed"};
constexpr Row kReservedPublish{"D23-2-4-3-MUST-NOT-033", "d23-no-reserved-namespace", "request_observed"};
constexpr Row kReservedAnyPurpose{"D23-2-4-3-MUST-NOT-032", "d23-no-reserved-namespace", "request_observed"};
constexpr Row kSessionNamespace{"D23-6-5-MUST-NOT-158", "d23-no-session-namespace", "request_observed"};
constexpr Row kAliasUnique{"D23-3-1-3-MUST-NOT-050", "d23-track-alias-unique", "request_observed"};
constexpr Row kDatagram{"D23-6-2-MUST-135", "d23-quic-datagram-negotiated", "transport_established"};
constexpr Row kFirstBidi{"D23-6-3-MUST-NOT-136", "d23-first-bidi-message-type", "request_observed"};
constexpr Row kControlOpen{"D23-6-3-MUST-NOT-140", "d23-control-stream-stays-open", "request_observed"};
constexpr Row kNoOverflow210{"D23-8-3-MUST-NOT-210", "d23-delta-encoding-no-overflow", "request_observed"};
constexpr Row kNoOverflow351{"D23-9-19-MUST-351", "d23-delta-encoding-no-overflow", "request_observed"};
constexpr Row kNoDuplicateParameter{"D23-9-19-MUST-NOT-355", "d23-no-duplicate-parameter", "request_observed"};
constexpr Row kAnsweredOk{"D23-9-6-MUST-332", "d23-subscribe-answered-ok", "request_observed"};
constexpr Row kZeroCount{"D23-9-9-MUST-341", "d23-publish-done-zero-stream-count", "request_observed,peer_stream_classified"};
constexpr Row kPadding{"D23-11-5-1-MUST-475", "d23-padding-all-zero", "peer_stream_classified"};
constexpr Row kFinAfterDone{"D23-6-4-2-2-SHOULD-154", "d23-fin-after-publish-done", "request_observed"};
constexpr Row kImplementationSent{"D23-9-1-5-SHOULD-281", "d23-implementation-option-sent", "peer_setup_received"};
constexpr Row kImplementationText{"D23-9-1-5-SHOULD-280", "d23-implementation-option-text", "peer_setup_received"};
constexpr Row kAliasReuse{"D23-3-1-3-SHOULD-NOT-052", "d23-track-alias-unique", "request_observed"};
constexpr Row kStreamSpread{"D23-11-3-SHOULD-NOT-443", "d23-subgroup-single-stream", "peer_stream_classified"};
constexpr Row kRows[] = {kFinAfterDone, kImplementationSent, kImplementationText, kAliasReuse, kStreamSpread, kTrackName, kNamespaceField, kReservedPublish, kReservedAnyPurpose, kSessionNamespace, kAliasUnique,
                         kDatagram, kFirstBidi, kControlOpen, kNoOverflow210, kNoOverflow351, kNoDuplicateParameter,
                         kAnsweredOk, kZeroCount, kPadding, kMultiplicity, kMaxSize, kAuthority, kPath, kQuery, kBitFour, kOneStream, kLength, kRequestId,
                         kSingleResponse, kFinRequired, kResponseBeforeFin, kDoneBeforeFin, kStreamCount};

// The state of a row no SETUP evaluator judges.
OutcomeState staged_state(const Requirement& row) {
    if (!row.reviewed) return OutcomeState::NotRun;
    switch (row.testability) {
        case Testability::NotTestable: return OutcomeState::NotTestable;
        case Testability::NotApplicable: return OutcomeState::NotApplicable;
        case Testability::Testable: return OutcomeState::NotRun;
    }
    return OutcomeState::NotRun;
}

OutcomeState verdict(bool ok) { return ok ? OutcomeState::Pass : OutcomeState::Fail; }

std::vector<const wire::draft21::KeyValue*> options_of(const session::Draft23PeerSetup& setup, std::uint64_t type) {
    std::vector<const wire::draft21::KeyValue*> found;
    for (const auto& option : setup.options) if (option.type == type) found.push_back(&option);
    return found;
}

std::optional<std::string> text_of(const wire::draft21::KeyValue& option) {
    const auto* bytes = std::get_if<std::vector<std::byte>>(&option.value);
    if (!bytes) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

// Option types whose definition allows a single instance per SETUP (Section 9.1); AUTHORIZATION TOKEN (0x03) may repeat.
bool single_instance_option(std::uint64_t type) { return type == kPathOption || (type >= 4 && type <= 9); }

OutcomeState multiplicity(const session::Draft23PeerSetup& setup) {
    std::map<std::uint64_t, int> counts;
    for (const auto& option : setup.options) if (single_instance_option(option.type)) ++counts[option.type];
    return verdict(std::none_of(counts.begin(), counts.end(), [](const auto& entry) { return entry.second > 1; }));
}

OutcomeState max_size(const session::Draft23PeerSetup& setup) {
    const auto found = options_of(setup, kMaxControlMessageSizeOption);
    if (found.empty()) return OutcomeState::NotApplicable;  // nothing sent, so no value to constrain
    const auto* bytes = std::get_if<std::vector<std::byte>>(&found.front()->value);
    if (!bytes) return OutcomeState::Fail;
    wire::Cursor input(*bytes);
    const auto decoded = wire::read_vi64(input);
    const auto* value = std::get_if<std::uint64_t>(&decoded);
    return verdict(value && input.remaining() == 0 && *value >= 65535);
}

OutcomeState authority(const session::Draft23PeerSetup& setup, const Draft23Evidence& context) {
    const auto found = options_of(setup, kAuthorityOption);
    return verdict(found.size() == 1 && text_of(*found.front()) == context.authority);
}

// PATH is the path-abempty, then a question mark and the query when the URI has one (Section 9.1.2).
std::optional<std::string> path_option(const session::Draft23PeerSetup& setup) {
    const auto found = options_of(setup, kPathOption);
    if (found.size() != 1) return std::nullopt;
    return text_of(*found.front());
}

OutcomeState path(const session::Draft23PeerSetup& setup, const Draft23Evidence& context) {
    const auto value = path_option(setup);
    return verdict(value && value->substr(0, value->find('?')) == context.path);
}

OutcomeState query(const session::Draft23PeerSetup& setup, const Draft23Evidence& context) {
    if (!context.query) return OutcomeState::NotApplicable;
    const auto value = path_option(setup);
    if (!value) return OutcomeState::Fail;
    const auto mark = value->find('?');
    return verdict(mark != std::string::npos && value->substr(mark + 1) == *context.query);
}

// Bit 4 of a SUBGROUP_HEADER type is always 1 (Section 11.3.1). The FETCH header type (5), types that need more than
// one byte and the reserved subgroup mode are other rules' business.
OutcomeState bit_four(const std::vector<session::Draft23DataStream>& streams) {
    if (streams.empty()) return OutcomeState::NotRun;
    return verdict(std::none_of(streams.begin(), streams.end(), [](const session::Draft23DataStream& stream) {
        return !stream.valid_header && stream.type_flags < 128 && (stream.type_flags & 0x10) == 0 && stream.type_flags != 5;
    }));
}

// One Subgroup is not spread over several streams unless a stream was reset (Section 2.2).
OutcomeState single_stream(const std::vector<session::Draft23DataStream>& streams) {
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, int> live;
    bool judged = false;
    for (const auto& stream : streams) {
        if (!stream.valid_header || !stream.subgroup_id) continue;
        judged = true;
        if (stream.reset) continue;
        if (++live[{stream.track_alias, stream.group_id, *stream.subgroup_id}] > 1) return OutcomeState::Fail;
    }
    return judged ? OutcomeState::Pass : OutcomeState::NotRun;
}

constexpr std::uint64_t kSubscribeOk = 4;
constexpr std::uint64_t kRequestError = 5;

bool is_response(const session::Draft23ControlMessage& message) { return message.type == kSubscribeOk || message.type == kRequestError; }

const session::Draft23RequestStream* subscribe_stream(const Draft23Evidence& context) {
    for (const auto& request : context.requests) if (request.local) return &request;
    return nullptr;
}

// The limit is the default of 65535: the runner sends no MAX_CONTROL_MESSAGE_SIZE option.
OutcomeState control_length(const Draft23Evidence& context) {
    constexpr std::uint64_t kDefaultLimit = 65535;
    bool seen = false;
    if (context.peer_setup) {
        seen = true;
        if (context.peer_setup->frame_length > kDefaultLimit) return OutcomeState::Fail;
    }
    for (const auto& request : context.requests) {
        for (const auto& message : request.messages) {
            seen = true;
            if (message.body_length > kDefaultLimit) return OutcomeState::Fail;
        }
    }
    if (context.oversize_control_message) return OutcomeState::Fail;
    return seen ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState request_id_unique(const Draft23Evidence& context) {
    if (context.duplicate_request_id) return OutcomeState::Fail;
    const bool seen = std::any_of(context.requests.begin(), context.requests.end(),
                                  [](const session::Draft23RequestStream& request) { return request.request_id.has_value(); });
    return seen ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState single_response(const Draft23Evidence& context) {
    if (!context.direct_subscribe) return OutcomeState::NotApplicable;
    const auto* stream = subscribe_stream(context);
    if (!stream) return OutcomeState::NotRun;
    const auto responses = std::count_if(stream->messages.begin(), stream->messages.end(), is_response);
    if (responses > 1) return OutcomeState::Fail;
    return responses == 1 ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState response_before_fin(const Draft23Evidence& context) {
    if (!context.direct_subscribe) return OutcomeState::NotApplicable;
    const auto* stream = subscribe_stream(context);
    if (!stream) return OutcomeState::NotRun;
    if (std::any_of(stream->messages.begin(), stream->messages.end(), is_response)) return OutcomeState::Pass;
    return stream->fin ? OutcomeState::Fail : OutcomeState::NotRun;
}

// The rule forbids a FIN before PUBLISH_DONE. A FIN that precedes it fails; once PUBLISH_DONE has arrived with no earlier
// FIN, the ordering holds whether or not the publisher ever closes its half of the stream.
OutcomeState done_before_fin(const Draft23Evidence& context) {
    bool passed = false;
    for (const auto& request : context.requests) {
        if (!request.established) continue;
        if (request.fin && !request.fin_after_publish_done) return OutcomeState::Fail;
        if (request.publish_done) passed = true;
    }
    return passed ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState fin_after_required(const Draft23Evidence& context) {
    const auto response = response_before_fin(context);
    const auto done = done_before_fin(context);
    if (response == OutcomeState::Fail || done == OutcomeState::Fail) return OutcomeState::Fail;
    // Every applicable half must be decided: an answered subscription whose stream is still open has not yet shown
    // that PUBLISH_DONE precedes the FIN.
    const bool response_decided = response == OutcomeState::Pass || response == OutcomeState::NotApplicable;
    if (response_decided && done == OutcomeState::Pass) return OutcomeState::Pass;
    return OutcomeState::NotRun;
}

// Stream Count is exact, or 2^64-1 when the publisher cannot count (Section 9.9). More streams received than counted
// is certain; fewer is inconclusive because a counted stream may still be in flight.
OutcomeState stream_count(const Draft23Evidence& context) {
    const session::Draft23PublishDone* done = nullptr;
    for (const auto& request : context.requests) if (request.established && request.publish_done) done = &*request.publish_done;
    if (!done || !context.subscription_alias) return OutcomeState::NotRun;
    if (done->stream_count == std::numeric_limits<std::uint64_t>::max()) return OutcomeState::Pass;
    const auto received = static_cast<std::uint64_t>(std::count_if(
        context.data_streams.begin(), context.data_streams.end(), [&](const session::Draft23DataStream& stream) {
            return stream.valid_header && stream.track_alias == *context.subscription_alias;
        }));
    if (received > done->stream_count) return OutcomeState::Fail;
    return received == done->stream_count ? OutcomeState::Pass : OutcomeState::NotRun;
}

bool saw_any_namespace(const Draft23Evidence& context) {
    return !context.published_tracks.empty() || !context.announced_namespaces.empty();
}
bool published_under(const Draft23Evidence& context, std::string_view first_field) {
    return std::any_of(context.announced_namespaces.begin(), context.announced_namespaces.end(),
                       [&](const std::string& name) { return name == first_field; }) ||
           std::any_of(context.published_tracks.begin(), context.published_tracks.end(),
                       [&](const session::Draft23PublishedTrack& track) { return track.namespace_first == first_field; });
}

OutcomeState track_name_not_empty(const Draft23Evidence& context) {
    if (context.empty_track_name) return OutcomeState::Fail;
    return context.published_tracks.empty() ? OutcomeState::NotRun : OutcomeState::Pass;
}

OutcomeState namespace_field_not_empty(const Draft23Evidence& context) {
    if (context.empty_namespace_field) return OutcomeState::Fail;
    return saw_any_namespace(context) ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState not_published_under(const Draft23Evidence& context, std::string_view first_field) {
    if (published_under(context, first_field)) return OutcomeState::Fail;
    return saw_any_namespace(context) ? OutcomeState::Pass : OutcomeState::NotRun;
}

// One Track Alias names one Track at a time (Section 3.1.3). Two PUBLISH messages for the same track are not a conflict.
OutcomeState alias_unique(const Draft23Evidence& context) {
    std::map<std::uint64_t, std::set<std::pair<std::string, std::string>>> tracks_by_alias;
    std::set<std::pair<std::string, std::string>> distinct;
    for (const auto& track : context.published_tracks) {
        const auto key = std::make_pair(track.namespace_first, track.name);
        tracks_by_alias[track.alias].insert(key);
        distinct.insert(key);
    }
    for (const auto& entry : tracks_by_alias) if (entry.second.size() > 1) return OutcomeState::Fail;
    return distinct.size() >= 2 ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState datagram_negotiated(const Draft23Evidence& context) {
    if (context.max_datagram_payload > 0) return OutcomeState::Pass;
    return context.connected ? OutcomeState::Fail : OutcomeState::NotRun;
}

// Section 6.3: the seven request openers.
bool request_opener(std::uint64_t type) {
    return type == 0xd || type == 0x3 || type == 0x1d || type == 0x16 || type == 0x6 || type == 0x50 || type == 0x51;
}

OutcomeState first_bidi_message(const Draft23Evidence& context) {
    bool seen = false;
    for (const auto& request : context.requests) {
        if (request.local || request.messages.empty()) continue;
        seen = true;
        if (!request_opener(request.messages.front().type)) return OutcomeState::Fail;
    }
    return seen ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState control_stream_open(const Draft23Evidence& context) {
    if (context.control_stream_closed) return OutcomeState::Fail;
    return context.peer_setup ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState no_delta_overflow(const Draft23Evidence& context) {
    if (context.delta_overflow) return OutcomeState::Fail;
    return (context.peer_setup || !context.requests.empty()) ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState no_duplicate_parameter(const Draft23Evidence& context) {
    if (context.duplicate_parameter) return OutcomeState::Fail;
    return saw_any_namespace(context) ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState answered_ok(const Draft23Evidence& context) {
    if (!context.direct_subscribe) return OutcomeState::NotApplicable;
    const auto* stream = subscribe_stream(context);
    if (!stream) return OutcomeState::NotRun;
    for (const auto& message : stream->messages) {
        if (message.type == kSubscribeOk) return OutcomeState::Pass;
        if (message.type == kRequestError) return OutcomeState::Fail;
    }
    return OutcomeState::NotRun;
}

// Streams that arrived for the established subscription's Track Alias.
std::uint64_t streams_for_subscription(const Draft23Evidence& context) {
    return static_cast<std::uint64_t>(std::count_if(
        context.data_streams.begin(), context.data_streams.end(), [&](const session::Draft23DataStream& stream) {
            return stream.valid_header && context.subscription_alias && stream.track_alias == *context.subscription_alias;
        }));
}

OutcomeState zero_stream_count(const Draft23Evidence& context) {
    const session::Draft23PublishDone* done = nullptr;
    for (const auto& request : context.requests) if (request.established && request.publish_done) done = &*request.publish_done;
    if (!done || !context.subscription_alias) return OutcomeState::NotRun;
    if (streams_for_subscription(context) > 0) return OutcomeState::NotApplicable;
    return done->stream_count == 0 ? OutcomeState::Pass : OutcomeState::NotRun;
}

OutcomeState padding_zero(const Draft23Evidence& context) {
    if (context.padding_nonzero) return OutcomeState::Fail;
    return context.padding_streams > 0 ? OutcomeState::Pass : OutcomeState::NotRun;
}

// Section 6.4.2.2 recommends a FIN promptly once there is nothing more to send: after PUBLISH_DONE, the publisher's stream.
OutcomeState fin_after_done_should(const Draft23Evidence& context) {
    bool done = false;
    for (const auto& request : context.requests) {
        if (!request.established || !request.publish_done) continue;
        if (!request.fin_after_publish_done) return OutcomeState::Fail;
        done = true;
    }
    return done ? OutcomeState::Pass : OutcomeState::NotRun;
}

const wire::draft21::KeyValue* implementation_option(const Draft23Evidence& context) {
    if (!context.peer_setup) return nullptr;
    for (const auto& option : context.peer_setup->options) if (option.type == 7) return &option;  // MOQT_IMPLEMENTATION
    return nullptr;
}

OutcomeState implementation_sent(const Draft23Evidence& context) {
    if (!context.peer_setup) return OutcomeState::NotRun;
    return verdict(implementation_option(context) != nullptr);
}

bool valid_utf8(const std::vector<std::byte>& bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = std::to_integer<unsigned>(bytes[i]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 0;
        if (length == 0 || i + length > bytes.size()) return false;
        for (std::size_t k = 1; k < length; ++k) if ((std::to_integer<unsigned>(bytes[i + k]) & 0xc0) != 0x80) return false;
        i += length;
    }
    return true;
}

OutcomeState implementation_text(const Draft23Evidence& context) {
    const auto* option = implementation_option(context);
    if (!option) return context.peer_setup ? OutcomeState::NotApplicable : OutcomeState::NotRun;
    const auto* bytes = std::get_if<std::vector<std::byte>>(&option->value);
    return verdict(bytes && valid_utf8(*bytes));
}

// A probe row is judged only on the run of its own probe. The publisher must close the session with the expected
// application code; any other close, or none, fails. A probe that was never sent proves nothing.
std::optional<OutcomeState> judge_probe(const session::Draft23Probe& probe, const Draft23Evidence& context) {
    if (!context.probe_scenario || *context.probe_scenario != probe.scenario_id) return std::nullopt;
    if (!context.probe || !context.probe->injected) return std::nullopt;
    if (probe.expect == session::Draft23ProbeExpect::Tolerate) {
        // Ignoring the input means the publisher keeps the session open for the whole probe window. A graceful close
        // (NO_ERROR) cannot be tied to the unknown input, so it settles nothing.
        if (!context.probe->close) return OutcomeState::Pass;
        if (context.probe->close->application && context.probe->close->code == 0) return std::nullopt;
        return OutcomeState::Fail;
    }
    if (probe.expect == session::Draft23ProbeExpect::RequestError) {
        // The request must be rejected, not accepted and not answered by closing the session.
        const auto& reply = context.probe->reply;
        return verdict(reply && reply->error_code && *reply->error_code == probe.expected_code);
    }
    if (!context.probe->close) return OutcomeState::Fail;
    return verdict(context.probe->close->application && context.probe->close->code == probe.expected_code);
}

// The REQUEST_ERROR a reply probe drew, if this run is a reply probe and one arrived.
const session::Draft23ProbeReply* request_error_reply(const Draft23Evidence& context) {
    if (!context.probe_scenario || !context.probe || !context.probe->injected || !context.probe->reply) return nullptr;
    const auto* probe = session::find_draft23_probe(*context.probe_scenario);
    if (!probe || probe->expect != session::Draft23ProbeExpect::RequestError) return nullptr;
    return context.probe->reply->error_code ? &*context.probe->reply : nullptr;
}

std::optional<OutcomeState> judge_reply_row(std::string_view id, const Draft23Evidence& context) {
    const auto* reply = request_error_reply(context);
    if (!reply) return std::nullopt;
    if (id == kReplyRows[2].requirement) return verdict(context.probe->reply_fin);
    if (id == kReplyRows[0].requirement) {
        std::set<std::uint64_t> seen;
        for (const auto type : reply->detail_types) if (!seen.insert(type).second) return OutcomeState::Fail;
        return OutcomeState::Pass;
    }
    if (*reply->error_code != kRedirectCode) return OutcomeState::NotApplicable;
    return verdict(std::find(reply->detail_types.begin(), reply->detail_types.end(), kRedirectDetail) != reply->detail_types.end());
}

// nullopt keeps the row at its staged NotRun: the evidence the row needs has not arrived.
std::optional<OutcomeState> judge(std::string_view id, const Draft23Evidence& context) {
    if (const auto* probe = probe_for(id)) return judge_probe(*probe, context);
    if (id == kReplyRows[0].requirement || id == kReplyRows[1].requirement || id == kReplyRows[2].requirement) return judge_reply_row(id, context);
    if (id == kFinAfterDone.requirement) return fin_after_done_should(context);
    if (id == kImplementationSent.requirement) return implementation_sent(context);
    if (id == kImplementationText.requirement) return implementation_text(context);
    if (id == kAliasReuse.requirement) return alias_unique(context);
    if (id == kStreamSpread.requirement) return single_stream(context.data_streams);
    if (id == kTrackName.requirement) return track_name_not_empty(context);
    if (id == kNamespaceField.requirement) return namespace_field_not_empty(context);
    if (id == kReservedPublish.requirement || id == kReservedAnyPurpose.requirement) return not_published_under(context, ".");
    if (id == kSessionNamespace.requirement) return not_published_under(context, ".session");
    if (id == kAliasUnique.requirement) return alias_unique(context);
    if (id == kDatagram.requirement) return datagram_negotiated(context);
    if (id == kFirstBidi.requirement) return first_bidi_message(context);
    if (id == kControlOpen.requirement) return control_stream_open(context);
    if (id == kNoOverflow210.requirement || id == kNoOverflow351.requirement) return no_delta_overflow(context);
    if (id == kNoDuplicateParameter.requirement) return no_duplicate_parameter(context);
    if (id == kAnsweredOk.requirement) return answered_ok(context);
    if (id == kZeroCount.requirement) return zero_stream_count(context);
    if (id == kPadding.requirement) return padding_zero(context);
    const auto native_only = id == kAuthority.requirement || id == kPath.requirement || id == kQuery.requirement;
    if (native_only && context.webtransport) return OutcomeState::NotApplicable;
    if (id == kBitFour.requirement) return bit_four(context.data_streams);
    if (id == kOneStream.requirement) return single_stream(context.data_streams);
    if (id == kLength.requirement) return control_length(context);
    if (id == kRequestId.requirement) return request_id_unique(context);
    if (id == kSingleResponse.requirement) return single_response(context);
    if (id == kFinRequired.requirement) return fin_after_required(context);
    if (id == kResponseBeforeFin.requirement) return response_before_fin(context);
    if (id == kDoneBeforeFin.requirement) return done_before_fin(context);
    if (id == kStreamCount.requirement) return stream_count(context);
    if (!context.peer_setup) return std::nullopt;
    const auto& setup = *context.peer_setup;
    if (id == kMultiplicity.requirement) return multiplicity(setup);
    if (id == kMaxSize.requirement) return max_size(setup);
    if (id == kAuthority.requirement) return authority(setup, context);
    if (id == kPath.requirement) return path(setup, context);
    if (id == kQuery.requirement) return query(setup, context);
    return std::nullopt;
}

}  // namespace

std::vector<Outcome> evaluate_draft23(const RequirementCatalog& catalog, const Draft23Evidence& context) {
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& row : catalog.requirements) {
        auto state = staged_state(row);
        // A row is judged only in the scenarios the catalog binds it to; elsewhere it stays at its staged state.
        const bool bound = context.scenario.empty() || row.scenarios.empty() ||
            std::find(row.scenarios.begin(), row.scenarios.end(), context.scenario) != row.scenarios.end();
        if (bound && row.reviewed && row.testability == Testability::Testable) {
            if (const auto judged = judge(row.id, context)) state = *judged;
        }
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

namespace {
std::vector<std::string> evidence_kinds(std::string_view list) {
    std::vector<std::string> kinds;
    while (!list.empty()) {
        const auto comma = list.find(',');
        kinds.emplace_back(list.substr(0, comma));
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    return kinds;
}
}  // namespace

std::vector<ExecutableBinding> draft23_executable_bindings() {
    std::vector<ExecutableBinding> bindings;
    for (const auto& probe : session::draft23_probes()) {
        const std::string kinds = probe.expect == session::Draft23ProbeExpect::RequestError ? "request_observed" : "peer_closed";
        for (const auto row : session::draft23_probe_rows(probe))
            bindings.push_back({23, std::string(row), std::string(probe.scenario_id), std::string(probe.evaluator_id), evidence_kinds(kinds)});
    }
    for (const auto& row : kReplyRows)
        for (const auto& probe : session::draft23_probes())
            if (probe.expect == session::Draft23ProbeExpect::RequestError)
                bindings.push_back({23, std::string(row.requirement), std::string(probe.scenario_id), std::string(row.evaluator),
                                    evidence_kinds(row.evidence)});
    for (const auto& row : kRows)
        for (const auto scenario : kScenarios)
            bindings.push_back({23, std::string(row.requirement), std::string(scenario), std::string(row.evaluator),
                                evidence_kinds(row.evidence)});
    return bindings;
}

}  // namespace moq::interop::requirements
