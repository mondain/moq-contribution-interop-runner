#include "moq/interop/scenarios/immutable_repeat.h"

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
#include <type_traits>
#include <utility>

namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
using Value = std::variant<std::uint64_t, Bytes>;
using Pairs = std::vector<std::pair<std::uint64_t, Value>>;
constexpr std::size_t kMaximumBytes = 65546;
constexpr std::size_t kMaximumEvents = 4096;
struct Fixture { Namespace track_namespace; Bytes track_name; };
struct Wrapper { std::optional<Bytes> bytes; Pairs contents; };
struct Retrieval {
    std::optional<Wrapper> track;
    std::optional<Wrapper> object;
    bool ordinary_object{false};
    bool object_fin{false};
    bool cancelled{false};
};

Bytes bytes(std::initializer_list<unsigned> input) {
    Bytes out;
    for (const auto value : input) out.push_back(static_cast<std::byte>(value));
    return out;
}
Bytes encode_fetch(unsigned draft, const Fixture& fixture, std::uint64_t id) {
    if (!fetch_first_object_fixture_valid(fixture.track_namespace, fixture.track_name))
        throw std::invalid_argument("invalid immutable repeat FETCH fixture");
    wire::ByteWriter output(kMaximumBytes);
    if (draft == 18) {
        const d18::FetchMessage request{id,
            d18::StandaloneFetch{{fixture.track_namespace}, {fixture.track_name}, {7, 9}, {7, 10}}, {}};
        if (!d18::encode_message(d18::Message{request}, output).has_value())
            throw std::invalid_argument("unencodable immutable repeat FETCH");
    } else {
        wire::ByteWriter body(65535), filter(27);
        bool ok = wire::write_vi64(id, body) &&
                  wire::write_vi64(fixture.track_namespace.size(), body);
        for (const auto& field : fixture.track_namespace)
            ok = ok && wire::write_length_prefixed_bytes(field, body);
        // Draft21 FETCH_RANGE is inclusive. Parameter types are delta encoded:
        // FETCH_RANGE 0x21 followed by INCLUDE_PROPERTIES 0x35 (delta 0x14).
        ok = ok && wire::write_length_prefixed_bytes(fixture.track_name, body) &&
             wire::write_vi64(2, body) && wire::write_vi64(0x21, body) &&
             wire::write_vi64(7, filter) && wire::write_vi64(9, filter) &&
             wire::write_vi64(0, filter) && wire::write_vi64(9, filter) &&
             wire::write_length_prefixed_bytes(filter.bytes(), body) &&
             wire::write_vi64(0x14, body) && body.append_byte(std::byte{1}) &&
             wire::write_vi64(0x16, output) &&
             output.append_byte(static_cast<std::byte>(body.size() >> 8u)) &&
             output.append_byte(static_cast<std::byte>(body.size() & 255u)) &&
             output.append_bytes(body.bytes());
        if (!ok) throw std::invalid_argument("unencodable immutable repeat FETCH");
    }
    return {output.bytes().begin(), output.bytes().end()};
}
std::optional<Fixture> decode_fixture(unsigned draft, std::span<const std::byte> input) {
    if (input.size() > kMaximumBytes) return {};
    wire::Cursor cursor(input);
    Fixture fixture;
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* fetch = message ? std::get_if<d18::FetchMessage>(message) : nullptr;
        const auto* standalone = fetch ? std::get_if<d18::StandaloneFetch>(&fetch->fetch) : nullptr;
        if (!standalone || cursor.remaining() != 0) return {};
        fixture = {standalone->track_namespace.fields, standalone->track_name.bytes};
    } else {
        const auto decoded = d21::decode_request_frame(cursor, true);
        const auto* frame = std::get_if<d21::RequestFrame>(&decoded);
        if (!frame || frame->type.type != 0x16 || cursor.remaining() != 0) return {};
        wire::Cursor body(frame->body);
        if (!std::holds_alternative<std::uint64_t>(wire::read_vi64(body))) return {};
        const auto count = wire::read_vi64(body);
        const auto* fields = std::get_if<std::uint64_t>(&count);
        if (!fields || *fields > 32) return {};
        std::size_t total = 0;
        for (std::uint64_t i = 0; i < *fields; ++i) {
            const auto field = wire::read_length_prefixed_bytes(body, 4096 - total);
            const auto* value = std::get_if<std::span<const std::byte>>(&field);
            if (!value || value->empty()) return {};
            total += value->size();
            fixture.track_namespace.emplace_back(value->begin(), value->end());
        }
        const auto name = wire::read_length_prefixed_bytes(body, 4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&name);
        if (!value) return {};
        fixture.track_name.assign(value->begin(), value->end());
    }
    if (!fetch_first_object_fixture_valid(fixture.track_namespace, fixture.track_name)) return {};
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
bool bounded(std::span<const transport::TransportEvent> events) {
    if (events.size() > kMaximumEvents) return false;
    std::size_t total = 0;
    const auto fits = [&](std::size_t size) {
        if (size > kMaximumBytes - total) return false;
        total += size;
        return true;
    };
    for (const auto& event : events) {
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (!fits(data->data.size())) return false;
        } else if (const auto* datagram = std::get_if<transport::DatagramEvent>(&event)) {
            if (!fits(datagram->data.size())) return false;
        } else if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&event)) {
            if (!fits(established->alpn.size()) || !fits(established->local_connection_id.size()) ||
                !fits(established->peer_connection_id.size())) return false;
        }
    }
    return true;
}
bool bounded(const RawProbeTranscript& t) {
    if (t.writes.size() != 2 || t.setup.write.bytes.size() > kMaximumBytes || !bounded(t.events))
        return false;
    std::size_t total = t.setup.write.bytes.size();
    for (const auto& write : t.writes) {
        if (write.write.bytes.size() > kMaximumBytes - total) return false;
        total += write.write.bytes.size();
    }
    return true;
}
Pairs normalized(const d18::KeyValuePairs& pairs) {
    Pairs result;
    for (const auto& pair : pairs) {
        if (const auto* value = std::get_if<d18::VarIntValue>(&pair.value))
            result.emplace_back(pair.type, value->value);
        else result.emplace_back(pair.type, std::get<d18::ByteValue>(pair.value).bytes);
    }
    return result;
}
Pairs normalized(const d21::KeyValues& pairs) {
    Pairs result;
    for (const auto& pair : pairs) result.emplace_back(pair.type, pair.value);
    return result;
}
std::optional<Wrapper> wrapper(unsigned draft, const Pairs& pairs) {
    Wrapper result;
    for (const auto& [type, value] : pairs) {
        if (type != 0xb) continue;
        const auto* inner = std::get_if<Bytes>(&value);
        if (!inner || result.bytes) return {};
        result.bytes = *inner;
        wire::Cursor cursor(*inner);
        if (draft == 18) {
            const auto decoded = d18::decode_key_value_pairs(cursor, inner->size(), {});
            const auto* contents = std::get_if<d18::KeyValuePairs>(&decoded);
            if (!contents || cursor.remaining() != 0) return {};
            result.contents = normalized(*contents);
        } else {
            const auto decoded = d21::decode_key_values_to_end(cursor);
            const auto* contents = std::get_if<d21::KeyValues>(&decoded);
            if (!contents || cursor.remaining() != 0) return {};
            result.contents = normalized(*contents);
        }
        // Nested Track wrappers may be legal, but cannot establish this flat
        // comparison. Malformed Object wrappers belong to separate rows.
        if (std::ranges::any_of(result.contents, [](const auto& pair) { return pair.first == 0xb; }))
            return {};
    }
    return result;
}
std::optional<Wrapper> accepted_track(unsigned draft, const Bytes& input) {
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* ok = message ? std::get_if<d18::FetchOkMessage>(message) : nullptr;
        if (!ok || cursor.remaining() != 0 || ok->end_of_track > 1 ||
            ok->end_location != d18::Location{7, 10} ||
            !draft18_track_properties_valid(ok->track_properties)) return {};
        return wrapper(draft, normalized(ok->track_properties.entries));
    }
    const auto decoded = d21::decode_successful_response(cursor, d21::ResponseContext::Fetch);
    const auto* ok = std::get_if<d21::SuccessfulResponse>(&decoded);
    if (!ok || cursor.remaining() != 0 || !ok->end_location ||
        ok->end_location->group != 7 || ok->end_location->object != 9) return {};
    return wrapper(draft, normalized(ok->track_properties));
}
bool object_properties_valid(const Pairs& mutable_properties, const Wrapper& immutable) {
    bool group_gap_seen = false, object_gap_seen = false;
    const auto valid = [&](const Pairs& pairs) {
        for (const auto& [type, value] : pairs) {
            if (type == 4 || type == 0xe || type == 0x22 || type == 0x30 ||
                (type >= 0x4000 && type <= 0x7fff)) return false;
            if (type != 0x3c && type != 0x3e) continue;
            auto& seen = type == 0x3c ? group_gap_seen : object_gap_seen;
            const auto* gap = std::get_if<std::uint64_t>(&value);
            if (!gap || seen || *gap > (type == 0x3c ? 7u : 9u)) return false;
            seen = true;
        }
        return true;
    };
    return valid(mutable_properties) && valid(immutable.contents);
}
struct Candidate {
    Bytes bytes;
    std::size_t first;
    std::optional<std::size_t> fin;
    bool cancelled{false};
    bool invalid{false};
};
template<class Decoder, class Object, class Limits>
void parse_object(const Candidate& stream, unsigned draft, std::uint64_t request_id,
                  Retrieval& result, Limits limits) {
    using Order = std::conditional_t<std::is_same_v<Decoder, d18::FetchDecoder>,
                                    d18::FetchGroupOrder, d21::FetchGroupOrder>;
    limits.maximum_retained_payload_length = kMaximumBytes;
    Decoder decoder([request_id](std::uint64_t id) -> std::optional<Order> {
        return id == request_id ? std::optional{Order::Ascending} : std::nullopt;
    }, limits);
    const auto decoded = decoder.push(stream.bytes, stream.fin.has_value());
    if (!decoded.header || decoded.header->raw_type != 5 ||
        decoded.header->request_id != request_id || decoded.error ||
        !decoded.observations.empty() || decoded.local_api_misuse || stream.invalid || stream.cancelled)
        return;
    const Object* ordinary = nullptr;
    for (const auto& event : decoded.events) {
        const auto* object = std::get_if<Object>(&event);
        if (!object) continue;
        if (object->group_id != 7 || object->object_id != 9 ||
            object->request_id != request_id || ordinary) return;
        if (object->status && *object->status != 0) continue;
        if (object->retained_payload.size() != object->payload_length) return;
        ordinary = object;
    }
    if (!ordinary) return;
    const auto properties = normalized(ordinary->properties);
    const auto immutable = wrapper(draft, properties);
    if (!immutable || !object_properties_valid(properties, *immutable)) return;
    result.ordinary_object = true;
    result.object_fin = decoded.clean_fin;
    if (ordinary->serialization_flags && (*ordinary->serialization_flags & 0x20u) != 0)
        result.object = immutable;
}
Retrieval retrieve(std::span<const transport::TransportEvent> events,
                   const RawProbeAcceptedWrite& request, unsigned draft, std::uint64_t id) {
    Retrieval result;
    if (!request.stream_id || !request.delivery_event_count ||
        *request.delivery_event_count > events.size() || !bounded(events)) return result;
    const auto marker = *request.delivery_event_count;
    Candidate response{{}, marker, {}, false, false};
    std::map<transport::StreamId, Candidate> streams;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            Candidate* candidate = nullptr;
            if (data->stream_id == *request.stream_id) candidate = &response;
            else if ((data->stream_id & 3u) == 2u) {
                if (!streams.contains(data->stream_id)) {
                    if (streams.size() >= 64) return {};
                    streams.emplace(data->stream_id, Candidate{{}, i, {}, false, false});
                }
                candidate = &streams.at(data->stream_id);
            }
            if (!candidate) continue;
            if (candidate->fin || candidate->cancelled) { candidate->invalid = true; continue; }
            if (i < marker && candidate == &response) candidate->invalid = true;
            candidate->bytes.insert(candidate->bytes.end(), data->data.begin(), data->data.end());
            if (data->fin) candidate->fin = i;
        } else {
            std::optional<transport::StreamId> cancelled;
            if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) cancelled = reset->stream_id;
            else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) cancelled = stop->stream_id;
            if (!cancelled) continue;
            if (*cancelled == *request.stream_id) response.cancelled = true;
            if ((*cancelled & 3u) == 2u) {
                if (!streams.contains(*cancelled)) {
                    if (streams.size() >= 64) return {};
                    streams.emplace(*cancelled, Candidate{{}, i, {}, true, false});
                } else streams.at(*cancelled).cancelled = true;
            }
        }
    }
    result.cancelled = response.cancelled;
    if (!response.invalid && !response.cancelled) result.track = accepted_track(draft, response.bytes);
    const Candidate* associated = nullptr;
    for (const auto& [stream_id, candidate] : streams) {
        (void)stream_id;
        wire::Cursor cursor(candidate.bytes);
        const auto type = wire::read_vi64(cursor), request_id = wire::read_vi64(cursor);
        const auto* value = std::get_if<std::uint64_t>(&type);
        const auto* fetched = std::get_if<std::uint64_t>(&request_id);
        if (!value || *value != 5 || !fetched || *fetched != id) continue;
        if (associated || candidate.first < marker) return result;
        associated = &candidate;
    }
    if (!associated || response.cancelled) return result;
    result.cancelled = associated->cancelled;
    if (draft == 18)
        parse_object<d18::FetchDecoder, d18::ObjectEvent>(*associated, draft, id, result, d18::Limits{});
    else parse_object<d21::FetchDecoder, d21::ObjectEvent>(*associated, draft, id, result, d21::Limits{});
    return result;
}
bool first_retrieval_ready(const RawProbeGateInput& input, unsigned draft) {
    if (input.prior_writes.size() != 1 || !bounded(input.events) ||
        std::ranges::any_of(input.events, terminal)) return false;
    const auto& request = input.prior_writes.front();
    const auto fixture = decode_fixture(draft, request.write.bytes);
    if (!fixture || request.write.channel != RawProbeChannel::NewBidi || !request.stream_id ||
        (*request.stream_id & 3u) != 1u || !request.write.fin || !request.fin_accepted ||
        request.accepted != request.write.bytes.size() ||
        request.write.bytes != encode_fetch(draft, *fixture, 1)) return false;
    const auto observation = retrieve(input.events, request, draft, 1);
    return observation.track.has_value() && observation.ordinary_object && observation.object_fin &&
           !observation.cancelled;
}
std::optional<bool> compare(const std::optional<Wrapper>& first,
                            const std::optional<Wrapper>& second, ImmutableRepeatAspect aspect) {
    if (!first || !first->bytes || !second) return {};
    if (aspect == ImmutableRepeatAspect::Presence) return second->bytes.has_value();
    if (!second->bytes) return {};
    if (aspect == ImmutableRepeatAspect::Serialization) return first->bytes == second->bytes;
    return first->contents == second->contents;
}
std::optional<bool> observe(const RawProbeTranscript& t, unsigned draft, ImmutableRepeatAspect aspect) {
    if (t.writes.size() != 2 || !bounded(t.events)) return {};
    const auto first = retrieve(t.events, t.writes[0], draft, 1);
    const auto second = retrieve(t.events, t.writes[1], draft, 3);
    const auto track = compare(first.track, second.track, aspect);
    const auto object = compare(first.object, second.object, aspect);
    if (track == false || object == false) return false;
    if (track == true && object == true && first.object_fin && second.object_fin) return true;
    return {};
}
bool response_ready(const RawProbeTranscript& t, unsigned draft) {
    if (t.writes.size() != 2 || !bounded(t.events)) return false;
    const auto first = retrieve(t.events, t.writes[0], draft, 1);
    const auto second = retrieve(t.events, t.writes[1], draft, 3);
    if (first.track && second.track && first.ordinary_object && second.ordinary_object &&
        first.object_fin && second.object_fin) return true;
    // The same actual context serves all three rows. Finish on a proven
    // violation even when the other identity domain is unavailable.
    for (const auto aspect : {ImmutableRepeatAspect::Content, ImmutableRepeatAspect::Presence,
                              ImmutableRepeatAspect::Serialization})
        if (compare(first.track, second.track, aspect) == false ||
            compare(first.object, second.object, aspect) == false) return true;
    return false;
}
std::vector<ImmutableRepeatProbe> profiles(unsigned draft, std::chrono::milliseconds deadline,
                                         const Fixture& fixture) {
    if (deadline.count() <= 0) throw std::invalid_argument("invalid immutable repeat deadline");
    const auto first = encode_fetch(draft, fixture, 1), second = encode_fetch(draft, fixture, 3);
    std::vector<ImmutableRepeatProbe> result;
    for (const auto aspect : {ImmutableRepeatAspect::Content, ImmutableRepeatAspect::Presence,
                              ImmutableRepeatAspect::Serialization}) {
        const auto index = static_cast<unsigned>(aspect);
        const std::string scenario = draft == 21 ? "d21-immutable-property-repeat"
            : aspect == ImmutableRepeatAspect::Serialization
                ? "repeat-immutable-property-with-alternative-varint-encodings-available"
                : "publish-and-retrieve-same-object-and-track-immutable-properties";
        const char* const d18_ids[] = {"D18-12-7-MUST-NOT-002", "D18-12-7-MUST-NOT-003", "D18-12-7-MUST-NOT-004"};
        const char* const d21_ids[] = {"D21-10-7-MUST-NOT-480", "D21-10-7-MUST-NOT-481", "D21-10-7-MUST-NOT-482"};
        const char* const d18_evaluators[] = {"immutable-property-values-unchanged-across-deliveries",
            "previously-published-immutable-property-remains-present", "immutable-property-key-value-serialization-unchanged"};
        const char* const d21_evaluators[] = {"d21-immutable-property-content-stable",
            "d21-immutable-property-preserved", "d21-immutable-property-bytes-stable"};
        RawProbeDefinition definition{scenario, bytes({0xaf, 0, 0, 0}), {}, true,
            [draft](auto input) { return setup_ready(draft, input); }, deadline,
            [draft](const auto& t) { return response_ready(t, draft); }, {}};
        definition.writes.push_back({RawProbeChannel::NewBidi, first, true});
        RawProbeWrite repeated{RawProbeChannel::NewBidi, second, true};
        repeated.evidence_ready = [draft](const auto& input) { return first_retrieval_ready(input, draft); };
        definition.writes.push_back(std::move(repeated));
        result.push_back({draft == 18 ? d18_ids[index] : d21_ids[index],
                          draft == 18 ? d18_evaluators[index] : d21_evaluators[index],
                          draft, aspect, std::move(definition)});
    }
    return result;
}
} // namespace

