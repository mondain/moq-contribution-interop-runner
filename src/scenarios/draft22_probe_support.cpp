#include "draft22_probe_support.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>

namespace moq::interop::scenarios::d22support {

namespace d21 = wire::draft21;

Bytes setup_message() { return Bytes{std::byte{0xaf}, std::byte{0}, std::byte{0}, std::byte{0}}; }

void integer(Bytes& output, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64 capacity");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}

Bytes frame(std::uint64_t type, const Bytes& body) {
    if (body.size() > 65535) throw std::invalid_argument("draft 22 probe request too large");
    Bytes result;
    integer(result, type);
    result.push_back(static_cast<std::byte>(body.size() >> 8u));
    result.push_back(static_cast<std::byte>(body.size() & 255u));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

void track_namespace(Bytes& body, const Namespace& ns) {
    integer(body, ns.size());
    for (const auto& field : ns) {
        integer(body, field.size());
        body.insert(body.end(), field.begin(), field.end());
    }
}

void track(Bytes& body, const Fixture& fixture) {
    track_namespace(body, fixture.ns);
    integer(body, fixture.name.size());
    body.insert(body.end(), fixture.name.begin(), fixture.name.end());
}

std::optional<std::uint64_t> number(wire::Cursor& cursor) {
    const auto decoded = wire::read_vi64(cursor);
    if (const auto* value = std::get_if<std::uint64_t>(&decoded)) return *value;
    return {};
}

std::optional<Namespace> read_namespace(wire::Cursor& cursor) {
    const auto count = number(cursor);
    if (!count || *count > 32) return {};
    Namespace result;
    std::size_t total = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto field = wire::read_length_prefixed_bytes(cursor, 4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&field);
        if (!value || value->empty()) return {};
        total += value->size();
        result.emplace_back(value->begin(), value->end());
    }
    return result;
}

bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
}

Messages parse_messages(std::span<const std::byte> input) {
    Messages result;
    wire::Cursor cursor(input);
    while (cursor.remaining() != 0) {
        auto working = cursor;
        const auto start = working.offset();
        const auto type = wire::read_vi64(working);
        if (std::holds_alternative<wire::NeedMore>(type)) break;
        if (std::holds_alternative<wire::DecodeError>(type)) { result.malformed = true; break; }
        const auto length = wire::read_bytes(working, 2);
        if (std::holds_alternative<wire::NeedMore>(length)) break;
        const auto* prefix = std::get_if<std::span<const std::byte>>(&length);
        if (!prefix) { result.malformed = true; break; }
        const auto size = (static_cast<std::size_t>(std::to_integer<unsigned>((*prefix)[0])) << 8u) |
                          std::to_integer<unsigned>((*prefix)[1]);
        const auto body = wire::read_bytes(working, size);
        if (std::holds_alternative<wire::NeedMore>(body)) break;
        const auto* value = std::get_if<std::span<const std::byte>>(&body);
        if (!value) { result.malformed = true; break; }
        const auto begin = input.begin() + static_cast<std::ptrdiff_t>(start);
        const auto end = input.begin() + static_cast<std::ptrdiff_t>(working.offset());
        result.complete.push_back({std::get<std::uint64_t>(type), Bytes(begin, end), Bytes(value->begin(), value->end())});
        cursor = working;
    }
    return result;
}

bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
           std::holds_alternative<transport::LocalCloseEvent>(event) ||
           std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
           std::holds_alternative<transport::TransportErrorEvent>(event) ||
           std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}

Collected collect(std::span<const transport::TransportEvent> events, CollectLimits limits) {
    Collected result;
    if (events.size() > kRawProbeMaximumEvents) { result.bounded = false; return result; }
    std::size_t total = 0;
    const auto kept = [&](transport::StreamId id) { return !limits.request_streams_only || (id & 2u) == 0u; };
    // A new stream entry, unless the stream bound is reached (then the evidence is not bounded).
    const auto entry = [&](transport::StreamId id, std::size_t index) -> StreamData* {
        if (!result.streams.contains(id) && result.streams.size() >= kMaximumStreams) return nullptr;
        auto [found, inserted] = result.streams.try_emplace(id);
        if (inserted) found->second.first_event = index;
        return &found->second;
    };
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (!kept(data->stream_id)) continue;
            if (data->data.size() > limits.maximum_bytes - total) { result.bounded = false; return result; }
            auto* stream = entry(data->stream_id, index);
            if (!stream) { result.bounded = false; return result; }
            total += data->data.size();
            if (!stream->first_data_event) stream->first_data_event = index;
            if (stream->fin || stream->reset) { stream->overrun = true; continue; }
            stream->bytes.insert(stream->bytes.end(), data->data.begin(), data->data.end());
            stream->fin = data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if (!kept(reset->stream_id)) continue;
            auto* stream = entry(reset->stream_id, index);
            if (!stream) { result.bounded = false; return result; }
            if (!stream->reset) stream->reset_event = index;
            stream->reset = true;
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) {
            if (!kept(stop->stream_id)) continue;
            auto* stream = entry(stop->stream_id, index);
            if (!stream) { result.bounded = false; return result; }
            if (!stream->stop_sending) stream->stop_event = index;
            stream->stop_sending = true;
        }
    }
    return result;
}

