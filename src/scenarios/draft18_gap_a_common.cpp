#include "draft18_gap_a_common.h"

#include <algorithm>
#include <stdexcept>

namespace moq::interop::scenarios::gap_a {
namespace {
constexpr std::size_t kMaximumStreams = 64;
constexpr std::size_t kMaximumCollectedBytes = std::size_t{8} << 20;

Bytes literal(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
}  // namespace

bool fixture_valid(const Fixture& fixture) {
    if (fixture.track_namespace.empty() || fixture.track_namespace.size() > 31 ||
        fixture.track_name.empty() || fixture.track_name.size() > 4096) return false;
    std::size_t total = fixture.track_name.size();
    for (const auto& field : fixture.track_namespace) {
        if (field.empty() || field.size() > 4096 - total) return false;
        total += field.size();
    }
    // The period-prefixed namespaces are reserved by Section 3.2.1.
    const auto& first = fixture.track_namespace.front();
    return first.empty() || first.front() != std::byte{'.'};
}

Fixture canonical_fixture(Namespace track_namespace, Bytes track_name) {
    if (track_namespace.empty()) track_namespace.push_back(literal({'a'}));
    return {std::move(track_namespace), std::move(track_name)};
}

Bytes encode(const d18::Message& message) {
    wire::ByteWriter output(kMaximumBytes);
    if (!d18::encode_message(message, output).has_value())
        throw std::invalid_argument("unencodable draft-18 gap probe message");
    return {output.bytes().begin(), output.bytes().end()};
}

Bytes setup_frame(const d18::KeyValuePairs& options) {
    return encode(d18::SetupMessage{options});
}

bool setup_ready(std::span<const std::byte> bytes) {
    wire::Cursor cursor(bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Control, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    return message && std::holds_alternative<d18::SetupMessage>(*message);
}

bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
        std::holds_alternative<transport::LocalCloseEvent>(event) ||
        std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
        std::holds_alternative<transport::TransportErrorEvent>(event) ||
        std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}

SplitFrames split_frames(std::span<const std::byte> bytes) {
    SplitFrames result;
    std::size_t position = 0;
    while (position < bytes.size()) {
        wire::Cursor cursor(bytes.subspan(position));
        const auto type = wire::read_vi64(cursor);
        if (std::holds_alternative<wire::NeedMore>(type)) break;
        const auto* value = std::get_if<std::uint64_t>(&type);
        if (!value) { result.malformed = true; return result; }
        const auto header = cursor.offset();
        if (bytes.size() - position < header + 2) break;
        const auto length = (std::to_integer<std::size_t>(bytes[position + header]) << 8u) |
            std::to_integer<std::size_t>(bytes[position + header + 1]);
        if (bytes.size() - position < header + 2 + length) break;
        result.frames.push_back({*value, position, header + 2 + length});
        position += header + 2 + length;
    }
    result.trailing_bytes = bytes.size() - position;
    return result;
}

std::optional<d18::Message> decode_frame(std::span<const std::byte> bytes, const Frame& frame) {
    if (frame.offset > bytes.size() || frame.size > bytes.size() - frame.offset) return std::nullopt;
    wire::Cursor cursor(bytes.subspan(frame.offset, frame.size));
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message || cursor.remaining() != 0) return std::nullopt;
    return *message;
}

std::optional<Streams> collect_streams(std::span<const transport::TransportEvent> events,
                                       std::size_t end) {
    if (end > events.size() || end > kMaximumEvents) return std::nullopt;
    Streams streams;
    std::size_t total = 0;
    const auto find = [&](transport::StreamId id, std::size_t index) -> StreamData* {
        auto found = streams.find(id);
        if (found == streams.end()) {
            if (streams.size() >= kMaximumStreams) return nullptr;
            found = streams.emplace(id, StreamData{}).first;
            found->second.first_event = index;
        }
        return &found->second;
    };
    for (std::size_t i = 0; i < end; ++i) {
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&events[i])) {
            if (data->data.size() > kMaximumCollectedBytes - total) return std::nullopt;
            total += data->data.size();
            auto* stream = find(data->stream_id, i);
            if (!stream) return std::nullopt;
            // Bytes after a FIN or reset cannot belong to a well-formed stream.
            if (stream->fin || stream->reset) {
                if (!data->data.empty() || !data->fin) return std::nullopt;
                continue;
            }
            stream->bytes.insert(stream->bytes.end(), data->data.begin(), data->data.end());
            if (data->fin) { stream->fin = true; stream->fin_event = i; }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&events[i])) {
            auto* stream = find(reset->stream_id, i);
            if (!stream) return std::nullopt;
            if (!stream->reset) { stream->reset = true; stream->reset_event = i; }
        }
    }
    return streams;
}

