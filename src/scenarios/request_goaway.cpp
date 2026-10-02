#include "moq/interop/scenarios/request_goaway.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/wire/draft21/request_error.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
constexpr std::size_t kMaximumBytes = 65546;
constexpr std::size_t kMaximumEvents = 4096;

Bytes frame(std::uint64_t type, const wire::ByteWriter& body) {
    wire::ByteWriter output(kMaximumBytes);
    if (!wire::write_vi64(type,output) ||
        !output.append_byte(static_cast<std::byte>(body.size() >> 8u)) ||
        !output.append_byte(static_cast<std::byte>(body.size() & 255u)) ||
        !output.append_bytes(body.bytes())) throw std::invalid_argument("unencodable request GOAWAY frame");
    return {output.bytes().begin(),output.bytes().end()};
}
Bytes namespace_request(std::uint64_t id, char field) {
    wire::ByteWriter body(65535);
    if (!wire::write_vi64(id,body) || !wire::write_vi64(1,body) ||
        !wire::write_vi64(1,body) || !body.append_byte(static_cast<std::byte>(field)) ||
        !wire::write_vi64(0,body)) throw std::invalid_argument("unencodable namespace request");
    return frame(0x50,body);
}
Bytes goaway() {
    wire::ByteWriter body(65535);
    if (!wire::write_vi64(0,body) || !wire::write_vi64(10000,body))
        throw std::invalid_argument("unencodable request GOAWAY");
    // Neither draft carries Request ID on a request-stream GOAWAY.
    return frame(0x10,body);
}
bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
        std::holds_alternative<transport::LocalCloseEvent>(event) ||
        std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
        std::holds_alternative<transport::TransportErrorEvent>(event) ||
        std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