std::optional<transport::StreamId> control_stream(const Streams& streams) {
    std::optional<transport::StreamId> best;
    for (const auto& [id, stream] : streams) {
        if ((id & 3u) != 2u || !setup_ready(stream.bytes)) continue;
        if (!best || stream.first_event < streams.at(*best).first_event) best = id;
    }
    return best;
}

Response response_of(const RawProbeTranscript& t, const Collected& collected, std::size_t index) {
    Response response;
    if (index >= t.writes.size()) return response;
    const auto& write = t.writes[index];
    if (!write.stream_id || !write.delivery_event_count || *write.delivery_event_count > t.events.size())
        return response;
    const auto found = collected.streams.find(*write.stream_id);
    if (found == collected.streams.end() || found->second.first_event < *write.delivery_event_count ||
        found->second.overrun) return response;
    response.stream = &found->second;
    response.messages = parse_messages(found->second.bytes);
    return response;
}

void require_draft22_wire(const char* what) {
    if (current_wire_draft() != 22)
        throw std::logic_error(std::string(what) + " probes are built on the draft 22 wire only");
}

std::optional<Fixture> recover_fixture(std::span<const std::byte> input, std::uint64_t type,
                                       std::uint64_t expected_request) {
    if (input.size() > kMaximumBytes) return {};
    wire::Cursor cursor(input);
    const auto decoded = d21::decode_request_frame(cursor, true);
    const auto* request = std::get_if<d21::RequestFrame>(&decoded);
    if (!request || request->type.type != type || cursor.remaining() != 0) return {};
    wire::Cursor body(request->body);
    if (number(body) != expected_request) return {};
    auto ns = read_namespace(body);
    if (!ns) return {};
    std::size_t total = 0;
    for (const auto& field : *ns) total += field.size();
    const auto name = wire::read_length_prefixed_bytes(body, 4096 - total);
    const auto* value = std::get_if<std::span<const std::byte>>(&name);
    if (!value) return {};
    Fixture fixture{std::move(*ns), Bytes(value->begin(), value->end())};
    if (!fetch_first_object_fixture_valid(fixture.ns, fixture.name)) return {};
    return fixture;
}

std::optional<Proven> prove(const RawProbeTranscript& t, const RawProbeDefinition& expected, bool window) {
    if (t.scenario_id != expected.id) return {};
    if (!t.complete && !(window && t.timed_out)) return {};
    Proven proven{t, false};
    const auto end = std::find_if(t.events.begin(), t.events.end(), terminal);
    proven.prefix.events.assign(t.events.begin(), end);
    proven.prefix.complete = true;
    proven.prefix.timed_out = false;
    if (!raw_probe_stimulus_valid(proven.prefix, expected)) return {};
    proven.ended = window && (t.timed_out || end != t.events.end());
    return proven;
}

std::optional<RawProbeTranscript> prove_delivered(const RawProbeTranscript& t, RawProbeDefinition expected) {
    if (t.scenario_id != expected.id || !t.timed_out || t.stimulus_delivered || t.harness_failed ||
        t.writes.size() != expected.writes.size())
        return {};
    std::size_t delivered = 0;
    while (delivered < t.writes.size() && t.writes[delivered].delivery_event_count) ++delivered;
    if (delivered == 0) return {};
    expected.writes.erase(expected.writes.begin() + static_cast<std::ptrdiff_t>(delivered), expected.writes.end());
    RawProbeTranscript prefix = t;
    prefix.writes.erase(prefix.writes.begin() + static_cast<std::ptrdiff_t>(delivered), prefix.writes.end());
    const auto end = std::find_if(t.events.begin(), t.events.end(), terminal);
    prefix.events.assign(t.events.begin(), end);
    prefix.delivery_event_count = prefix.writes.back().delivery_event_count;
    prefix.stimulus_delivered = true;
    prefix.complete = true;
    prefix.timed_out = false;
    if (!raw_probe_stimulus_valid(prefix, expected)) return {};
    return prefix;
}

}  // namespace moq::interop::scenarios::d22support
