#include "moq/interop/scenarios/draft21_response.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft21/location_filter.h"
#include "moq/interop/wire/draft21/publish_done.h"
#include "moq/interop/wire/draft21/request_error.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <set>
#include <utility>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
constexpr std::size_t kMaximumResponseBytes = 2 * 65546;
constexpr std::size_t kMaximumEvents = kRawProbeMaximumEvents;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<wire::draft21::SetupMessage>(wire::draft21::decode_setup(cursor));
}
bool publish_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = decode_publish_for_wire(cursor);
    const auto* publish = std::get_if<wire::draft21::PublishMessage>(&decoded);
    if (!publish || publish->request_id % 2 != 0 || cursor.remaining() != 0 ||
        wire::draft21::validate_track_properties(publish->track_properties)) return false;
    for (const auto& parameter : publish->parameters) {
        if (parameter.type == 0x21 &&
            !std::holds_alternative<wire::draft21::LocationFilter>(
                wire::draft21::decode_location_filter(std::get<Bytes>(parameter.value)))) return false;
        // The fresh server SETUP offers no token cache. It cannot resolve
        // an incoming cached alias or admit a REGISTER's nonzero cache cost.
        if (parameter.type == 3 &&
            std::get<wire::draft21::Token>(parameter.value).alias_type !=
                wire::draft21::TokenAliasType::UseValue) return false;
    }
    return true;
}

bool initial_response_ready(std::span<const std::byte> input,
                            wire::draft21::ResponseContext context) {
    wire::Cursor cursor(input);
    if (!std::holds_alternative<wire::draft21::SuccessfulResponse>(
            wire::draft21::decode_successful_response(cursor, context))) return false;
    if (context != wire::draft21::ResponseContext::SubscribeNamespace)
        return cursor.remaining() == 0;
    // Sections9.15/9.16/9.17: namespace notifications may follow the OK
    // in the same read. The profile's empty prefix makes each suffix a full
    // namespace. Track active suffixes by fields, not their wire encodings.
    std::set<std::vector<Bytes>> active;
    while (cursor.remaining() != 0) {
        const auto decoded = wire::draft21::decode_request_frame(cursor, false);
        const auto* frame = std::get_if<wire::draft21::RequestFrame>(&decoded);
        if (!frame || (frame->type.kind != wire::draft21::MessageKind::Namespace &&
                       frame->type.kind != wire::draft21::MessageKind::NamespaceDone)) return false;
        wire::Cursor body(frame->body, cursor.offset() - frame->body.size());
        const auto count = wire::read_vi64(body);
        const auto* fields = std::get_if<std::uint64_t>(&count);
        if (!fields || *fields > 32) return false;
        std::vector<Bytes> name_space;
        std::size_t total = 0;
        for (std::uint64_t field = 0; field < *fields; ++field) {
            const auto value = wire::read_length_prefixed_bytes(body, 4096 - total);
            const auto* bytes = std::get_if<std::span<const std::byte>>(&value);
            if (!bytes || bytes->empty()) return false;
            total += bytes->size();
            name_space.emplace_back(bytes->begin(), bytes->end());
        }
        if (body.remaining() != 0) return false;
        if (frame->type.kind == wire::draft21::MessageKind::Namespace)
            active.insert(std::move(name_space));
        else if (active.erase(name_space) != 1)
            return false;
    }
    return true;
}

struct Observation {
    bool valid_ok{false};
    bool valid_error{false};
    std::optional<std::uint64_t> done_status;
    bool terminal{false};
    bool session_closed{false};
    bool protocol_close{false};
    bool invalid_chronology{false};
    bool malformed{false};
    bool incomplete{false};
};
template<class T>
bool decoded(const wire::DecodeResult<T>& value, Observation& result) {
    if (std::holds_alternative<wire::NeedMore>(value)) result.incomplete = true;
    if (std::holds_alternative<wire::DecodeError>(value)) result.malformed = true;
    return std::holds_alternative<T>(value);
}

void parse_responses(std::span<const std::byte> bytes, bool namespace_scoped,
                     Observation& result) {
    wire::Cursor cursor(bytes);
    while (cursor.remaining() != 0) {
        wire::Cursor lookahead = cursor;
        const auto type = wire::read_vi64(lookahead);
        if (!decoded(type, result)) return;
        if (std::get<std::uint64_t>(type) == 5) {
            const auto error = wire::draft21::decode_request_error(cursor, true, namespace_scoped);
            if (!decoded(error, result)) return;
            if (!wire::draft21::valid_reason_phrase(
                    std::get<wire::draft21::RequestErrorMessage>(error).reason)) {
                result.malformed = true;
                return;
            }
            result.valid_error = true;
        } else if (std::get<std::uint64_t>(type) == 7) {
            const auto ok = wire::draft21::decode_successful_response(
                cursor, wire::draft21::ResponseContext::RequestUpdate);
            if (!decoded(ok, result)) return;
            result.valid_ok = true;
        } else if (std::get<std::uint64_t>(type) == 0x0b) {
            const auto done = wire::draft21::decode_publish_done(cursor);
            if (!decoded(done, result)) return;
            // Initial or unrelated DONE cannot establish a failed update.
            if (result.valid_error)
                result.done_status = std::get<wire::draft21::PublishDoneMessage>(done).status_code;
        } else {
            // Ancillary framed notifications do not fabricate an update
            // response. Response exclusivity belongs to a separate row.
            const auto frame = wire::draft21::decode_request_frame(cursor, false);
            if (!decoded(frame, result)) return;
        }
    }
}

