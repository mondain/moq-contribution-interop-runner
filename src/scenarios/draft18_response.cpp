#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/wire/draft18/messages.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kMaximumResponseBytes = 2 * 65546;
constexpr std::size_t kMaximumEvents = 4096;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

bool valid_reason(std::span<const std::byte> reason) {
    if (reason.size() > 1024) return false;
    for (std::size_t index = 0; index < reason.size();) {
        const auto lead = std::to_integer<unsigned>(reason[index]);
        if (lead < 0x80) { ++index; continue; }
        const auto count = lead >= 0xc2 && lead <= 0xdf ? 1u :
                           lead >= 0xe0 && lead <= 0xef ? 2u :
                           lead >= 0xf0 && lead <= 0xf4 ? 3u : 0u;
        if (count == 0 || count > reason.size() - index - 1) return false;
        const auto second = std::to_integer<unsigned>(reason[index + 1]);
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
            (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) return false;
        for (unsigned offset = 1; offset <= count; ++offset) {
            const auto octet = std::to_integer<unsigned>(reason[index + offset]);
            if (octet < 0x80 || octet > 0xbf) return false;
        }
        index += count + 1;
    }
    return true;
}

bool valid_property_type(std::uint64_t type) {
    // Sections2.5.1/12 and Table14: this fresh session supports no
    // mandatory extensions, and the two prior-gap properties are Object-only.
    return !(type >= 0x4000 && type <= 0x7fff) && type != 0x3c && type != 0x3e;
}
bool valid_property_integer(std::uint64_t type, std::uint64_t value) {
    return (type != 0x0e || value <= 255) &&
           (type != 0x22 || value == 1 || value == 2) &&
           (type != 0x30 || value <= 1);
}
bool valid_properties(const d18::TrackProperties& properties) {
    std::vector<std::span<const std::byte>> pending;
    for (const auto& property : properties.entries) {
        if (!valid_property_type(property.type)) return false;
        if (const auto* value = std::get_if<d18::VarIntValue>(&property.value)) {
            if (!valid_property_integer(property.type,value->value)) return false;
        } else if (property.type == 0x0b) {
            pending.push_back(std::get<d18::ByteValue>(property.value).bytes);
        }
    }
    // Section12.7 permits nested Track Immutable Properties. Borrow spans
    // instead of copying each nested payload or recursing on peer input.
    while (!pending.empty()) {
        wire::Cursor cursor(pending.back());
        pending.pop_back();
        std::uint64_t previous_type = 0;
        while (cursor.remaining() != 0) {
            const auto delta = wire::read_vi64(cursor);
            const auto* increment = std::get_if<std::uint64_t>(&delta);
            if (!increment || *increment > std::numeric_limits<std::uint64_t>::max() - previous_type)
                return false;
            const auto type = previous_type + *increment;
            previous_type = type;
            if (!valid_property_type(type)) return false;
            if ((type & 1u) == 0) {
                const auto value = wire::read_vi64(cursor);
                const auto* integer = std::get_if<std::uint64_t>(&value);
                if (!integer || !valid_property_integer(type,*integer)) return false;
            } else {
                const auto length = wire::read_vi64(cursor);
                const auto* count = std::get_if<std::uint64_t>(&length);
                if (!count || *count > cursor.remaining()) return false;
                const auto value = wire::read_bytes(cursor,static_cast<std::size_t>(*count));
                const auto* payload = std::get_if<std::span<const std::byte>>(&value);
                if (!payload) return false;
                if (type == 0x0b) pending.push_back(*payload);
            }
        }
    }
    return true;
}

bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = d18::decode_message(d18::StreamRole::Control,cursor,{});
    const auto* message = std::get_if<d18::Message>(&decoded);
    return message && std::holds_alternative<d18::SetupMessage>(*message);
}
bool initial_response_ready(std::span<const std::byte> input, bool namespace_scoped) {
    wire::Cursor cursor(input);
    const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message) return false;
    if (!namespace_scoped) {
        const auto* ok = std::get_if<d18::SubscribeOkMessage>(message);
        return ok && cursor.remaining() == 0 && valid_properties(ok->track_properties);
    }
    const auto* ok = std::get_if<d18::RequestOkMessage>(message);
    if (!ok || !ok->track_properties.entries.empty()) return false;
    // REQUEST_OK's generic decoder cannot know the original request scope.
    // In draft18 EXPIRES is forbidden here, unlike draft21 namespace OK.
    for (const auto& parameter : ok->parameters)
        if (d18::validate_parameter_scope(parameter.type,d18::ParameterContext::SubscribeNamespaceOk) !=
            d18::ParameterScopeResult::Allowed) return false;
    // Sections10.16-18 permit notifications immediately after namespace OK.
    // Fully consume coalesced notifications before establishing the request.
    std::vector<d18::TrackNamespace> active;
    while (cursor.remaining() != 0) {
        const auto notification = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* next = std::get_if<d18::Message>(&notification);
        if (!next) return false;
        if (const auto* announced = std::get_if<d18::NamespaceMessage>(next)) {
            active.push_back(announced->track_namespace_suffix);
        } else if (const auto* done = std::get_if<d18::NamespaceDoneMessage>(next)) {
            const auto found = std::find_if(active.begin(),active.end(),[&](const auto& name) {
                return name.fields == done->track_namespace_suffix.fields;
            });
            if (found == active.end()) return false;
            active.erase(found);
        } else {
            return false;
        }
    }
    return true;
}

