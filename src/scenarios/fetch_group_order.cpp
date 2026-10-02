#include "moq/interop/scenarios/fetch_group_order.h"

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
constexpr std::size_t kMaximumBytes = 65546;
constexpr std::size_t kMaximumEvents = 4096;
struct Fixture { Namespace track_namespace; Bytes track_name; };
struct Request { std::uint64_t id; bool descending; bool explicit_order; };
struct Location {
    std::uint64_t group;
    std::uint64_t object;
    auto operator<=>(const Location&) const = default;
};
constexpr Location kStart{7, 0};
constexpr Location kEnd{9, 9};

Bytes bytes(std::initializer_list<unsigned> input) {
    Bytes output;
    for (const auto value : input) output.push_back(static_cast<std::byte>(value));
    return output;
}
std::vector<Request> requests(FetchGroupOrderRequest order) {
    if (order == FetchGroupOrderRequest::BothExplicit)
        return {{1, false, true}, {3, true, true}};
    return {{1, order == FetchGroupOrderRequest::Descending,
                order != FetchGroupOrderRequest::Default}};
}
Bytes encode_fetch(unsigned draft, const Fixture& fixture, const Request& request) {
    if (!fetch_first_object_fixture_valid(fixture.track_namespace, fixture.track_name))
        throw std::invalid_argument("invalid FETCH track fixture");
    wire::ByteWriter output(kMaximumBytes);
    if (draft == 18) {
        const d18::FetchMessage fetch{
            request.id,
            d18::StandaloneFetch{{fixture.track_namespace}, {fixture.track_name},
                                {kStart.group, kStart.object}, {kEnd.group, kEnd.object}},
            {{0x22, d18::Uint8ParameterValue{
                static_cast<std::uint8_t>(request.descending ? 2 : 1)}}}};
        if (!d18::encode_message(d18::Message{fetch}, output).has_value())
            throw std::invalid_argument("unencodable draft18 FETCH fixture");
    } else {
        wire::ByteWriter body(65535), filter(27);
        bool ok = wire::write_vi64(request.id, body) &&
                  wire::write_vi64(fixture.track_namespace.size(), body);
        for (const auto& field : fixture.track_namespace)
            ok = ok && wire::write_length_prefixed_bytes(field, body);
        ok = ok && wire::write_length_prefixed_bytes(fixture.track_name, body) &&
             wire::write_vi64(request.explicit_order ? 2 : 1, body) &&
             wire::write_vi64(0x21, body) &&
             wire::write_vi64(kStart.group, filter) &&
             wire::write_vi64(kStart.object, filter) &&
             wire::write_vi64(kEnd.group - kStart.group, filter) &&
             wire::write_vi64(kEnd.object, filter) &&
             wire::write_length_prefixed_bytes(filter.bytes(), body);
        if (request.explicit_order)
            ok = ok && wire::write_vi64(1, body) &&
                 body.append_byte(static_cast<std::byte>(request.descending ? 2 : 1));
        // Draft21 parameter types are ascending delta encoded: 0x21 then
        // 0x22 is a delta of one. The default request omits GROUP_ORDER.
        ok = ok && wire::write_vi64(0x16, output) &&
             output.append_byte(static_cast<std::byte>(body.size() >> 8u)) &&
             output.append_byte(static_cast<std::byte>(body.size() & 255u)) &&
             output.append_bytes(body.bytes());
        if (!ok) throw std::invalid_argument("unencodable draft21 FETCH fixture");
    }
    return {output.bytes().begin(), output.bytes().end()};
}
std::optional<Fixture> decode_fixture(unsigned draft, std::span<const std::byte> input) {
    if (input.size() > kMaximumBytes) return {};
    Fixture fixture;
    wire::Cursor cursor(input);
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
bool bounded(const RawProbeTranscript& t) {
    if (t.events.size() > kMaximumEvents || t.setup.write.bytes.size() > kMaximumBytes) return false;
    std::size_t total = t.setup.write.bytes.size();
    for (const auto& write : t.writes) {
        if (write.write.bytes.size() > kMaximumBytes - total) return false;
        total += write.write.bytes.size();
    }
    total = 0;
    const auto fits = [&](std::size_t size) {
        if (size > kMaximumBytes - total) return false;
        total += size;
        return true;
    };
    for (const auto& event : t.events) {
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
std::optional<Location> accepted_endpoint(unsigned draft, const Bytes& input) {
    wire::Cursor cursor(input);
    Location location{};
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* ok = message ? std::get_if<d18::FetchOkMessage>(message) : nullptr;
        if (!ok || ok->end_of_track > 1 || !draft18_track_properties_valid(ok->track_properties)) return {};
        location = {ok->end_location.group, ok->end_location.object};
    } else {
        const auto decoded = d21::decode_successful_response(cursor, d21::ResponseContext::Fetch);
        const auto* ok = std::get_if<d21::SuccessfulResponse>(&decoded);
        if (!ok || !ok->end_location) return {};
        location = {ok->end_location->group, ok->end_location->object};
    }
    if (cursor.remaining() != 0 || location < kStart || location > kEnd ||
        (draft == 18 && location.group == kEnd.group && location.object == 0)) return {};
    return location;
}
struct Candidate {
    Bytes bytes;
    std::size_t first;
    std::optional<std::size_t> fin;
    std::optional<std::size_t> reset;
    bool invalid{false};
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
};
struct OrderEvidence {
    std::optional<bool> result;
    std::size_t end_offset{0};
};
template<class Decoder, class Object>
OrderEvidence ordered(const Candidate& stream, const Request& request,
                      const Location& endpoint) {
    using Order = std::conditional_t<std::is_same_v<Decoder, d18::FetchDecoder>,
                                    d18::FetchGroupOrder, d21::FetchGroupOrder>;
    Decoder decoder([request](std::uint64_t id) -> std::optional<Order> {
        if (id != request.id) return {};
        return request.descending ? Order::Descending : Order::Ascending;
    });
    const auto parsed = decoder.push(stream.bytes, stream.fin.has_value());
    if (!parsed.header || parsed.header->raw_type != 5 ||
        parsed.header->request_id != request.id || parsed.local_api_misuse) return {};
    std::optional<std::uint64_t> prior_group;
    bool distinct_groups = false;
    for (const auto& event : parsed.events) {
        const auto* object = std::get_if<Object>(&event);
        if (!object) continue;
        const Location location{object->group_id, object->object_id};
        const bool beyond_end = std::is_same_v<Decoder, d18::FetchDecoder>
            ? location.group > endpoint.group ||
              (location.group == endpoint.group && endpoint.object != 0 &&
               location.object >= endpoint.object)
            : location > endpoint;
        if (object->request_id != request.id || location < kStart || beyond_end) return {};
        if (prior_group && *prior_group != object->group_id) {
            distinct_groups = true;
            const bool violation = request.descending ? object->group_id > *prior_group
                                                       : object->group_id < *prior_group;
            if (violation) {
                const auto end = object->stream_end_offset;
                if ((parsed.error && parsed.error->offset < end) ||
                    std::any_of(parsed.observations.begin(), parsed.observations.end(),
                        [end](const auto& observation) { return observation.offset < end; })) return {};
                // A later partial record or reset cannot erase the completed
                // ordinary records that already contradict the request order.
                return {false, end};
            }
        }
        prior_group = object->group_id;
    }
    if (!distinct_groups || stream.invalid || parsed.error ||
        !parsed.observations.empty() || !parsed.clean_fin) return {};
    return {true, stream.bytes.size()};
}
std::optional<bool> observe_request(const RawProbeTranscript& t, unsigned draft,
                                    const Request& request, std::size_t write_index) {
    if (write_index >= t.writes.size() || !t.writes[write_index].stream_id ||
        !t.writes[write_index].delivery_event_count ||
        *t.writes[write_index].delivery_event_count > t.events.size()) return {};
    const auto request_stream = *t.writes[write_index].stream_id;
    const auto marker = *t.writes[write_index].delivery_event_count;
    Bytes response;
    bool response_closed = false;
    std::optional<Location> endpoint;
    std::optional<std::size_t> accepted_index, request_reset;
    std::map<transport::StreamId, Candidate> streams;
    for (std::size_t i = 0; i < t.events.size(); ++i) {
        const auto& event = t.events[i];
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (data->stream_id == request_stream) {
                if (i < marker || response_closed) return {};
                response.insert(response.end(), data->data.begin(), data->data.end());
                if (const auto location = accepted_endpoint(draft, response)) {
                    endpoint = location;
                    if (!accepted_index) accepted_index = i;
                } else if (endpoint) return {};
                response_closed = data->fin;
            } else if ((data->stream_id & 3u) == 2u) {
                if (!streams.contains(data->stream_id)) {
                    if (streams.size() >= 64) return {};
                    streams.emplace(data->stream_id, Candidate{{}, i, {}, {}, false, {}});
                }
                auto& candidate = streams.at(data->stream_id);
                if (candidate.fin || candidate.reset) { candidate.invalid = true; continue; }
                candidate.bytes.insert(candidate.bytes.end(), data->data.begin(), data->data.end());
                candidate.chunks.emplace_back(candidate.bytes.size(), i);
                if (data->fin) candidate.fin = i;
            }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if (reset->stream_id == request_stream && !request_reset) {
                request_reset = i;
                response_closed = true;
            }
            if ((reset->stream_id & 3u) == 2u) {
                if (!streams.contains(reset->stream_id)) {
                    if (streams.size() >= 64) return {};
                    streams.emplace(reset->stream_id, Candidate{{}, i, {}, i, false, {}});
                } else if (!streams.at(reset->stream_id).reset)
                    streams.at(reset->stream_id).reset = i;
            }
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event);
                   stop && stop->stream_id == request_stream && !request_reset) {
            request_reset = i;
            response_closed = true;
        }
    }
    if (!endpoint || !accepted_index) return {};
    const Candidate* associated = nullptr;
    for (const auto& [id, candidate] : streams) {
        (void)id;
        wire::Cursor cursor(candidate.bytes);
        const auto type = wire::read_vi64(cursor), request_id = wire::read_vi64(cursor);
        const auto* value = std::get_if<std::uint64_t>(&type);
        const auto* fetched = std::get_if<std::uint64_t>(&request_id);
        if (!value || *value != 5 || !fetched || *fetched != request.id) continue;
        if (associated || candidate.first < marker) return {};
        associated = &candidate;
    }
    if (!associated) return {};
    const auto evidence = draft == 18
        ? ordered<d18::FetchDecoder, d18::ObjectEvent>(*associated, request, *endpoint)
        : ordered<d21::FetchDecoder, d21::ObjectEvent>(*associated, request, *endpoint);
    if (!evidence.result.has_value()) return {};
    const auto chunk = std::find_if(associated->chunks.begin(), associated->chunks.end(),
        [&](const auto& entry) { return entry.first >= evidence.end_offset; });
    if (chunk == associated->chunks.end()) return {};
    const auto data_index = *evidence.result ? *associated->fin : chunk->second;
    const auto proof_index = std::max(*accepted_index, data_index);
    if ((associated->reset && *associated->reset < proof_index) ||
        (request_reset && *request_reset < proof_index)) return {};
    return evidence.result;
}
std::optional<bool> observe(const RawProbeTranscript& t, unsigned draft,
                            FetchGroupOrderRequest order) {
    const auto expected_requests = requests(order);
    if (!bounded(t) || t.writes.size() != expected_requests.size()) return {};
    bool missing = false;
    for (std::size_t i = 0; i < expected_requests.size(); ++i) {
        if (i && t.writes[i].stream_id == t.writes[0].stream_id) return {};
        const auto result = observe_request(t, draft, expected_requests[i], i);
        if (result == false) return false;
        missing = missing || !result.has_value();
    }
    return missing ? std::nullopt : std::optional{true};
}
std::vector<FetchGroupOrderProbe> profiles(unsigned draft, std::chrono::milliseconds deadline,
                                         const Fixture& fixture) {
    if (deadline.count() <= 0) throw std::invalid_argument("invalid FETCH group-order deadline");
    const std::vector<FetchGroupOrderRequest> orders = draft == 18
        ? std::vector{FetchGroupOrderRequest::BothExplicit}
        : std::vector{FetchGroupOrderRequest::Ascending, FetchGroupOrderRequest::Descending,
                      FetchGroupOrderRequest::Default};
    std::vector<FetchGroupOrderProbe> result;
    for (const auto order : orders) {
        const std::string id = draft == 18 ? "fetch-multiple-published-groups-in-each-explicit-order"
            : order == FetchGroupOrderRequest::Ascending ? "d21-fetch-ascending-groups"
            : order == FetchGroupOrderRequest::Descending ? "d21-fetch-descending-groups"
                                                        : "d21-fetch-default-group-order";
        RawProbeDefinition definition{id, bytes({0xaf, 0, 0, 0}), {}, true,
            [draft](auto input) { return setup_ready(draft, input); }, deadline,
            [draft, order](const auto& t) { return observe(t, draft, order).has_value(); }, {}};
        for (const auto& request : requests(order))
            definition.writes.push_back({RawProbeChannel::NewBidi, encode_fetch(draft, fixture, request), true});
        result.push_back({draft == 18 ? "D18-10-12-3-MUST-006" : "D21-9-11-MUST-377",
                          draft == 18 ? "fetch-groups-follow-requested-ascending-or-descending-order"
                                      : "d21-fetch-group-order", draft, order, std::move(definition)});
    }
    return result;
}
} // namespace

