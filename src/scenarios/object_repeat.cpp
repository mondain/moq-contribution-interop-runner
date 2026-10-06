#include "moq/interop/scenarios/object_repeat.h"
#include "inline_filter_sites_testing.h"
#include "moq/interop/scenarios/location_filter_param.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/wire/draft18/objects.h"
#include "moq/interop/wire/draft21/objects.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
constexpr std::size_t kMaximumBytes = 65546;
constexpr std::size_t kMaximumEvents = kRawProbeMaximumEvents;
struct Fixture { Namespace ns; Bytes name; };

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
void integer(Bytes& output, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64 capacity");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}
Bytes frame(std::uint64_t type, const Bytes& body) {
    if (body.size() > 65535) throw std::invalid_argument("Object repeat request too large");
    Bytes result;
    integer(result, type);
    result.push_back(static_cast<std::byte>(body.size() >> 8u));
    result.push_back(static_cast<std::byte>(body.size() & 255u));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
Bytes subscribe(const Fixture& fixture) {
    if (!fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid Object repeat track fixture");
    Bytes body = bytes({1});
    integer(body, fixture.ns.size());
    for (const auto& field : fixture.ns) {
        integer(body, field.size());
        body.insert(body.end(), field.begin(), field.end());
    }
    integer(body, fixture.name.size());
    body.insert(body.end(), fixture.name.begin(), fixture.name.end());
    // FORWARD=1 and an absolute LOCATION_FILTER starting at Group7/Object9.
    // Two parameters: FORWARD (0x10) = 1, then LOCATION_FILTER (delta 0x11 from 0x10).
    Bytes parameters = bytes({2, 0x10, 1, 0x11});
    const auto filter = filter_param_value({7, 9});
    parameters.insert(parameters.end(), filter.begin(), filter.end());
    body.insert(body.end(), parameters.begin(), parameters.end());
    return frame(3, body);
}
Bytes fetch(unsigned draft, const Fixture& fixture, unsigned request_id) {
    const auto profiles = draft == 18
        ? draft18_fetch_first_object_probes(std::chrono::milliseconds(1000), fixture.ns, fixture.name)
        : draft21_fetch_first_object_probes(std::chrono::milliseconds(1000), fixture.ns, fixture.name);
    auto result = profiles.front().definition.writes.front().bytes;
    // Both canonical FETCH frames have a one-byte type and request ID.
    result.at(3) = static_cast<std::byte>(request_id);
    return result;
}
std::optional<std::uint64_t> number(wire::Cursor& cursor) {
    const auto decoded = wire::read_vi64(cursor);
    if (const auto* value = std::get_if<std::uint64_t>(&decoded)) return *value;
    return {};
}
std::optional<Fixture> recover_fixture(unsigned draft, ObjectRepeatAspect aspect,
                                      std::span<const std::byte> input) {
    if (input.size() > kMaximumBytes) return {};
    Fixture fixture;
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* request = message ? std::get_if<d18::FetchMessage>(message) : nullptr;
        const auto* standalone = request ? std::get_if<d18::StandaloneFetch>(&request->fetch) : nullptr;
        if (!standalone || request->request_id != 1 || cursor.remaining() != 0) return {};
        fixture = {standalone->track_namespace.fields, standalone->track_name.bytes};
    } else {
        const auto decoded = d21::decode_request_frame(cursor, true);
        const auto* request = std::get_if<d21::RequestFrame>(&decoded);
        if (!request || request->type.type != (aspect == ObjectRepeatAspect::Payload ? 3u : 0x16u) ||
            cursor.remaining() != 0) return {};
        wire::Cursor body(request->body);
        const auto id = number(body), count = number(body);
        if (id != 1 || !count || *count > 32) return {};
        std::size_t total = 0;
        for (std::uint64_t index = 0; index < *count; ++index) {
            const auto field = wire::read_length_prefixed_bytes(body, 4096 - total);
            const auto* value = std::get_if<std::span<const std::byte>>(&field);
            if (!value || value->empty()) return {};
            total += value->size();
            fixture.ns.emplace_back(value->begin(), value->end());
        }
        const auto name = wire::read_length_prefixed_bytes(body, 4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&name);
        if (!value) return {};
        fixture.name.assign(value->begin(), value->end());
    }
    if (!fetch_first_object_fixture_valid(fixture.ns, fixture.name)) return {};
    const auto expected = aspect == ObjectRepeatAspect::Payload ? subscribe(fixture) : fetch(draft, fixture, 1);
    if (input.size() != expected.size() || !std::equal(input.begin(), input.end(), expected.begin())) return {};
    return fixture;
}
bool setup_ready(unsigned draft, std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Control, cursor, {});
        const auto* message = std::get_if<d18::Message>(&decoded);
        return message && std::holds_alternative<d18::SetupMessage>(*message);
    }
    return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
}
bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
        std::holds_alternative<transport::LocalCloseEvent>(event) ||
        std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
        std::holds_alternative<transport::TransportErrorEvent>(event) ||
        std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