Observation observe(const RawProbeTranscript& transcript, bool namespace_scoped) {
    Observation result;
    if (!transcript.stimulus_delivered || !transcript.delivery_event_count ||
        *transcript.delivery_event_count > transcript.events.size() ||
        transcript.events.size() > kMaximumEvents || transcript.writes.empty() ||
        !transcript.writes.back().stream_id) return result;
    const auto stream = *transcript.writes.back().stream_id;
    Bytes received;
    for (std::size_t index = *transcript.delivery_event_count;
         index < transcript.events.size(); ++index) {
        const auto& event = transcript.events[index];
        if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
            result.session_closed = true;
            result.protocol_close = result.protocol_close ||
                (close->error_space == transport::CloseErrorSpace::Application && close->error_code == 3);
        } else if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
                   data && data->stream_id == stream) {
            if (result.terminal || result.session_closed ||
                data->data.size() > kMaximumResponseBytes - received.size()) {
                result.invalid_chronology = true;
                continue;
            }
            received.insert(received.end(), data->data.begin(), data->data.end());
            result.terminal = data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
                   reset && reset->stream_id == stream) {
            if (result.session_closed) {
                result.invalid_chronology = true;
                continue;
            }
            result.terminal = true;
        }
    }
    parse_responses(received, namespace_scoped, result);
    return result;
}

bool response_ready(const RawProbeTranscript& transcript,
                    Draft21ResponseExpectation expectation, bool namespace_scoped) {
    const auto result = observe(transcript, namespace_scoped);
    if (result.terminal || result.session_closed) return true;
    return expectation == Draft21ResponseExpectation::PermittedPublishUpdate &&
        (result.valid_ok || result.valid_error) && !result.malformed &&
        !result.incomplete && !result.invalid_chronology;
}
}  // namespace

std::vector<Draft21ResponseProbe> draft21_response_probes(std::chrono::milliseconds deadline) {
    std::vector<Draft21ResponseProbe> result;
    const auto add = [&](const char* requirement, const char* scenario, const char* evaluator,
                         Draft21ResponseExpectation expectation, bool namespace_scoped,
                         Bytes initial, wire::draft21::ResponseContext context) {
        const bool permitted = expectation == Draft21ResponseExpectation::PermittedPublishUpdate;
        const auto channel = permitted ? RawProbeChannel::PeerBidi : RawProbeChannel::NewBidi;
        RawProbeWrite update{channel,
            permitted ? bytes({2,0,2,1,0}) : bytes({2,0,6,3,1,3,2,2,0}),
            !permitted, 0, {}};
        if (!permitted) update.peer_response_ready = [context](auto input) {
            return initial_response_ready(input, context);
        };
        RawProbeDefinition definition{scenario, bytes({0xaf,0,0,0}),
            {{channel,std::move(initial),false},std::move(update)}, true, setup_ready, deadline,
            [expectation,namespace_scoped](const auto& transcript) {
                return response_ready(transcript,expectation,namespace_scoped);
            }, permitted ? publish_ready : std::function<bool(std::span<const std::byte>)>{}};
        result.push_back({requirement,evaluator,expectation,namespace_scoped,std::move(definition)});
    };
    // Sections9.5/9.20.17: a parameterless subscriber update is legal after
    // accepting PUBLISH, even though this server was not the original sender.
    add("D21-9-5-MUST-344", "d21-subscriber-update-on-publish",
        "d21-request-update-context-and-direction", Draft21ResponseExpectation::PermittedPublishUpdate,
        false, bytes({7,0,1,0}), wire::draft21::ResponseContext::RequestUpdate);
    // Section8.9: USE_ALIAS0 is structurally valid and unregistered in this
    // fresh sender. Conditional cleanup follows any actual valid rejection;
    // these profiles make no assumption about UNKNOWN_AUTH_TOKEN_ALIAS's code.
    add("D21-9-5-1-MUST-346", "d21-failed-subscription-update-cleanup",
        "d21-failed-update-publish-done-update-failed", Draft21ResponseExpectation::FailedSubscriptionCleanup,
        false, bytes({3,0,5,1,0,1,'x',0}), wire::draft21::ResponseContext::Subscribe);
    add("D21-9-5-1-MUST-348", "d21-failed-subscribe-namespace-update-close",
        "d21-failed-namespace-update-stream-close", Draft21ResponseExpectation::FailedDiscoveryCleanup,
        true, bytes({0x50,0,3,1,0,0}), wire::draft21::ResponseContext::SubscribeNamespace);
    add("D21-9-5-1-MUST-349", "d21-failed-subscribe-tracks-update-close",
        "d21-failed-subscribe-tracks-update-stream-close", Draft21ResponseExpectation::FailedDiscoveryCleanup,
        true, bytes({0x51,0,3,1,0,0}), wire::draft21::ResponseContext::SubscribeTracks);
    return result;
}

std::optional<bool> evaluate_draft21_response_probe(
    const RawProbeTranscript& transcript, const Draft21ResponseProbe& profile) {
    if (!raw_probe_stimulus_valid(transcript, profile.definition)) return std::nullopt;
    const auto result = observe(transcript, profile.namespace_scoped);
    if (profile.expectation == Draft21ResponseExpectation::PermittedPublishUpdate) {
        if (result.protocol_close) return false;
        if (result.invalid_chronology || result.malformed || result.incomplete) return std::nullopt;
        if (result.valid_ok || result.valid_error) return true;
        return std::nullopt;
    }
    if (result.invalid_chronology || !result.valid_error || !result.terminal) return std::nullopt;
    if (profile.expectation == Draft21ResponseExpectation::FailedSubscriptionCleanup)
        return result.done_status == 8 && !result.malformed && !result.incomplete;
    return true;
}
}  // namespace moq::interop::scenarios