std::optional<Streams> collect_streams(std::span<const transport::TransportEvent> events) {
    return collect_streams(events, events.size());
}

std::optional<PeerControl> peer_control(const Streams& streams) {
    for (const auto& [id, stream] : streams) {
        if (!is_peer_uni(id) || stream.bytes.empty()) continue;
        wire::Cursor cursor(stream.bytes);
        const auto decoded = d18::decode_message(d18::StreamRole::Control, cursor, {});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* setup = message ? std::get_if<d18::SetupMessage>(message) : nullptr;
        if (setup) return PeerControl{id, *setup};
    }
    return std::nullopt;
}

StreamMessages stream_messages(const StreamData& stream) {
    StreamMessages result;
    const auto split = split_frames(stream.bytes);
    result.malformed = split.malformed;
    for (const auto& frame : split.frames) {
        result.types.push_back(frame.type);
        const auto message = decode_frame(stream.bytes, frame);
        if (!message) { result.undecodable = true; continue; }
        result.messages.push_back(*message);
    }
    return result;
}

bool is_peer_bidi(transport::StreamId id) { return (id & 3u) == 0u; }
bool is_peer_uni(transport::StreamId id) { return (id & 3u) == 2u; }
bool is_local_bidi(transport::StreamId id) { return (id & 3u) == 1u; }

std::optional<d18::Location> largest_object(const d18::Parameters& parameters) {
    for (const auto& parameter : parameters) {
        if (parameter.type != 0x09) continue;
        if (const auto* location = std::get_if<d18::Location>(&parameter.value)) return *location;
    }
    return std::nullopt;
}

bool location_less(const d18::Location& left, const d18::Location& right) {
    return left.group < right.group || (left.group == right.group && left.object < right.object);
}

d18::TrackNamespace track_namespace(const Namespace& fields) { return {fields}; }
Namespace namespace_fields(const d18::TrackNamespace& value) { return value.fields; }

std::vector<SubgroupStream> subgroup_streams(const Streams& streams, std::uint64_t track_alias) {
    std::vector<SubgroupStream> result;
    for (const auto& [id, stream] : streams) {
        if (!is_peer_uni(id) || stream.bytes.empty()) continue;
        wire::Cursor type_cursor(stream.bytes);
        const auto type = wire::read_vi64(type_cursor);
        const auto* raw = std::get_if<std::uint64_t>(&type);
        if (!raw || *raw >= 128 || (*raw & 0x10u) == 0) continue;
        d18::SubgroupDecoder decoder;
        const auto pushed = decoder.push(stream.bytes, stream.fin);
        if (!pushed.header) {
            // An incomplete header cannot be attributed to an alias yet.
            continue;
        }
        if (pushed.header->track_alias != track_alias) continue;
        result.push_back({id, *pushed.header, pushed.objects, stream.fin, stream.reset,
                          pushed.error.has_value()});
    }
    return result;
}

const StreamData* local_stream(const Streams& streams, const RawProbeAcceptedWrite& write) {
    if (!write.stream_id) return nullptr;
    const auto found = streams.find(*write.stream_id);
    return found == streams.end() ? nullptr : &found->second;
}

std::optional<std::size_t> write_marker(const RawProbeAcceptedWrite& write) {
    return write.delivery_event_count;
}

Bytes subscribe_request(std::uint64_t request_id, const Fixture& fixture, d18::Parameters parameters) {
    return encode(d18::SubscribeMessage{request_id, track_namespace(fixture.track_namespace),
        {fixture.track_name}, std::move(parameters)});
}