bool bounded(std::span<const transport::TransportEvent> events, std::size_t total = 0) {
    if (events.size() > kMaximumEvents || total > kMaximumBytes) return false;
    const auto fits = [&](std::size_t size) {
        if (size > kMaximumBytes - total) return false;
        total += size;
        return true;
    };
    for (const auto& event : events) {
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (!fits(data->data.size())) return false;
        } else if (const auto* datagram = std::get_if<transport::DatagramEvent>(&event)) {
            if (!fits(datagram->data.size())) return false;
        } else if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&event)) {
            if (!fits(established->alpn.size()) || !fits(established->local_connection_id.size()) ||
                !fits(established->peer_connection_id.size())) return false;
        } else if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
            if (!fits(close->reason.size())) return false;
        } else if (const auto* close = std::get_if<transport::LocalCloseEvent>(&event)) {
            if (!fits(close->reason.size())) return false;
        }
    }
    return true;
}
bool bounded(const RawProbeTranscript& transcript) {
    if (transcript.writes.size() > 2 || transcript.setup.write.bytes.size() > kMaximumBytes) return false;
    auto total = transcript.setup.write.bytes.size();
    for (const auto& write : transcript.writes) {
        if (write.write.bytes.size() > kMaximumBytes - total) return false;
        total += write.write.bytes.size();
    }
    return bounded(transcript.events, total);
}