std::vector<ImmutableRepeatProbe> draft18_immutable_repeat_probes(
    std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    return profiles(18, deadline, {std::move(ns), std::move(name)});
}
std::vector<ImmutableRepeatProbe> draft21_immutable_repeat_probes(
    std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    return profiles(21, deadline, {std::move(ns), std::move(name)});
}
std::optional<bool> evaluate_immutable_repeat_probe(const RawProbeTranscript& t,
                                                  const ImmutableRepeatProbe& p) {
    if ((p.draft != 18 && p.draft != 21) || p.definition.deadline.count() <= 0 ||
        !bounded(t)) return {};
    const auto fixture = decode_fixture(p.draft, t.writes.front().write.bytes);
    if (!fixture) return {};
    const auto expected = profiles(p.draft, p.definition.deadline, *fixture);
    const auto selected = std::ranges::find_if(expected, [&](const auto& candidate) {
        return candidate.requirement_id == p.requirement_id && candidate.evaluator_id == p.evaluator_id &&
               candidate.aspect == p.aspect && candidate.definition.id == p.definition.id;
    });
    if (selected == expected.end()) return {};
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
    const auto end = std::ranges::find_if(t.events, terminal);
    prefix.events.assign(t.events.begin(), end);
    if (!raw_probe_stimulus_valid(prefix, selected->definition)) return {};
    return observe(t, p.draft, p.aspect);
}

} // namespace moq::interop::scenarios