Bytes track_status_request(std::uint64_t request_id, const Fixture& fixture, d18::Parameters parameters) {
    return encode(d18::TrackStatusMessage{request_id, track_namespace(fixture.track_namespace),
        {fixture.track_name}, std::move(parameters)});
}

Bytes subscribe_namespace_request(std::uint64_t request_id, const Namespace& prefix,
                                  d18::Parameters parameters) {
    return encode(d18::SubscribeNamespaceMessage{request_id, track_namespace(prefix),
        std::move(parameters)});
}

Bytes subscribe_tracks_request(std::uint64_t request_id, const Namespace& prefix,
                               d18::Parameters parameters) {
    return encode(d18::SubscribeTracksMessage{request_id, track_namespace(prefix),
        std::move(parameters)});
}

Bytes standalone_fetch_request(std::uint64_t request_id, const Fixture& fixture,
                               d18::Location start, d18::Location end) {
    return encode(d18::FetchMessage{request_id,
        d18::StandaloneFetch{track_namespace(fixture.track_namespace), {fixture.track_name}, start, end},
        {}});
}

Bytes joining_fetch_request(std::uint64_t request_id, std::uint64_t joining_request_id,
                            std::uint64_t joining_start, bool relative) {
    d18::Fetch fetch = relative
        ? d18::Fetch{d18::RelativeJoiningFetch{joining_request_id, joining_start}}
        : d18::Fetch{d18::AbsoluteJoiningFetch{joining_request_id, joining_start}};
    return encode(d18::FetchMessage{request_id, std::move(fetch), {}});
}

Bytes request_update(std::uint64_t request_id, d18::Parameters parameters) {
    return encode(d18::RequestUpdateMessage{request_id, std::move(parameters)});
}

d18::Parameter forward_parameter(std::uint8_t value) {
    return {0x10, d18::Uint8ParameterValue{value}};
}
d18::Parameter priority_parameter(std::uint8_t value) {
    return {0x20, d18::Uint8ParameterValue{value}};
}
d18::Parameter filter_parameter(d18::SubscriptionFilter filter) {
    return {0x21, std::move(filter)};
}
d18::Parameter token_parameter(d18::Token token) {
    return {0x03, std::move(token)};
}

std::optional<Fixture> recover_fixture(FirstWrite kind, std::span<const std::byte> bytes) {
    if (bytes.size() > kMaximumBytes) return std::nullopt;
    wire::Cursor cursor(bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message || cursor.remaining() != 0) return std::nullopt;
    Fixture fixture;
    if (kind == FirstWrite::Subscribe) {
        const auto* value = std::get_if<d18::SubscribeMessage>(message);
        if (!value) return std::nullopt;
        fixture = {value->track_namespace.fields, value->track_name.bytes};
    } else if (kind == FirstWrite::TrackStatus) {
        const auto* value = std::get_if<d18::TrackStatusMessage>(message);
        if (!value) return std::nullopt;
        fixture = {value->track_namespace.fields, value->track_name.bytes};
    } else if (kind == FirstWrite::Fetch) {
        const auto* value = std::get_if<d18::FetchMessage>(message);
        const auto* standalone = value ? std::get_if<d18::StandaloneFetch>(&value->fetch) : nullptr;
        if (!standalone) return std::nullopt;
        fixture = {standalone->track_namespace.fields, standalone->track_name.bytes};
    } else {
        // Discovery probes name only a namespace; the track name is unused.
        if (const auto* value = std::get_if<d18::SubscribeNamespaceMessage>(message))
            fixture = {value->track_namespace_prefix.fields, literal({'x'})};
        else if (const auto* tracks = std::get_if<d18::SubscribeTracksMessage>(message))
            fixture = {tracks->track_namespace_prefix.fields, literal({'x'})};
        else return std::nullopt;
    }
    if (!fixture_valid(fixture)) return std::nullopt;
    return fixture;
}