struct Observation {
    bool valid_error{false};
    std::optional<std::uint64_t> done_status;
    bool terminal{false};
    bool session_closed{false};
    bool invalid_chronology{false};
    bool malformed{false};
    bool incomplete{false};
};
void parse_responses(std::span<const std::byte> input, bool namespace_scoped, Observation& result) {
    wire::Cursor cursor(input);
    while (cursor.remaining() != 0) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        if (!message) {
            result.incomplete = std::holds_alternative<wire::NeedMore>(decoded);
            result.malformed = !result.incomplete;
            return;
        }
        if (const auto* error = std::get_if<d18::RequestErrorMessage>(message)) {
            if (!draft18_request_error_valid(*error, namespace_scoped)) {
                result.malformed = true;
                return;
            }
            result.valid_error = true;
        } else if (const auto* done = std::get_if<d18::PublishDoneMessage>(message)) {
            if (!valid_reason(done->reason_phrase.bytes)) {
                result.malformed = true;
                return;
            }
            // Only a DONE after the actual rejection is additional cleanup.
            // StreamCount accounting is a separate obligation from Status8.
            if (result.valid_error) result.done_status = done->status_code;
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
    for (std::size_t index = *transcript.delivery_event_count; index < transcript.events.size(); ++index) {
        const auto& event = transcript.events[index];
        if (std::holds_alternative<transport::PeerCloseEvent>(event)) {
            result.session_closed = true;
        } else if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
                   data && data->stream_id == stream) {
            if (result.terminal || result.session_closed ||
                data->data.size() > kMaximumResponseBytes - received.size()) {
                result.invalid_chronology = true;
                continue;
            }
            received.insert(received.end(),data->data.begin(),data->data.end());
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
    parse_responses(received,namespace_scoped,result);
    return result;
}
}  // namespace

bool draft18_track_properties_valid(const wire::draft18::TrackProperties& properties) {
    return valid_properties(properties);
}
bool draft18_request_error_valid(const wire::draft18::RequestErrorMessage& error,
                                 bool namespace_scoped) {
    return valid_reason(error.reason_phrase.bytes) &&
        (!error.redirect || (error.redirect->connect_uri.empty() &&
            (!namespace_scoped || error.redirect->track_name.bytes.empty())));
}

std::vector<Draft18ResponseProbe> draft18_response_probes(std::chrono::milliseconds deadline) {
    std::vector<Draft18ResponseProbe> result;
    const auto add = [&](const char* requirement, const char* scenario, const char* evaluator,
                         Draft18ResponseExpectation expectation, bool namespace_scoped, Bytes initial) {
        RawProbeWrite update{RawProbeChannel::NewBidi,bytes({2,0,6,3,1,3,2,2,0}),true,0,
            [namespace_scoped](auto input) { return initial_response_ready(input,namespace_scoped); }};
        RawProbeDefinition definition{scenario,bytes({0xaf,0,0,0}),
            {{RawProbeChannel::NewBidi,std::move(initial),false},std::move(update)},true,setup_ready,deadline,
            [namespace_scoped](const auto& transcript) {
                const auto observation = observe(transcript,namespace_scoped);
                return observation.terminal || observation.session_closed;
            },{}};
        result.push_back({requirement,evaluator,expectation,namespace_scoped,std::move(definition)});
    };
    // Section10.9.1: fresh USE_ALIAS0 has no registration. Cleanup follows
    // any valid actual rejection, without inferring an alias error code.
    add("D18-10-9-1-MUST-001","reject-subscription-request-update","publish-done-update-failed",
        Draft18ResponseExpectation::FailedSubscriptionCleanup,false,bytes({3,0,5,1,0,1,'x',0}));
    add("D18-10-9-1-MUST-003","reject-subscribe-namespace-request-update","namespace-request-stream-closed",
        Draft18ResponseExpectation::FailedDiscoveryCleanup,true,bytes({0x50,0,3,1,0,0}));
    return result;
}

std::optional<bool> evaluate_draft18_response_probe(
    const RawProbeTranscript& transcript, const Draft18ResponseProbe& profile) {
    if (!raw_probe_stimulus_valid(transcript,profile.definition)) return std::nullopt;
    const auto result = observe(transcript,profile.namespace_scoped);
    if (result.invalid_chronology || !result.valid_error || !result.terminal) return std::nullopt;
    if (profile.expectation == Draft18ResponseExpectation::FailedSubscriptionCleanup)
        return result.done_status == 8 && !result.malformed && !result.incomplete;
    return true;
}
}  // namespace moq::interop::scenarios
