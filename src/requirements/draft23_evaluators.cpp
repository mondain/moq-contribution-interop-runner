#include "moq/interop/requirements/draft23_evaluators.h"

#include "moq/interop/wire/cursor.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
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
constexpr Row kRows[] = {kMultiplicity, kMaxSize, kAuthority, kPath, kQuery, kBitFour, kOneStream, kLength, kRequestId,
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

// nullopt keeps the row at its staged NotRun: the evidence the row needs has not arrived.
std::optional<OutcomeState> judge(std::string_view id, const Draft23Evidence& context) {
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
        if (row.reviewed && row.testability == Testability::Testable) {
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
    for (const auto& row : kRows)
        for (const auto scenario : kScenarios)
            bindings.push_back({23, std::string(row.requirement), std::string(scenario), std::string(row.evaluator),
                                evidence_kinds(row.evidence)});
    return bindings;
}

}  // namespace moq::interop::requirements
