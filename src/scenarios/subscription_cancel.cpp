#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/scenarios/subscription_cancel.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
constexpr std::size_t kMaximumFrame = 65546;
constexpr std::size_t kMaximumEvents = kRawProbeMaximumEvents;
constexpr std::size_t kMaximumHeader = 37;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
struct Fixture { Namespace track_namespace; Bytes track_name; };
bool valid_fixture(const Fixture& fixture) {
    if (fixture.track_namespace.size() > 32) return false;
    std::size_t total = fixture.track_name.size();
    if (total > 4096) return false;
    for (const auto& field : fixture.track_namespace) {
        if (field.empty() || field.size() > 4096 - total) return false;
        total += field.size();
    }
    if (fixture.track_namespace.empty()) return true;
    return fixture.track_namespace.front() != bytes({'.'}) &&
        (!fixture.track_name.empty() || fixture.track_namespace.front() != bytes({'.','s','e','s','s','i','o','n'}));
}
Bytes encode_subscribe(const Fixture& fixture) {
    if (!valid_fixture(fixture)) throw std::invalid_argument("invalid SUBSCRIBE track fixture");
    wire::ByteWriter body(65535);
    bool success = wire::write_vi64(1,body) && wire::write_vi64(fixture.track_namespace.size(),body);
    for (const auto& field : fixture.track_namespace)
        success = success && wire::write_length_prefixed_bytes(field,body);
    success = success && wire::write_length_prefixed_bytes(fixture.track_name,body) && wire::write_vi64(0,body);
    wire::ByteWriter frame(kMaximumFrame);
    success = success && wire::write_vi64(3,frame) &&
        frame.append_byte(static_cast<std::byte>(body.size() >> 8u)) &&
        frame.append_byte(static_cast<std::byte>(body.size() & 255u)) && frame.append_bytes(body.bytes());
    if (!success) throw std::invalid_argument("unencodable SUBSCRIBE track fixture");
    return {frame.bytes().begin(),frame.bytes().end()};
}
std::optional<Fixture> decode_fixture(unsigned draft, std::span<const std::byte> input) {
    if (input.size() > kMaximumFrame) return std::nullopt;
    Fixture fixture;
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* subscribe = message ? std::get_if<d18::SubscribeMessage>(message) : nullptr;
        if (!subscribe || subscribe->request_id != 1 || cursor.remaining() != 0) return std::nullopt;
        fixture = {subscribe->track_namespace.fields,subscribe->track_name.bytes};
    } else {
        const auto decoded = d21::decode_request_frame(cursor,true);
        const auto* frame = std::get_if<d21::RequestFrame>(&decoded);
        if (!frame || frame->type.type != 3 || cursor.remaining() != 0) return std::nullopt;
        wire::Cursor body(frame->body);
        const auto id = wire::read_vi64(body);
        const auto count = wire::read_vi64(body);
        const auto* request = std::get_if<std::uint64_t>(&id);
        const auto* fields = std::get_if<std::uint64_t>(&count);
        if (!request || *request != 1 || !fields || *fields > 32) return std::nullopt;
        std::size_t total = 0;
        for (std::uint64_t i = 0; i < *fields; ++i) {
            const auto decoded_field = wire::read_length_prefixed_bytes(body,4096 - total);
            const auto* field = std::get_if<std::span<const std::byte>>(&decoded_field);
            if (!field || field->empty()) return std::nullopt;
            total += field->size();
            fixture.track_namespace.emplace_back(field->begin(),field->end());
        }
        const auto decoded_name = wire::read_length_prefixed_bytes(body,4096 - total);
        const auto* name = std::get_if<std::span<const std::byte>>(&decoded_name);
        if (!name) return std::nullopt;
        fixture.track_name.assign(name->begin(),name->end());
    }
    if (!valid_fixture(fixture)) return std::nullopt;
    // Only namespace/name may differ from the fixed request preset.
    const auto expected = encode_subscribe(fixture);
    if (input.size() != expected.size() || !std::equal(input.begin(),input.end(),expected.begin()))
        return std::nullopt;
    return fixture;
}
bool setup_ready(unsigned draft, std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Control,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        return message && std::holds_alternative<d18::SetupMessage>(*message);
    }
    return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
}
std::optional<std::uint64_t> response_alias(unsigned draft, std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* ok = message ? std::get_if<d18::SubscribeOkMessage>(message) : nullptr;
        if (!ok || cursor.remaining() != 0 || !draft18_track_properties_valid(ok->track_properties))
            return std::nullopt;
        return ok->track_alias;
    }
    const auto decoded = d21::decode_successful_response(cursor,d21::ResponseContext::Subscribe);
    const auto* ok = std::get_if<d21::SuccessfulResponse>(&decoded);
    if (!ok || cursor.remaining() != 0) return std::nullopt;
    return ok->track_alias;
}
bool session_terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
        std::holds_alternative<transport::LocalCloseEvent>(event) ||
        std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
        std::holds_alternative<transport::TransportErrorEvent>(event) ||
        std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