std::optional<d18::ObjectEvent> fetched_object(const Streams& streams,
                                               const RawProbeAcceptedWrite& fetch_write) {
    if (!fetch_write.stream_id || !fetch_write.delivery_event_count) return std::nullopt;
    const auto* response = local_stream(streams, fetch_write);
    if (!response || response->reset) return std::nullopt;
    const auto messages = stream_messages(*response);
    if (messages.malformed || messages.undecodable || messages.messages.empty()) return std::nullopt;
    if (!std::holds_alternative<d18::FetchOkMessage>(messages.messages.front())) return std::nullopt;
    wire::Cursor request(fetch_write.write.bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, request, {});
    const auto* request_message = std::get_if<d18::Message>(&decoded);
    const auto* fetch = request_message ? std::get_if<d18::FetchMessage>(request_message) : nullptr;
    if (!fetch) return std::nullopt;
    for (const auto& [id, stream] : streams) {
        if (!is_peer_uni(id) || stream.first_event < *fetch_write.delivery_event_count) continue;
        wire::Cursor header(stream.bytes);
        const auto type = wire::read_vi64(header);
        const auto request_id = wire::read_vi64(header);
        const auto* raw = std::get_if<std::uint64_t>(&type);
        const auto* associated = std::get_if<std::uint64_t>(&request_id);
        if (!raw || *raw != 0x5 || !associated || *associated != fetch->request_id) continue;
        const auto expected = fetch->request_id;
        d18::Limits limits;
        limits.maximum_retained_payload_length = kMaximumBytes;
        d18::FetchDecoder decoder([expected](std::uint64_t value) -> std::optional<d18::FetchGroupOrder> {
            return value == expected ? std::optional{d18::FetchGroupOrder::Ascending} : std::nullopt;
        }, limits);
        const auto pushed = decoder.push(stream.bytes, stream.fin);
        if (pushed.error) return std::nullopt;
        for (const auto& event : pushed.events) {
            const auto* object = std::get_if<d18::ObjectEvent>(&event);
            if (object && object->group_id == 7 && object->object_id == 9 &&
                object->status.value_or(0) == 0 &&
                object->payload_length == object->retained_payload.size()) return *object;
        }
    }
    return std::nullopt;
}

bool fetch_object_observed(const RawProbeAcceptedWrite& fetch_write,
                           std::span<const transport::TransportEvent> events) {
    const auto streams = collect_streams(events);
    return streams && fetched_object(*streams, fetch_write).has_value();
}

std::optional<std::uint64_t> application_close_code(
    std::span<const transport::TransportEvent> events) {
    for (const auto& event : events) {
        const auto* close = std::get_if<transport::PeerCloseEvent>(&event);
        if (close && close->error_space == transport::CloseErrorSpace::Application)
            return close->error_code;
    }
    return std::nullopt;
}

const d18::Message* first_response(const StreamMessages& messages) {
    return messages.messages.empty() ? nullptr : &messages.messages.front();
}

std::optional<StreamMessages> messages_of(const Streams& streams, const RawProbeAcceptedWrite& write) {
    const auto* stream = local_stream(streams, write);
    if (!stream) return std::nullopt;
    return stream_messages(*stream);
}

bool is_final_response(const d18::Message& message) {
    return std::holds_alternative<d18::SubscribeOkMessage>(message) ||
        std::holds_alternative<d18::RequestOkMessage>(message) ||
        std::holds_alternative<d18::RequestErrorMessage>(message) ||
        std::holds_alternative<d18::FetchOkMessage>(message);
}

RawProbeWrite make_write(RawProbeChannel channel, Bytes bytes, bool fin) {
    RawProbeWrite write;
    write.channel = channel;
    write.bytes = std::move(bytes);
    write.fin = fin;
    return write;
}

RawProbeDefinition make_definition(std::string id, Bytes setup, std::chrono::milliseconds deadline) {
    RawProbeDefinition definition;
    definition.id = std::move(id);
    definition.setup_bytes = std::move(setup);
    definition.deadline = deadline;
    definition.peer_setup_ready = setup_ready;
    return definition;
}

}  // namespace moq::interop::scenarios::gap_a