struct Stream {
    Bytes bytes;
    std::size_t first;
    bool fin{false};
    bool cancelled{false};
};
using Streams = std::map<transport::StreamId, Stream>;
std::optional<Streams> collect(std::span<const transport::TransportEvent> events) {
    if (!bounded(events)) return {};
    Streams streams;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (streams.size() >= 64 && !streams.contains(data->stream_id)) return {};
            auto [found, inserted] = streams.try_emplace(data->stream_id, Stream{{}, index});
            (void)inserted;
            auto& stream = found->second;
            if (stream.fin || stream.cancelled) { stream.cancelled = true; continue; }
            stream.bytes.insert(stream.bytes.end(), data->data.begin(), data->data.end());
            stream.fin = data->fin;
        } else {
            std::optional<transport::StreamId> id;
            if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) id = reset->stream_id;
            if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) id = stop->stream_id;
            if (id) {
                if (streams.size() >= 64 && !streams.contains(*id)) return {};
                streams.try_emplace(*id, Stream{{}, index}).first->second.cancelled = true;
            }
        }
    }
    return streams;
}
std::optional<d21::SuccessfulResponse> ok21(const Stream& stream, d21::ResponseContext context) {
    if (stream.cancelled) return {};
    wire::Cursor cursor(stream.bytes);
    const auto decoded = d21::decode_successful_response(cursor, context);
    const auto* response = std::get_if<d21::SuccessfulResponse>(&decoded);
    if (!response || cursor.remaining() != 0) return {};
    return *response;
}
bool fetch_ok(unsigned draft, const Stream& stream) {
    if (stream.cancelled) return false;
    if (draft == 21) {
        const auto ok = ok21(stream, d21::ResponseContext::Fetch);
        return ok && ok->end_location && ok->end_location->group == 7 && ok->end_location->object == 9;
    }
    wire::Cursor cursor(stream.bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    const auto* ok = message ? std::get_if<d18::FetchOkMessage>(message) : nullptr;
    return ok && cursor.remaining() == 0 && ok->end_of_track <= 1 &&
        ok->end_location.group == 7 && ok->end_location.object == 10 &&
        draft18_track_properties_valid(ok->track_properties);
}
struct Payload { Bytes bytes; };
std::optional<Payload> subscription(std::span<const RawProbeAcceptedWrite> writes,
                                    std::span<const transport::TransportEvent> events) {
    if (writes.empty() || !writes[0].stream_id || !writes[0].delivery_event_count) return {};
    const auto marker = *writes[0].delivery_event_count;
    if (marker > events.size()) return {};
    const auto streams = collect(events);
    if (!streams) return {};
    const auto response = streams->find(*writes[0].stream_id);
    if (response == streams->end() || response->second.first < marker) return {};
    const auto ok = ok21(response->second, d21::ResponseContext::Subscribe);
    if (!ok || !ok->track_alias) return {};
    std::optional<Payload> result;
    bool associated = false;
    for (const auto& [id, stream] : *streams) {
        if ((id & 3u) != 2u) continue;
        wire::Cursor cursor(stream.bytes);
        const auto type = number(cursor), alias = number(cursor);
        if (!type || *type >= 128 || (*type & 0x10u) == 0 || alias != ok->track_alias) continue;
        if (associated || stream.first < marker || stream.cancelled || !stream.fin) return {};
        associated = true;
        d21::SubgroupDecoder decoder({.maximum_retained_payload_length = kMaximumBytes});
        const auto decoded = decoder.push(stream.bytes, true);
        if (decoded.error || !decoded.observations.empty() || !decoded.clean_fin ||
            decoded.objects.size() != 1) return {};
        const auto& object = decoded.objects.front();
        if (object.track_alias != ok->track_alias || object.group_id != 7 || object.object_id != 9 ||
            object.status.value_or(0) != 0 || object.payload_length != object.retained_payload.size()) return {};
        result = Payload{object.retained_payload};
    }
    return result;
}
struct FirstHeader { std::size_t immutable_count; };
// Duplicate outer Immutable Properties prevent the full wire decoder from
// emitting an Object. Parse its actual first header to obtain this count while
// preserving every other syntax, scope and nesting prerequisite.
std::optional<FirstHeader> first_header(std::span<const std::byte> input,
                                        std::uint64_t request_id) {
    wire::Cursor cursor(input);
    const auto type = number(cursor), request = number(cursor), flags = number(cursor);
    if (type != 5 || request != request_id || !flags || *flags >= 128 ||
        (*flags & 0x1cu) != 0x1cu ||
        ((*flags & 0x40u) == 0 && ((*flags & 3u) == 1 || (*flags & 3u) == 2))) return {};
    const auto group = number(cursor);
    if (!group || *group != 7) return {};
    if ((*flags & 0x40u) == 0 && (*flags & 3u) == 3 && !number(cursor)) return {};
    const auto object = number(cursor);
    if (!object || *object != 9 || !std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor, 1))) return {};
    d21::KeyValues properties;
    if ((*flags & 0x20u) != 0) {
        const auto length = number(cursor);
        if (!length || *length > 65535) return {};
        const auto raw = wire::read_bytes(cursor, static_cast<std::size_t>(*length));
        const auto* block = std::get_if<std::span<const std::byte>>(&raw);
        if (!block) return {};
        wire::Cursor property_cursor(*block);
        auto decoded = d21::decode_key_values_to_end(property_cursor);
        const auto* parsed = std::get_if<d21::KeyValues>(&decoded);
        if (!parsed) return {};
        properties = *parsed;
    }
    std::size_t immutable_count = 0;
    bool group_gap_seen = false, object_gap_seen = false;
    const auto valid_list = [&](const d21::KeyValues& list, bool nested) {
        for (const auto& entry : list) {
            const auto type_id = entry.type;
            if (type_id == 4 || type_id == 0x0e || type_id == 0x22 || type_id == 0x30 ||
                (type_id >= 0x4000 && type_id <= 0x7fff) || (nested && type_id == 0xb)) return false;
            if (type_id == 0x3c || type_id == 0x3e) {
                auto& seen = type_id == 0x3c ? group_gap_seen : object_gap_seen;
                const auto* value = std::get_if<std::uint64_t>(&entry.value);
                if (!value || seen || *value > (type_id == 0x3c ? 7u : 9u)) return false;
                seen = true;
            }
        }
        return true;
    };
    if (!valid_list(properties, false)) return {};
    for (const auto& property : properties) {
        if (property.type != 0xb) continue;
        ++immutable_count;
        const auto* raw = std::get_if<Bytes>(&property.value);
        if (!raw) return {};
        wire::Cursor nested(*raw);
        const auto decoded = d21::decode_key_values_to_end(nested);
        const auto* entries = std::get_if<d21::KeyValues>(&decoded);
        if (!entries || !valid_list(*entries, true)) return {};
    }
    return FirstHeader{immutable_count};
}
template <class Decoder, class Object, class Order, class Limits>
std::optional<Payload> full_fetch(const Stream& stream, std::uint64_t request_id, Limits limits) {
    if (!stream.fin || stream.cancelled) return {};
    limits.maximum_retained_payload_length = kMaximumBytes;
    Decoder decoder([request_id](std::uint64_t id) -> std::optional<Order> {
        return id == request_id ? std::optional{Order::Ascending} : std::nullopt;
    }, limits);
    const auto decoded = decoder.push(stream.bytes, true);
    const bool unavailable = std::any_of(decoded.observations.begin(), decoded.observations.end(), [](const auto& observation) {
        return observation.kind != decltype(observation.kind)::ShouldViolation;
    });
    if (decoded.error || unavailable || !decoded.clean_fin || decoded.events.size() != 1 ||
        !decoded.header || decoded.header->request_id != request_id) return {};
    const auto* object = std::get_if<Object>(&decoded.events.front());
    if (!object || object->request_id != request_id || object->group_id != 7 || object->object_id != 9 ||
        object->status.value_or(0) != 0 || object->payload_length != object->retained_payload.size()) return {};
    return Payload{object->retained_payload};
}
std::optional<bool> observe(const RawProbeTranscript& t, unsigned draft, ObjectRepeatAspect aspect) {
    const auto expected_writes = aspect == ObjectRepeatAspect::Payload ? 2u : 1u;
    if (t.writes.size() != expected_writes || !bounded(t)) return {};
    const auto& write = t.writes.back();
    if (!write.stream_id || !write.delivery_event_count || *write.delivery_event_count > t.events.size()) return {};
    const auto streams = collect(t.events);
    if (!streams) return {};
    const auto response = streams->find(*write.stream_id);
    if (response == streams->end() || response->second.first < *write.delivery_event_count ||
        !fetch_ok(draft, response->second)) return {};
    const auto request_id = aspect == ObjectRepeatAspect::Payload ? 3u : 1u;
    const Stream* associated = nullptr;
    for (const auto& [id, stream] : *streams) {
        if ((id & 3u) != 2u) continue;
        wire::Cursor cursor(stream.bytes);
        const auto type = number(cursor), request = number(cursor);
        if (type != 5 || request != request_id) continue;
        if (associated || stream.first < *write.delivery_event_count || stream.cancelled) return {};
        associated = &stream;
    }
    if (!associated) return {};
    const auto header = first_header(associated->bytes, request_id);
    if (!header) return {};
    if (aspect == ObjectRepeatAspect::ImmutableCount && header->immutable_count > 1) return false;
    const auto fetched = draft == 18
        ? full_fetch<d18::FetchDecoder, d18::ObjectEvent, d18::FetchGroupOrder>(*associated, request_id, d18::Limits{})
        : full_fetch<d21::FetchDecoder, d21::ObjectEvent, d21::FetchGroupOrder>(*associated, request_id, d21::Limits{});
    if (!fetched) return {};
    if (aspect == ObjectRepeatAspect::ImmutableCount) return true;
    const auto first = subscription(t.writes, t.events);
    if (!first) return {};
    return first->bytes == fetched->bytes;
}
std::vector<ObjectRepeatProbe> profiles(unsigned draft, std::chrono::milliseconds deadline, const Fixture& fixture) {
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid Object repeat probe fixture or deadline");
    const auto definition = [&](std::string id, ObjectRepeatAspect aspect) {
        RawProbeDefinition result;
        result.id = std::move(id);
        result.setup_bytes = bytes({0xaf, 0, 0, 0});
        result.deadline = deadline;
        result.peer_setup_ready = [draft](auto input) { return setup_ready(draft, input); };
        if (aspect == ObjectRepeatAspect::Payload) {
            result.writes.push_back({RawProbeChannel::NewBidi, subscribe(fixture), false});
            RawProbeWrite second{RawProbeChannel::NewBidi, fetch(draft, fixture, 3), true};
            second.evidence_ready = [](const auto& input) {
                return subscription(input.prior_writes, input.events).has_value();
            };
            result.writes.push_back(std::move(second));
        } else {
            result.writes.push_back({RawProbeChannel::NewBidi, fetch(draft, fixture, 1), true});
        }
        result.response_ready = [draft, aspect](const auto& t) { return observe(t, draft, aspect).has_value(); };
        return result;
    };
    if (draft == 18) return {{"D18-12-7-MUST-NOT-005", "object-has-at-most-one-immutable-properties-wrapper", 18,
        ObjectRepeatAspect::ImmutableCount, definition("publish-object-with-immutable-properties", ObjectRepeatAspect::ImmutableCount)}};
    return {
        {"D21-2-1-MUST-NOT-016", "d21-object-payload-stable-for-full-identity", 21,
            ObjectRepeatAspect::Payload, definition("d21-repeat-object-retrieval", ObjectRepeatAspect::Payload)},
        {"D21-10-7-MUST-NOT-492", "d21-object-immutable-property-count", 21,
            ObjectRepeatAspect::ImmutableCount, definition("d21-object-immutable-property-singleton", ObjectRepeatAspect::ImmutableCount)},
    };
}
}  // namespace