std::vector<FetchGroupOrderProbe> draft18_fetch_group_order_probes(
    std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    return profiles(18, deadline, {std::move(ns), std::move(name)});
}
std::vector<FetchGroupOrderProbe> draft21_fetch_group_order_probes(
    std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    return profiles(21, deadline, {std::move(ns), std::move(name)});
}
std::optional<bool> evaluate_fetch_group_order_probe(const RawProbeTranscript& t,
                                                    const FetchGroupOrderProbe& p) {
    if ((p.draft != 18 && p.draft != 21) || t.writes.empty() ||
        p.definition.deadline.count() <= 0 || !bounded(t)) return {};
    const auto fixture = decode_fixture(p.draft, t.writes.front().write.bytes);
    if (!fixture) return {};
    const auto expected = profiles(p.draft, p.definition.deadline, *fixture);
    const auto selected = std::find_if(expected.begin(), expected.end(), [&](const auto& candidate) {
        return candidate.requirement_id == p.requirement_id && candidate.evaluator_id == p.evaluator_id &&
               candidate.order == p.order && candidate.definition.id == p.definition.id;
    });
    if (selected == expected.end()) return {};
    // Regeneration validates every actual request, including namespace/name,
    // IDs, parameter presence/order, and accepted write chronology.
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
    if (!raw_probe_stimulus_valid(prefix, selected->definition)) return {};
    return observe(t, p.draft, p.order);
}

} // namespace moq::interop::scenarios