struct HeaderEvidence { Bytes prefix; std::size_t first; bool closed{false}; };
using StreamSet = std::set<transport::StreamId>;
enum class HeaderAssociation { Pending, Unrelated, Associated, Invalid };
struct ParsedHeader { HeaderAssociation association; std::size_t length{0}; };
ParsedHeader parse_subgroup(std::span<const std::byte> input, std::uint64_t alias) {
    wire::Cursor cursor(input);
    const auto decoded_type = wire::read_vi64(cursor);
    const auto* flags = std::get_if<std::uint64_t>(&decoded_type);
    if (!flags) return {HeaderAssociation::Pending};
    if (*flags >= 128 || (*flags & 0x10u) == 0) return {HeaderAssociation::Unrelated};
    if ((*flags & 0x06u) == 0x06u) return {HeaderAssociation::Invalid};
    const auto decoded_alias = wire::read_vi64(cursor);
    const auto* track_alias = std::get_if<std::uint64_t>(&decoded_alias);
    if (!track_alias) return {HeaderAssociation::Pending};
    if (*track_alias != alias) return {HeaderAssociation::Unrelated};
    if (!std::holds_alternative<std::uint64_t>(wire::read_vi64(cursor)))
        return {HeaderAssociation::Pending};
    if ((*flags & 0x06u) == 0x04u &&
        !std::holds_alternative<std::uint64_t>(wire::read_vi64(cursor)))
        return {HeaderAssociation::Pending};
    if ((*flags & 0x20u) == 0 &&
        !std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor,1)))
        return {HeaderAssociation::Pending};
    return {HeaderAssociation::Associated,cursor.offset()};
}
using PeerRequests = std::map<transport::StreamId,Bytes>;
std::optional<PeerRequests> peer_requests(std::span<const transport::TransportEvent> events) {
    PeerRequests result;
    std::size_t total = 0;
    for (const auto& event : events) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 3u) != 0u) continue;
        if (data->data.size() > kMaximumFrame - total) return std::nullopt;
        total += data->data.size();
        if (!result.contains(data->stream_id)) {
            if (result.size() >= 64) return std::nullopt;
            result.emplace(data->stream_id,Bytes{});
        }
        auto& request = result.at(data->stream_id);
        request.insert(request.end(),data->data.begin(),data->data.end());
    }
    return result;
}
bool publish_aliases_distinct(unsigned draft, const std::map<transport::StreamId,Bytes>& requests,
    std::uint64_t alias) {
    for (const auto& [id,request] : requests) {
        (void)id;
        wire::Cursor type_cursor(request);
        const auto decoded_type = wire::read_vi64(type_cursor);
        const auto* type = std::get_if<std::uint64_t>(&decoded_type);
        if (!type) return false;
        if (*type != 0x1d) continue;
        wire::Cursor cursor(request);
        if (draft == 18) {
            const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
            const auto* message = std::get_if<d18::Message>(&decoded);
            const auto* publish = message ? std::get_if<d18::PublishMessage>(message) : nullptr;
            if (!publish || publish->track_alias == alias) return false;
        } else {
            const auto decoded = decode_publish_for_wire(cursor);
            const auto* publish = std::get_if<d21::PublishMessage>(&decoded);
            if (!publish || publish->track_alias == alias) return false;
        }
    }
    return true;
}
struct OpenSubscription { StreamSet streams; std::uint64_t alias; };
std::optional<OpenSubscription> open_streams(unsigned draft, const RawProbeGateInput& input) {
    if (input.prior_writes.size() != 1 || input.events.size() > kMaximumEvents) return std::nullopt;
    const auto& opener = input.prior_writes.front();
    if (!opener.stream_id || (*opener.stream_id & 3u) != 1u || !opener.delivery_event_count ||
        *opener.delivery_event_count > input.events.size() || !opener.write.fin || !opener.fin_accepted ||
        opener.accepted != opener.write.bytes.size() || opener.operation_accepted ||
        opener.write.operation != RawProbeOperation::Write || opener.write.channel != RawProbeChannel::NewBidi ||
        !decode_fixture(draft,opener.write.bytes)) return std::nullopt;
    Bytes response;
    std::map<transport::StreamId,HeaderEvidence> streams;
    const auto requests = peer_requests(input.events);
    if (!requests) return std::nullopt;
    for (std::size_t i = 0; i < input.events.size(); ++i) {
        const auto& event = input.events[i];
        if (session_terminal(event)) return std::nullopt;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (data->stream_id == *opener.stream_id) {
                if (i < *opener.delivery_event_count || data->fin ||
                    data->data.size() > kMaximumFrame - response.size()) return std::nullopt;
                response.insert(response.end(),data->data.begin(),data->data.end());
            } else if ((data->stream_id & 3u) == 2u) {
                if (!streams.contains(data->stream_id)) {
                    if (streams.size() + requests->size() >= 64) return std::nullopt;
                    streams.emplace(data->stream_id,HeaderEvidence{{},i,false});
                }
                auto& header = streams.at(data->stream_id);
                if (header.closed && !data->data.empty()) return std::nullopt;
                const auto count = std::min(data->data.size(),kMaximumHeader - header.prefix.size());
                header.prefix.insert(header.prefix.end(),data->data.begin(),
                    data->data.begin() + static_cast<std::ptrdiff_t>(count));
                header.closed |= data->fin;
            }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if (reset->stream_id == *opener.stream_id) return std::nullopt;
            if ((reset->stream_id & 3u) == 2u) {
                if (!streams.contains(reset->stream_id)) {
                    if (streams.size() + requests->size() >= 64) return std::nullopt;
                    streams.emplace(reset->stream_id,HeaderEvidence{{},i,true});
                } else streams.at(reset->stream_id).closed = true;
            }
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) {
            if (stop->stream_id == *opener.stream_id) return std::nullopt;
        }
    }
    const auto alias = response_alias(draft,response);
    // A peer PUBLISH may legally share this alias. Such subgroup streams
    // cannot be attributed uniquely to our SUBSCRIBE cancellation.
    if (!alias || !publish_aliases_distinct(draft,*requests,*alias)) return std::nullopt;
    StreamSet result{*opener.stream_id};
    for (const auto& [id,header] : streams) {
        if (header.closed) continue;
        const auto parsed = parse_subgroup(header.prefix,*alias);
        if (parsed.association == HeaderAssociation::Unrelated) continue;
        if (parsed.association != HeaderAssociation::Associated ||
            header.first < *opener.delivery_event_count) return std::nullopt;
        result.insert(id);
    }
    if (result.size() < 3) return std::nullopt;
    return OpenSubscription{std::move(result),*alias};
}
struct HeaderCompletion { std::size_t event; std::size_t length; std::size_t bytes; };
struct ObservedHeader {
    Bytes prefix;
    std::size_t bytes{0};
    bool data_seen{false};
    bool closed_before_stop{false};
    std::optional<HeaderCompletion> completion;
};
using ObservedHeaders = std::map<transport::StreamId,ObservedHeader>;
std::optional<ObservedHeaders> observed_headers(const RawProbeTranscript& transcript,
    std::uint64_t alias, std::size_t peer_request_count) {
    ObservedHeaders result;
    for (std::size_t i = 0; i < transcript.events.size(); ++i) {
        const auto& event = transcript.events[i];
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
        if ((!data || (data->stream_id & 3u) != 2u) &&
            (!reset || (reset->stream_id & 3u) != 2u)) continue;
        const auto id = data ? data->stream_id : reset->stream_id;
        if (!result.contains(id)) {
            if (result.size() + peer_request_count >= 64) return std::nullopt;
            result.emplace(id,ObservedHeader{});
        }
        auto& header = result.at(id);
        if (i < *transcript.delivery_event_count && (reset || (data && data->fin)))
            header.closed_before_stop = true;
        if (!data) continue;
        header.data_seen = true;
        const auto count = std::min(data->data.size(),kMaximumHeader - header.prefix.size());
        header.prefix.insert(header.prefix.end(),data->data.begin(),
            data->data.begin() + static_cast<std::ptrdiff_t>(count));
        // One extra counted byte distinguishes a late header from post-reset
        // object payload without retaining the objects themselves.
        header.bytes = std::min(kMaximumHeader + 1,
            header.bytes + std::min(data->data.size(),kMaximumHeader + 1));
        const auto parsed = parse_subgroup(header.prefix,alias);
        if (!header.completion && parsed.association == HeaderAssociation::Associated)
            header.completion = HeaderCompletion{i,parsed.length,header.bytes};
    }
    return result;
}
struct Cleanup { std::optional<bool> result; bool invalid{false}; bool ambiguous_terminal{false}; };
Cleanup observe(const RawProbeTranscript& transcript, unsigned draft) {
    Cleanup result;
    if (transcript.writes.size() != 2 || !transcript.delivery_event_count ||
        *transcript.delivery_event_count > transcript.events.size() || transcript.events.size() > kMaximumEvents)
        return result;
    const auto marker = *transcript.delivery_event_count;
    const auto opened = open_streams(draft,{std::span(transcript.writes).first(1),
        std::span(transcript.events).first(marker)});
    if (!opened) return result;
    // Later control bytes can expose ambiguity from reordered PUBLISH data.
    // They disqualify attribution without establishing the initial gate.
    const auto requests = peer_requests(transcript.events);
    if (!requests || !publish_aliases_distinct(draft,*requests,opened->alias)) {
        result.invalid = true;
        return result;
    }
    const auto headers = observed_headers(transcript,opened->alias,requests->size());
    if (!headers) { result.invalid = true; return result; }
    auto targets = opened->streams;
    for (const auto& [id,header] : *headers) {
        if (header.closed_before_stop || !header.data_seen) continue;
        const auto parsed = parse_subgroup(header.prefix,opened->alias);
        if (parsed.association == HeaderAssociation::Unrelated) continue;
        if (parsed.association != HeaderAssociation::Associated) { result.invalid = true; return result; }
        targets.insert(id);
    }
    StreamSet reset_streams;
    StreamSet terminal_streams;
    bool session_closed = false;
    bool fin_seen = false;
    for (std::size_t i = marker; i < transcript.events.size(); ++i) {
        const auto& event = transcript.events[i];
        if (session_terminal(event)) { session_closed = true; continue; }
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && targets.contains(data->stream_id)) {
            if (session_closed) { result.invalid = true; continue; }
            if (terminal_streams.contains(data->stream_id)) {
                const auto header = headers->find(data->stream_id);
                // RESET can precede reordered bytes of a newly observed header.
                // Keep the tombstone closed and accept only that header's bytes.
                const bool reordered_header = !opened->streams.contains(data->stream_id) &&
                    reset_streams.contains(data->stream_id) && !data->fin && header != headers->end() &&
                    header->second.completion && i <= header->second.completion->event &&
                    header->second.completion->bytes == header->second.completion->length;
                if (!reordered_header) result.invalid = true;
                continue;
            }
            if (data->fin) { terminal_streams.insert(data->stream_id); fin_seen = true; }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
                   reset && targets.contains(reset->stream_id)) {
            if (session_closed || !terminal_streams.insert(reset->stream_id).second) { result.invalid = true; continue; }
            reset_streams.insert(reset->stream_id);
        }
    }
    // Local STOP acceptance does not prove peer receipt. A clean FIN may
    // already have been committed, so it ends observation without a score.
    if (fin_seen) result.ambiguous_terminal = true;
    else if (reset_streams == targets) result.result = true;
    return result;
}
SubscriptionCancelProbe profile(unsigned draft, std::chrono::milliseconds deadline, const Fixture& fixture) {
    if (deadline.count() <= 0) throw std::invalid_argument("invalid subscription cancellation deadline");
    RawProbeWrite stop{RawProbeChannel::NewBidi,{},false,0,{}};
    stop.operation = RawProbeOperation::StopSending;
    stop.application_error = 1;
    stop.evidence_ready = [draft](const auto& input) { return open_streams(draft,input).has_value(); };
    const char* scenario = draft == 18 ? "cancel-subscribe-with-multiple-open-subgroups" :
        "d21-cancel-subscribe-with-open-streams";
    RawProbeDefinition definition{scenario,bytes({0xaf,0,0,0}),
        {{RawProbeChannel::NewBidi,encode_subscribe(fixture),true},std::move(stop)},true,
        [draft](auto input) { return setup_ready(draft,input); },deadline,
        [draft](const auto& transcript) {
            const auto cleanup = observe(transcript,draft);
            return !cleanup.invalid && (cleanup.result.has_value() || cleanup.ambiguous_terminal);
        },{}};
    return {draft == 18 ? "D18-5-1-1-MUST-001" : "D21-3-1-1-MUST-045",
        draft == 18 ? "all-open-subscription-streams-reset" : "d21-subscribe-stop-sending-resets-open-streams",
        draft,std::move(definition)};
}
}  // namespace
std::vector<SubscriptionCancelProbe> draft18_subscription_cancel_probes(
    std::chrono::milliseconds deadline, Namespace track_namespace, Bytes track_name) {
    return {profile(18,deadline,{std::move(track_namespace),std::move(track_name)})};
}
std::vector<SubscriptionCancelProbe> draft21_subscription_cancel_probes(
    std::chrono::milliseconds deadline, Namespace track_namespace, Bytes track_name) {
    return {profile(21,deadline,{std::move(track_namespace),std::move(track_name)})};
}
std::optional<bool> evaluate_subscription_cancel_probe(const RawProbeTranscript& transcript,
    const SubscriptionCancelProbe& supplied) {
    if ((supplied.draft != 18 && supplied.draft != 21) || supplied.definition.deadline.count() <= 0 ||
        transcript.writes.size() != 2) return std::nullopt;
    const auto fixture = decode_fixture(supplied.draft,transcript.writes.front().write.bytes);
    if (!fixture) return std::nullopt;
    const auto expected = profile(supplied.draft,supplied.definition.deadline,*fixture);
    if (supplied.requirement_id != expected.requirement_id || supplied.evaluator_id != expected.evaluator_id ||
        supplied.definition.id != expected.definition.id ||
        !raw_probe_stimulus_valid(transcript,expected.definition)) return std::nullopt;
    const auto cleanup = observe(transcript,supplied.draft);
    return cleanup.invalid ? std::nullopt : cleanup.result;
}
}  // namespace moq::interop::scenarios