SiteBytes object_repeat_subscribe_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name) {
    return subscribe(Fixture{track_namespace, track_name});
}
std::vector<ObjectRepeatProbe> draft18_object_repeat_probes(std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    return profiles(18, deadline, {std::move(ns), std::move(name)});
}
std::vector<ObjectRepeatProbe> draft21_object_repeat_probes(std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    return profiles(21, deadline, {std::move(ns), std::move(name)});
}
std::optional<bool> evaluate_object_repeat_probe(const RawProbeTranscript& t, const ObjectRepeatProbe& p) {
    if ((p.draft != 18 && p.draft != 21) || t.writes.empty() || p.definition.deadline.count() <= 0 ||
        !bounded(t) || (p.draft == 18 && p.aspect == ObjectRepeatAspect::Payload)) return {};
    const auto fixture = recover_fixture(p.draft, p.aspect, t.writes.front().write.bytes);
    if (!fixture) return {};
    const auto candidates = profiles(p.draft, p.definition.deadline, *fixture);
    const auto expected = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return candidate.requirement_id == p.requirement_id && candidate.evaluator_id == p.evaluator_id &&
            candidate.aspect == p.aspect && candidate.definition.id == p.definition.id;
    });
    if (expected == candidates.end() || t.scenario_id != expected->definition.id) return {};
    RawProbeTranscript prefix;
    prefix.scenario_id = t.scenario_id;
    prefix.setup = t.setup;
    prefix.writes = t.writes;
    prefix.transport_established = t.transport_established;
    prefix.max_datagram_payload = t.max_datagram_payload;
    prefix.peer_setup_received = t.peer_setup_received;
    prefix.stimulus_delivered = t.stimulus_delivered;
    prefix.complete = t.complete;
    prefix.harness_failed = t.harness_failed;
    prefix.timed_out = t.timed_out;
    prefix.delivery_event_count = t.delivery_event_count;
    const auto end = std::find_if(t.events.begin(), t.events.end(), terminal);
    prefix.events.assign(t.events.begin(), end);
    if (!raw_probe_stimulus_valid(prefix, expected->definition)) return {};
    return observe(prefix, p.draft, p.aspect);
}
}  // namespace moq::interop::scenarios