bool setup_ready(unsigned draft, std::span<const std::byte> bytes) {
    wire::Cursor cursor(bytes);
    if (draft == 21) return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
    const auto decoded = d18::decode_message(d18::StreamRole::Control,cursor,{});
    const auto* message = std::get_if<d18::Message>(&decoded);
    return message && std::holds_alternative<d18::SetupMessage>(*message);
}
bool namespace_tail(wire::Cursor& cursor) {
    std::set<Namespace> announced;
    while (cursor.remaining()) {
        // NAMESPACE and NAMESPACE_DONE use the same framing and suffix
        // structure in both drafts. Include our one-byte prefix in bounds.
        const auto decoded = d21::decode_request_frame(cursor,false);
        const auto* message = std::get_if<d21::RequestFrame>(&decoded);
        if (!message || (message->type.type != 8 && message->type.type != 14)) return false;
        wire::Cursor body(message->body);
        const auto decoded_count = wire::read_vi64(body);
        const auto* count = std::get_if<std::uint64_t>(&decoded_count);
        if (!count || *count > 31) return false;
        Namespace fields;
        std::size_t length = 1;
        for (std::uint64_t i = 0; i < *count; ++i) {
            const auto decoded_field = wire::read_length_prefixed_bytes(body,4096 - length);
            const auto* field = std::get_if<std::span<const std::byte>>(&decoded_field);
            if (!field || field->empty()) return false;
            length += field->size();
            fields.emplace_back(field->begin(),field->end());
        }
        if (body.remaining()) return false;
        if (message->type.type == 8) announced.insert(std::move(fields));
        else if (announced.erase(fields) != 1) return false;
    }
    return true;
}
bool typed_response(unsigned draft, std::span<const std::byte> bytes, bool allow_error) {
    if (bytes.empty() || bytes.size() > kMaximumBytes) return false;
    wire::Cursor cursor(bytes);
    if (draft == 21) {
        if (std::holds_alternative<d21::SuccessfulResponse>(
                d21::decode_successful_response(cursor,d21::ResponseContext::SubscribeNamespace)))
            return namespace_tail(cursor);
        if (!allow_error) return false;
        cursor = wire::Cursor(bytes);
        return std::holds_alternative<d21::RequestErrorMessage>(d21::decode_request_error(cursor,true,true)) &&
            cursor.remaining() == 0;
    }
    const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message) return false;
    if (const auto* ok = std::get_if<d18::RequestOkMessage>(message)) {
        if (!ok->track_properties.entries.empty()) return false;
        for (const auto& parameter : ok->parameters)
            if (d18::validate_parameter_scope(parameter.type,d18::ParameterContext::SubscribeNamespaceOk) !=
                d18::ParameterScopeResult::Allowed) return false;
        return namespace_tail(cursor);
    }
    const auto* error = std::get_if<d18::RequestErrorMessage>(message);
    return allow_error && error && draft18_request_error_valid(*error,true) && cursor.remaining() == 0;
}
struct Observation { Bytes bytes; bool invalid{false}; };
Observation stream_response(std::span<const transport::TransportEvent> events,
                            transport::StreamId stream, std::size_t marker, bool active) {
    Observation result;
    bool closed = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        if (terminal(event)) { result.invalid = true; continue; }
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event); data && data->stream_id == stream) {
            if (closed || (i < marker && !data->data.empty()) ||
                data->data.size() > kMaximumBytes - result.bytes.size()) {
                result.invalid = true;
                continue;
            }
            if (i >= marker) result.bytes.insert(result.bytes.end(),data->data.begin(),data->data.end());
            if (data->fin) {
                closed = true;
                if (active || i < marker) result.invalid = true;
            }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event); reset && reset->stream_id == stream) {
            closed = true;
            result.invalid = true;
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event); stop && stop->stream_id == stream) {
            closed = true;
            result.invalid = true;
        }
    }
    return result;
}
bool active_request(unsigned draft, const RawProbeGateInput& input, std::size_t index) {
    if (input.events.size() > kMaximumEvents || index >= input.prior_writes.size()) return false;
    const auto& write = input.prior_writes[index];
    if (!write.stream_id || (*write.stream_id & 3u) != 1 || !write.delivery_event_count ||
        *write.delivery_event_count > input.events.size() || write.write.fin || write.fin_accepted ||
        write.accepted != write.write.bytes.size()) return false;
    const auto observed = stream_response(input.events,*write.stream_id,*write.delivery_event_count,true);
    return !observed.invalid && typed_response(draft,observed.bytes,false);
}
bool first_goaways_accepted(const RawProbeGateInput& input) {
    if (input.prior_writes.size() != 4 || input.events.size() > kMaximumEvents ||
        std::any_of(input.events.begin(),input.events.end(),terminal)) return false;
    std::set<transport::StreamId> streams;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& write = input.prior_writes[i];
        if (!write.stream_id || !write.delivery_event_count ||
            *write.delivery_event_count > input.events.size() ||
            write.accepted != write.write.bytes.size() || write.write.fin || write.fin_accepted) return false;
        if (i < 2) {
            if ((*write.stream_id & 3u) != 1 || !streams.insert(*write.stream_id).second) return false;
        } else if (write.stream_id != input.prior_writes[i - 2].stream_id || write.write.bytes != goaway()) return false;
    }
    return true;
}
bool barrier_response(unsigned draft, const RawProbeTranscript& transcript) {
    if (transcript.events.size() > kMaximumEvents || transcript.writes.size() != 5) return false;
    const auto& barrier = transcript.writes.back();
    if (!barrier.stream_id || !barrier.delivery_event_count ||
        *barrier.delivery_event_count > transcript.events.size()) return false;
    const auto observed = stream_response(transcript.events,*barrier.stream_id,*barrier.delivery_event_count,false);
    return !observed.invalid && typed_response(draft,observed.bytes,true);
}
RawProbeDefinition definition(unsigned draft, const std::string& id, bool duplicate,
                              std::chrono::milliseconds deadline) {
    RawProbeDefinition result{id,{std::byte{0xaf},std::byte{0},std::byte{0},std::byte{0}}, {},true,
        [draft](auto bytes) { return setup_ready(draft,bytes); },deadline};
    result.writes.push_back({RawProbeChannel::NewBidi,namespace_request(1,'a'),false});
    if (!duplicate) result.writes.push_back({RawProbeChannel::NewBidi,namespace_request(3,'b'),false});
    RawProbeWrite first{RawProbeChannel::NewBidi,goaway(),false,0};
    first.evidence_ready = [draft,duplicate](const auto& input) {
        return active_request(draft,input,0) && (duplicate || active_request(draft,input,1));
    };
    result.writes.push_back(std::move(first));
    RawProbeWrite second{RawProbeChannel::NewBidi,goaway(),false,duplicate ? 0u : 1u};
    second.evidence_ready = [draft,duplicate](const auto& input) {
        return active_request(draft,input,duplicate ? 0u : 1u);
    };
    result.writes.push_back(std::move(second));
    if (!duplicate) {
        RawProbeWrite barrier{RawProbeChannel::NewBidi,namespace_request(5,'c'),false};
        barrier.evidence_ready = first_goaways_accepted;
        result.writes.push_back(std::move(barrier));
    }
    result.response_ready = [draft,duplicate](const auto& transcript) {
        return duplicate ? std::any_of(transcript.events.begin(),transcript.events.end(),terminal)
                         : barrier_response(draft,transcript);
    };
    return result;
}
std::vector<RequestGoawayProbe> profiles(unsigned draft, std::chrono::milliseconds deadline) {
    if (deadline.count() <= 0) throw std::invalid_argument("invalid request GOAWAY deadline");
    const std::string requirement = draft == 18 ? "D18-10-4-MUST-003" : "D21-9-2-MUST-328";
    const std::string evaluator = draft == 18 ? "session-closed-protocol-violation"
        : "d21-duplicate-request-goaway-protocol-violation";
    const std::string duplicate = draft == 18 ? "receive-two-goaways-on-same-request-stream"
        : "d21-duplicate-request-goaway";
    std::vector<RequestGoawayProbe> result{{requirement,evaluator,draft,definition(draft,duplicate,true,deadline),true}};
    if (draft == 21) result.push_back({requirement,evaluator,draft,
        definition(draft,"d21-goaway-on-distinct-request-streams",false,deadline),false});
    return result;
}
}  // namespace

std::vector<RequestGoawayProbe> draft18_request_goaway_probes(std::chrono::milliseconds deadline) {
    return profiles(18,deadline);
}
std::vector<RequestGoawayProbe> draft21_request_goaway_probes(std::chrono::milliseconds deadline) {
    return profiles(21,deadline);
}
std::optional<bool> evaluate_request_goaway_probe(
    const RawProbeTranscript& transcript, const RequestGoawayProbe& profile) {
    if ((profile.draft != 18 && profile.draft != 21) || profile.definition.deadline.count() <= 0)
        return std::nullopt;
    const auto known = profiles(profile.draft,profile.definition.deadline);
    const auto found = std::find_if(known.begin(),known.end(),[&](const auto& candidate) {
        return candidate.definition.id == profile.definition.id &&
            candidate.requirement_id == profile.requirement_id &&
            candidate.evaluator_id == profile.evaluator_id && candidate.duplicate == profile.duplicate;
    });
    if (found == known.end()) return std::nullopt;
    if (profile.duplicate) return evaluate_raw_probe_close(transcript,found->definition,3);
    if (!raw_probe_stimulus_valid(transcript,found->definition)) return std::nullopt;
    return barrier_response(profile.draft,transcript) ? std::optional<bool>{true} : std::nullopt;
}
}  // namespace moq::interop::scenarios
