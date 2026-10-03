#include "moq/interop/scenarios/draft21_gap_a.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/wire/draft21/key_values.h"
#include "moq/interop/wire/draft21/publish_done.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {

namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;

constexpr std::size_t kMaximumBytes = 65546;
constexpr std::size_t kMaximumEvents = kRawProbeMaximumEvents;
constexpr std::size_t kMaximumStreams = 64;
constexpr std::uint64_t kGroup = 7;
constexpr std::uint64_t kObject = 9;
constexpr std::uint64_t kUnknownStreamCount = std::numeric_limits<std::uint64_t>::max();

// Section 9 message types used on request streams.
constexpr std::uint64_t kSubscribeOk = 0x04;
constexpr std::uint64_t kRequestError = 0x05;
constexpr std::uint64_t kRequestOk = 0x07;
constexpr std::uint64_t kNamespace = 0x08;
constexpr std::uint64_t kPublishDone = 0x0b;
constexpr std::uint64_t kFetchOk = 0x18;

struct Fixture {
    Namespace ns;
    Bytes name;
};

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
    if (body.size() > 65535) throw std::invalid_argument("gap probe request too large");
    Bytes result;
    integer(result, type);
    result.push_back(static_cast<std::byte>(body.size() >> 8u));
    result.push_back(static_cast<std::byte>(body.size() & 255u));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

void track(Bytes& body, const Fixture& fixture) {
    integer(body, fixture.ns.size());
    for (const auto& field : fixture.ns) {
        integer(body, field.size());
        body.insert(body.end(), field.begin(), field.end());
    }
    integer(body, fixture.name.size());
    body.insert(body.end(), fixture.name.begin(), fixture.name.end());
}

// Section 9.20.19: FORWARD (0x10, a varint value).
void forward(Bytes& params, std::uint64_t previous, std::uint64_t value) {
    integer(params, 0x10 - previous);
    integer(params, value);
}

// Section 9.20.10: LOCATION_FILTER (0x21) carrying only the listed vi64 fields.
void location_filter(Bytes& params, std::uint64_t previous,
                     std::initializer_list<std::uint64_t> fields) {
    Bytes value;
    for (const auto field : fields) integer(value, field);
    integer(params, 0x21 - previous);
    integer(params, value.size());
    params.insert(params.end(), value.begin(), value.end());
}

enum class Filter { OpenFromObject, BoundedObject, WholeGroup, None };

// Section 9.6: SUBSCRIBE with Request ID 1 (the first server-parity request).
Bytes subscribe(const Fixture& fixture, bool forwarding, Filter filter) {
    Bytes body;
    integer(body, 1);
    track(body, fixture);
    Bytes params;
    std::uint64_t count = 1;
    forward(params, 0, forwarding ? 1 : 0);
    switch (filter) {
    case Filter::OpenFromObject: location_filter(params, 0x10, {kGroup, kObject}); ++count; break;
    case Filter::BoundedObject: location_filter(params, 0x10, {kGroup, kObject, 0, kObject}); ++count; break;
    case Filter::WholeGroup: location_filter(params, 0x10, {kGroup, 0, 0}); ++count; break;
    case Filter::None: break;
    }
    integer(body, count);
    body.insert(body.end(), params.begin(), params.end());
    return frame(3, body);
}

// Section 9.5: REQUEST_UPDATE (Request ID 3) carrying FORWARD=1 and a bounded
// Location filter for Group 7, Object 9.
Bytes bounded_update() {
    Bytes body;
    integer(body, 3);
    Bytes params;
    forward(params, 0, 1);
    location_filter(params, 0x10, {kGroup, kObject, 0, kObject});
    integer(body, 2);
    body.insert(body.end(), params.begin(), params.end());
    return frame(2, body);
}

// Section 9.5.1: raise the Start Location to Group 7, Object 10 (past Object 9).
Bytes raise_start_update() {
    Bytes body;
    integer(body, 3);
    Bytes params;
    location_filter(params, 0, {kGroup, kObject + 1});
    integer(body, 1);
    body.insert(body.end(), params.begin(), params.end());
    return frame(2, body);
}

// Section 8.9: USE_ALIAS 0 names no registered Alias in this session, so the
// update fails and Section 9.5.1 obliges the publisher to end the subscription.
Bytes failing_update() { return bytes({2, 0, 6, 3, 1, 3, 2, 2, 0}); }

// Section 9.11: FETCH (Request ID 5) for exactly Group 7, Object 9.
Bytes fetch(const Fixture& fixture) {
    Bytes body;
    integer(body, 5);
    track(body, fixture);
    Bytes params;
    location_filter(params, 0, {kGroup, kObject, 0, kObject});
    integer(body, 1);
    body.insert(body.end(), params.begin(), params.end());
    return frame(0x16, body);
}

// Section 9.15: SUBSCRIBE_NAMESPACE (Request ID 3) with the empty prefix, so each
// NAMESPACE suffix is a complete namespace.
Bytes subscribe_namespace_all() {
    Bytes body;
    integer(body, 3);
    integer(body, 0);
    integer(body, 0);
    return frame(0x50, body);
}

bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
}

std::optional<std::uint64_t> number(wire::Cursor& cursor) {
    const auto decoded = wire::read_vi64(cursor);
    if (const auto* value = std::get_if<std::uint64_t>(&decoded)) return *value;
    return {};
}

// ---------------------------------------------------------------- messages

struct Message {
    std::uint64_t type;
    Bytes body;
};
struct Messages {
    std::vector<Message> complete;
    bool partial{false};
    bool malformed{false};
};

// Request-stream messages are Type (vi64), Length (16), body (Section 9).
Messages parse_messages(const Bytes& input) {
    Messages result;
    wire::Cursor cursor(input);
    while (cursor.remaining() != 0) {
        auto working = cursor;
        const auto type = wire::read_vi64(working);
        if (std::holds_alternative<wire::NeedMore>(type)) { result.partial = true; break; }
        if (std::holds_alternative<wire::DecodeError>(type)) { result.malformed = true; break; }
        const auto length = wire::read_bytes(working, 2);
        if (std::holds_alternative<wire::NeedMore>(length)) { result.partial = true; break; }
        const auto* prefix = std::get_if<std::span<const std::byte>>(&length);
        if (!prefix) { result.malformed = true; break; }
        const auto size = (static_cast<std::size_t>(std::to_integer<unsigned>((*prefix)[0])) << 8u) |
                          std::to_integer<unsigned>((*prefix)[1]);
        const auto body = wire::read_bytes(working, size);
        if (std::holds_alternative<wire::NeedMore>(body)) { result.partial = true; break; }
        const auto* value = std::get_if<std::span<const std::byte>>(&body);
        if (!value) { result.malformed = true; break; }
        result.complete.push_back({std::get<std::uint64_t>(type), Bytes(value->begin(), value->end())});
        cursor = working;
    }
    return result;
}

bool has_type(const Messages& messages, std::uint64_t type) {
    return std::any_of(messages.complete.begin(), messages.complete.end(),
                       [&](const Message& message) { return message.type == type; });
}

// ----------------------------------------------------------------- streams

struct StreamData {
    Bytes bytes;
    std::size_t first_event{0};
    bool fin{false};
    bool reset{false};
    std::size_t reset_event{0};
    bool overrun{false};  // data arrived after FIN or reset
};
using Streams = std::map<transport::StreamId, StreamData>;

bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
           std::holds_alternative<transport::LocalCloseEvent>(event) ||
           std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
           std::holds_alternative<transport::TransportErrorEvent>(event) ||
           std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}

struct Collected {
    Streams streams;
    bool bounded{true};
};

Collected collect(std::span<const transport::TransportEvent> events) {
    Collected result;
    if (events.size() > kMaximumEvents) { result.bounded = false; return result; }
    std::size_t total = 0;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (data->data.size() > kMaximumBytes - total ||
                (!result.streams.contains(data->stream_id) && result.streams.size() >= kMaximumStreams)) {
                result.bounded = false;
                return result;
            }
            total += data->data.size();
            auto [found, inserted] = result.streams.try_emplace(data->stream_id);
            auto& stream = found->second;
            if (inserted) stream.first_event = index;
            if (stream.fin || stream.reset) { stream.overrun = true; continue; }
            stream.bytes.insert(stream.bytes.end(), data->data.begin(), data->data.end());
            stream.fin = data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if (!result.streams.contains(reset->stream_id) && result.streams.size() >= kMaximumStreams) {
                result.bounded = false;
                return result;
            }
            auto [found, inserted] = result.streams.try_emplace(reset->stream_id);
            if (inserted) found->second.first_event = index;
            if (!found->second.reset) found->second.reset_event = index;
            found->second.reset = true;
        }
    }
    return result;
}

// The publisher's control stream is the first peer unidirectional stream
// whose bytes decode as SETUP (Section 6.3).
std::optional<transport::StreamId> control_stream(const Streams& streams) {
    std::optional<transport::StreamId> best;
    for (const auto& [id, stream] : streams) {
        if ((id & 3u) != 2u) continue;
        wire::Cursor cursor(stream.bytes);
        if (!std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor))) continue;
        if (!best || stream.first_event < streams.at(*best).first_event) best = id;
    }
    return best;
}

// ------------------------------------------------------------ subgroup data

struct SubgroupObject {
    std::uint64_t id;
    std::vector<std::uint64_t> property_types;  // every Object Property type, incl. nested
};
struct Subgroup {
    bool header{false};
    bool malformed{false};
    std::uint64_t flags{0};
    std::uint64_t alias{0};
    std::uint64_t group{0};
    std::optional<std::uint64_t> subgroup_id;
    std::vector<SubgroupObject> objects;
};

void collect_property_types(const d21::KeyValues& values, std::vector<std::uint64_t>& types, unsigned depth) {
    for (const auto& value : values) {
        types.push_back(value.type);
        // Object Immutable Properties (0x0b) wrap a nested property list.
        if (value.type != 0x0b || depth >= 2) continue;
        const auto* nested = std::get_if<Bytes>(&value.value);
        if (!nested) continue;
        wire::Cursor cursor(*nested);
        const auto decoded = d21::decode_key_values_to_end(cursor);
        if (const auto* list = std::get_if<d21::KeyValues>(&decoded))
            collect_property_types(*list, types, depth + 1);
    }
}

// Section 11.3.1 and the Subgroup Object fields of Figure 26, parsed only as
// far as the bytes received so far allow.
Subgroup parse_subgroup(const Bytes& input) {
    Subgroup result;
    wire::Cursor cursor(input);
    const auto flags = number(cursor);
    if (!flags) return result;
    if (*flags >= 128 || (*flags & 0x10u) == 0 || ((*flags >> 1u) & 3u) == 3u) {
        result.malformed = true;
        return result;
    }
    const auto alias = number(cursor);
    const auto group = alias ? number(cursor) : std::nullopt;
    if (!alias || !group) return result;
    const auto mode = (*flags >> 1u) & 3u;
    if (mode == 2u) {
        const auto id = number(cursor);
        if (!id) return result;
        result.subgroup_id = *id;
    } else if (mode == 0u) {
        result.subgroup_id = 0;
    }
    if ((*flags & 0x20u) == 0u && !std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor, 1)))
        return result;
    result.header = true;
    result.flags = *flags;
    result.alias = *alias;
    result.group = *group;
    std::optional<std::uint64_t> previous;
    while (cursor.remaining() != 0) {
        auto working = cursor;
        const auto delta = number(working);
        if (!delta) break;
        SubgroupObject object{previous ? *previous + 1 + *delta : *delta, {}};
        if ((*flags & 0x01u) != 0u) {
            const auto length = number(working);
            if (!length || *length > 65535) break;
            const auto block = wire::read_bytes(working, static_cast<std::size_t>(*length));
            const auto* span = std::get_if<std::span<const std::byte>>(&block);
            if (!span) break;
            wire::Cursor properties(*span);
            const auto decoded = d21::decode_key_values_to_end(properties);
            const auto* list = std::get_if<d21::KeyValues>(&decoded);
            if (!list) { result.malformed = true; break; }
            collect_property_types(*list, object.property_types, 0);
        }
        const auto payload = number(working);
        if (!payload) break;
        if (*payload == 0) {
            if (!number(working)) break;  // Object Status
        } else if (!std::holds_alternative<std::span<const std::byte>>(
                       wire::read_bytes(working, static_cast<std::size_t>(std::min<std::uint64_t>(*payload, kMaximumBytes))))) {
            break;
        }
        cursor = working;
        previous = object.id;
        result.objects.push_back(std::move(object));
    }
    if (mode == 1u && !result.objects.empty()) result.subgroup_id = result.objects.front().id;
    return result;
}

// ------------------------------------------------------------- observation

enum class State { Pending, Inconclusive, Pass, Fail };

struct Response {
    const StreamData* stream{nullptr};
    Messages messages;
};

// The response direction of write `index`, restricted to events after the
// request was fully accepted.
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

bool first_is(const Response& response, std::initializer_list<std::uint64_t> types) {
    return !response.messages.complete.empty() &&
           std::find(types.begin(), types.end(), response.messages.complete.front().type) != types.end();
}

bool subscribe_ok_ready(std::span<const std::byte> input) {
    const auto messages = parse_messages(Bytes(input.begin(), input.end()));
    return !messages.complete.empty() && messages.complete.front().type == kSubscribeOk;
}

// Section 6.4.2.2. A FIN is the end of the direction, so everything in
// `messages` was sent before it.
State response_before_fin(const Response& response) {
    if (!response.stream || response.messages.malformed) return State::Pending;
    if (response.stream->fin && response.messages.complete.empty()) return State::Fail;
    if (response.stream->reset && response.messages.complete.empty()) return State::Inconclusive;
    if (response.messages.complete.empty()) return State::Pending;
    return first_is(response, {kSubscribeOk, kRequestError}) ? State::Pass : State::Inconclusive;
}

// An Established subscription (SUBSCRIBE_OK) terminated after a failed update:
// PUBLISH_DONE must precede the publisher's FIN.
State publish_done_before_fin(const Response& response) {
    if (!response.stream || response.messages.malformed) return State::Pending;
    if (response.messages.complete.empty()) {
        return response.stream->fin || response.stream->reset ? State::Inconclusive : State::Pending;
    }
    // Never established: nothing here is an Established subscription.
    if (!first_is(response, {kSubscribeOk})) return State::Inconclusive;
    if (response.stream->reset && !has_type(response.messages, kPublishDone)) return State::Inconclusive;
    if (response.stream->fin) return has_type(response.messages, kPublishDone) ? State::Pass : State::Fail;
    return State::Pending;
}

// Section 9.12: a FETCH needs its FETCH_OK or REQUEST_ERROR before the FIN.
State fetch_before_fin(const Response& response) {
    if (!response.stream || response.messages.malformed) return State::Pending;
    const bool answered = first_is(response, {kFetchOk, kRequestError});
    if (response.stream->fin) return answered ? State::Pass : State::Fail;
    if (response.stream->reset) return answered ? State::Pass : State::Inconclusive;
    return State::Pending;
}

State combine(State first, State second) {
    if (first == State::Fail || second == State::Fail) return State::Fail;
    if (first == State::Inconclusive || second == State::Inconclusive) return State::Inconclusive;
    if (first == State::Pass && second == State::Pass) return State::Pass;
    return State::Pending;
}

// Subgroup streams of the subscription carried on write 0.
struct Delivery {
    bool established{false};
    bool request_error{false};
    std::uint64_t alias{0};
    std::vector<std::pair<transport::StreamId, Subgroup>> streams;  // by stream ID
    // Parallel to `streams`.
    struct Facts {
        bool fin;
        bool reset;
        std::size_t reset_event;
    };
    std::vector<Facts> facts;
    std::size_t seen_terminal{0};
    std::size_t open{0};
};

Delivery delivery_of(const RawProbeTranscript& t, const Collected& collected) {
    Delivery result;
    const auto response = response_of(t, collected, 0);
    if (!response.stream || response.messages.complete.empty()) return result;
    const auto& first = response.messages.complete.front();
    if (first.type == kRequestError) { result.request_error = true; return result; }
    if (first.type != kSubscribeOk) return result;
    // Track Alias is the first field of SUBSCRIBE_OK (Figure 11). Read it
    // without validating the Track Properties so an unknown Mandatory Track
    // Property cannot hide the delivery from this probe.
    wire::Cursor body(first.body);
    const auto alias = number(body);
    if (!alias) return result;
    result.established = true;
    result.alias = *alias;
    const auto control = control_stream(collected.streams);
    const auto marker = *t.writes[0].delivery_event_count;
    std::vector<std::pair<std::size_t, transport::StreamId>> order;
    for (const auto& [id, stream] : collected.streams) {
        if ((id & 3u) != 2u || id == control || stream.first_event < marker) continue;
        order.emplace_back(stream.first_event, id);
    }
    // Stream IDs grow in the order the publisher opened the streams.
    std::sort(order.begin(), order.end(), [](const auto& left, const auto& right) {
        return left.second < right.second;
    });
    for (const auto& [event, id] : order) {
        const auto& stream = collected.streams.at(id);
        auto subgroup = parse_subgroup(stream.bytes);
        if (!subgroup.header || subgroup.malformed || subgroup.alias != result.alias) continue;
        if (stream.fin || stream.reset) ++result.seen_terminal; else ++result.open;
        result.streams.emplace_back(id, std::move(subgroup));
        result.facts.push_back({stream.fin, stream.reset, stream.reset_event});
    }
    return result;
}

// Section 2.2: the first stream opened for each Subgroup of Group 7 begins
// with the first Object ever published in it, so it must set FIRST_OBJECT.
// This relies on the fixture contract: Group 7 is retained from its first
// Object, and the subscription starts at the beginning of that Group.
State first_object_bit(const Delivery& delivery) {
    if (delivery.request_error) return State::Inconclusive;
    if (!delivery.established || delivery.streams.empty()) return State::Pending;
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    bool judged = false;
    for (const auto& [id, subgroup] : delivery.streams) {
        // Mode 0b01 needs the first Object to name the Subgroup.
        if (!subgroup.subgroup_id || subgroup.group != kGroup) continue;
        if (!seen.insert({subgroup.group, *subgroup.subgroup_id}).second) continue;  // a later stream
        judged = true;
        if ((subgroup.flags & 0x40u) == 0u) return State::Fail;
    }
    return judged ? State::Pass : State::Pending;
}

// Section 3.6: a Mandatory Track Property (0x4000-0x7FFF) as an Object
// Property makes the Object malformed.
State mandatory_property_scope(const Delivery& delivery) {
    if (delivery.request_error) return State::Inconclusive;
    if (!delivery.established) return State::Pending;
    std::size_t objects = 0;
    for (const auto& [id, subgroup] : delivery.streams) {
        for (const auto& object : subgroup.objects) {
            ++objects;
            for (const auto type : object.property_types)
                if (type >= 0x4000 && type <= 0x7fff) return State::Fail;
        }
    }
    return objects != 0 && delivery.seen_terminal != 0 ? State::Pass : State::Pending;
}

// Section 3.3.1: every subscription-delivered Object lies in Group 7, Object 9
// to Group 7, Object 9 inclusive. Updates are asynchronous, so the update
// scenario starts with FORWARD=0 (no Object may be sent) and sets the filter
// and FORWARD=1 in one REQUEST_UPDATE; every Object seen was sent after it.
State bounded_range(const Delivery& delivery, bool window_ended) {
    if (delivery.request_error) return State::Inconclusive;
    if (!delivery.established) return window_ended ? State::Inconclusive : State::Pending;
    std::size_t in_range = 0;
    for (const auto& [id, subgroup] : delivery.streams) {
        for (const auto& object : subgroup.objects) {
            if (subgroup.group == kGroup && object.id == kObject) ++in_range;
            else return State::Fail;
        }
    }
    if (!window_ended) return State::Pending;
    return in_range != 0 ? State::Pass : State::Inconclusive;
}

// Section 4.2: the original publisher sends NAMESPACE for each namespace that
// matches a SUBSCRIBE_NAMESPACE prefix. With the empty prefix the suffix is the
// whole fixture namespace.
State namespace_discovery(const RawProbeTranscript& t, const Collected& collected, const Fixture& fixture) {
    const auto response = response_of(t, collected, 1);
    if (!response.stream || response.messages.malformed) return State::Pending;
    if (response.messages.complete.empty()) {
        return response.stream->fin || response.stream->reset ? State::Inconclusive : State::Pending;
    }
    if (response.messages.complete.front().type == kRequestError) return State::Inconclusive;
    if (response.messages.complete.front().type != kRequestOk) return State::Inconclusive;
    for (const auto& message : response.messages.complete) {
        if (message.type != kNamespace) continue;
        wire::Cursor body(message.body);
        const auto count = number(body);
        if (!count || *count > 32) continue;
        Namespace suffix;
        bool ok = true;
        std::size_t total = 0;
        for (std::uint64_t index = 0; index < *count && ok; ++index) {
            const auto field = wire::read_length_prefixed_bytes(body, 4096 - total);
            const auto* value = std::get_if<std::span<const std::byte>>(&field);
            if (!value) { ok = false; break; }
            total += value->size();
            suffix.emplace_back(value->begin(), value->end());
        }
        if (ok && suffix == fixture.ns) return State::Pass;
    }
    // A FIN means every active namespace is treated as done (Section 9.15).
    if (response.stream->fin) return State::Fail;
    if (response.stream->reset) return State::Inconclusive;
    return State::Pending;
}

// Section 6.3: the control stream must not be closed during the session.
State control_stream_lifetime(const RawProbeTranscript& t, const Collected& collected) {
    const auto control = control_stream(collected.streams);
    if (!control) return State::Pending;
    const auto& stream = collected.streams.at(*control);
    if (stream.fin || stream.reset) return State::Fail;
    const auto response = response_of(t, collected, 0);
    if (response.stream && first_is(response, {kSubscribeOk, kRequestError})) return State::Pass;
    return State::Pending;
}

// Section 2.2: Objects of one Subgroup must not be sent on different streams
// of a subscription unless an earlier stream was reset prematurely (a FIN, or a
// reset after the FIN, is a complete stream) or upstream conditions forced the
// later stream's Objects out of Object ID order.
bool subgroup_split_violation(const Delivery& delivery) {
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::size_t>> by_subgroup;
    for (std::size_t index = 0; index < delivery.streams.size(); ++index) {
        const auto& subgroup = delivery.streams[index].second;
        if (!subgroup.subgroup_id) continue;  // mode 0b01 before its first Object
        by_subgroup[{subgroup.group, *subgroup.subgroup_id}].push_back(index);
    }
    for (const auto& [subgroup_key, indexes] : by_subgroup) {
        for (std::size_t position = 1; position < indexes.size(); ++position) {
            const auto earlier = indexes[position - 1];
            const auto later = indexes[position];
            const auto& facts = delivery.facts[earlier];
            if (facts.reset && !facts.fin) continue;  // premature reset permits a restart
            const auto& before = delivery.streams[earlier].second.objects;
            const auto& after = delivery.streams[later].second.objects;
            if (!before.empty() && !after.empty()) {
                std::uint64_t highest = 0;
                for (const auto& object : before) highest = std::max(highest, object.id);
                if (after.front().id < highest) continue;  // forced out of Object ID order
            }
            // A later stream with no Object yet cannot be called a split.
            if (after.empty()) continue;
            return true;
        }
    }
    return false;
}

State single_subgroup(const Delivery& delivery, bool window_ended) {
    if (delivery.request_error) return State::Inconclusive;
    if (!delivery.established) return window_ended ? State::Inconclusive : State::Pending;
    if (subgroup_split_violation(delivery)) return State::Fail;
    if (!window_ended) return State::Pending;
    return delivery.streams.empty() ? State::Inconclusive : State::Pass;
}

// The update raises the Start Location, so an open stream must be reset
// (Section 11.3.2). A restart on a new stream is then permitted.
State restart_after_reset(const RawProbeTranscript& t, const Collected& collected,
                          const Delivery& delivery, bool window_ended) {
    if (delivery.request_error) return State::Inconclusive;
    if (!delivery.established) return window_ended ? State::Inconclusive : State::Pending;
    if (subgroup_split_violation(delivery)) return State::Fail;
    if (!window_ended) return State::Pending;
    const auto response = response_of(t, collected, 0);
    if (t.writes.size() < 2 || !t.writes[1].delivery_event_count || !response.stream ||
        !has_type(response.messages, kRequestOk) || has_type(response.messages, kRequestError))
        return State::Inconclusive;
    const auto update_marker = *t.writes[1].delivery_event_count;
    for (const auto& facts : delivery.facts)
        if (facts.reset && !facts.fin && facts.reset_event >= update_marker) return State::Pass;
    return State::Inconclusive;  // nothing was reset, so the exception was never exercised
}

State observe(const RawProbeTranscript& t, Draft21GapAspect aspect, const Fixture& fixture,
              bool window_ended) {
    if (t.writes.empty() || !t.stimulus_delivered) return State::Pending;
    const auto collected = collect(t.events);
    if (!collected.bounded) return State::Inconclusive;
    switch (aspect) {
    case Draft21GapAspect::ResponseBeforeFin:
        return response_before_fin(response_of(t, collected, 0));
    case Draft21GapAspect::PublishDoneBeforeFin:
        return publish_done_before_fin(response_of(t, collected, 0));
    case Draft21GapAspect::TerminalMessageOrder:
        return combine(publish_done_before_fin(response_of(t, collected, 0)),
                       fetch_before_fin(response_of(t, collected, 2)));
    case Draft21GapAspect::FirstObjectBit:
        return first_object_bit(delivery_of(t, collected));
    case Draft21GapAspect::MandatoryPropertyScope:
        return mandatory_property_scope(delivery_of(t, collected));
    case Draft21GapAspect::NamespaceDiscovery:
        return namespace_discovery(t, collected, fixture);
    case Draft21GapAspect::ControlStreamLifetime:
        return control_stream_lifetime(t, collected);
    case Draft21GapAspect::BoundedRange:
        return bounded_range(delivery_of(t, collected), window_ended);
    case Draft21GapAspect::UpdatedRange: {
        // The filter only applies once the update succeeded: REQUEST_OK follows
        // SUBSCRIBE_OK on the request stream (Section 9.5).
        const auto response = response_of(t, collected, 0);
        if (response.stream && first_is(response, {kSubscribeOk})) {
            if (has_type(response.messages, kRequestError)) return State::Inconclusive;
            if (!has_type(response.messages, kRequestOk))
                return window_ended ? State::Inconclusive : State::Pending;
        }
        return bounded_range(delivery_of(t, collected), window_ended);
    }
    case Draft21GapAspect::SingleSubgroup:
        return single_subgroup(delivery_of(t, collected), window_ended);
    case Draft21GapAspect::SubgroupRestartAfterReset:
        return restart_after_reset(t, collected, delivery_of(t, collected), window_ended);
    case Draft21GapAspect::DatagramSupport:
    case Draft21GapAspect::DatagramNegotiation:
        return State::Pending;
    }
    return State::Pending;
}

// Section 6.2: the QUIC DATAGRAM extension must be supported and negotiated.
// The listener only reports an established connection when the publisher
// advertised DATAGRAM support, and closes with this reason when it did not.
State datagram_support(const RawProbeTranscript& t) {
    if (t.events.size() > kMaximumEvents) return State::Inconclusive;
    bool established = false;
    for (const auto& event : t.events) {
        if (const auto* connection = std::get_if<transport::ConnectionEstablishedEvent>(&event)) {
            if (established) return State::Inconclusive;
            established = true;
            if (connection->max_datagram_payload == 0) return State::Fail;
        } else if (const auto* close = std::get_if<transport::LocalCloseEvent>(&event)) {
            static constexpr std::string_view reason = "QUIC DATAGRAM not negotiated";
            const std::string_view text{reinterpret_cast<const char*>(close->reason.data()), close->reason.size()};
            if (!established && text == reason) return State::Fail;
        }
    }
    return established && t.peer_setup_received ? State::Pass : State::Inconclusive;
}

// ---------------------------------------------------------------- profiles

RawProbeWrite new_request(Bytes payload, bool fin = false) {
    return {RawProbeChannel::NewBidi, std::move(payload), fin};
}
RawProbeWrite update_on_stream_zero(Bytes payload) {
    RawProbeWrite result{RawProbeChannel::NewBidi, std::move(payload), false, 0, {}};
    result.peer_response_ready = subscribe_ok_ready;
    return result;
}

// The update is only sent while a subgroup stream of the subscription is open:
// a stream that already finished has nothing left to reset.
RawProbeWrite update_while_stream_open(Bytes payload) {
    auto result = update_on_stream_zero(std::move(payload));
    result.evidence_ready = [](const RawProbeGateInput& input) {
        RawProbeTranscript t;
        t.writes.assign(input.prior_writes.begin(), input.prior_writes.end());
        t.events.assign(input.events.begin(), input.events.end());
        const auto collected = collect(t.events);
        if (!collected.bounded || t.writes.empty() || !t.writes[0].delivery_event_count) return false;
        const auto delivery = delivery_of(t, collected);
        return delivery.established && delivery.open != 0;
    };
    return result;
}

std::vector<Draft21GapProbe> profiles(std::chrono::milliseconds deadline, const Fixture& fixture) {
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid gap A probe fixture or deadline");
    std::vector<Draft21GapProbe> result;
    const auto add = [&](const char* requirement, const char* scenario, const char* evaluator,
                         Draft21GapAspect aspect, std::vector<RawProbeWrite> writes) {
        RawProbeDefinition definition;
        definition.id = scenario;
        definition.setup_bytes = bytes({0xaf, 0, 0, 0});
        definition.writes = std::move(writes);
        definition.deadline = deadline;
        definition.peer_setup_ready = setup_ready;
        definition.response_ready = [aspect, fixture](const RawProbeTranscript& t) {
            // The range probes need the whole window unless an Object already
            // proves a violation; every other probe settles on its evidence.
            if (aspect == Draft21GapAspect::DatagramSupport ||
                aspect == Draft21GapAspect::DatagramNegotiation) {
                const auto state = datagram_support(t);
                return state == State::Pass || state == State::Fail;
            }
            return observe(t, aspect, fixture, false) != State::Pending;
        };
        result.push_back({requirement, evaluator, aspect, std::move(definition)});
    };
    const auto session_probe = [&] {
        return std::vector<RawProbeWrite>{new_request(subscribe(fixture, true, Filter::OpenFromObject))};
    };
    const auto failing_update_probe = [&] {
        return std::vector<RawProbeWrite>{new_request(subscribe(fixture, true, Filter::OpenFromObject)),
                                          update_on_stream_zero(failing_update())};
    };
    add("D21-6-4-2-2-MUST-157", "d21-publisher-request-response-before-fin",
        "d21-request-response-before-fin", Draft21GapAspect::ResponseBeforeFin, session_probe());
    add("D21-6-4-2-2-MUST-158", "d21-established-subscription-publisher-fin",
        "d21-publish-done-before-request-fin", Draft21GapAspect::PublishDoneBeforeFin,
        failing_update_probe());
    {
        auto writes = failing_update_probe();
        writes.push_back(new_request(fetch(fixture), true));
        add("D21-6-4-2-2-MUST-NOT-156", "d21-request-stream-terminal-message-order",
            "d21-all-required-request-messages-before-fin", Draft21GapAspect::TerminalMessageOrder,
            std::move(writes));
    }
    add("D21-2-2-MUST-020", "d21-original-publisher-opens-new-subgroup",
        "d21-new-subgroup-first-object-bit", Draft21GapAspect::FirstObjectBit,
        {new_request(subscribe(fixture, true, Filter::WholeGroup))});
    add("D21-3-6-MUST-070", "d21-publish-track-with-mandatory-property",
        "d21-mandatory-properties-confined-to-track-scope", Draft21GapAspect::MandatoryPropertyScope,
        {new_request(subscribe(fixture, true, Filter::WholeGroup))});
    {
        // Request ID 1 names the fixture track so the evaluator can recover it.
        RawProbeWrite discovery{RawProbeChannel::NewBidi, subscribe_namespace_all(), false};
        add("D21-4-2-MUST-089", "d21-discover-original-publisher-namespaces",
            "d21-original-publisher-matching-namespace-notification",
            Draft21GapAspect::NamespaceDiscovery,
            {new_request(subscribe(fixture, false, Filter::None)), std::move(discovery)});
    }
    add("D21-6-3-MUST-NOT-146", "d21-control-stream-lifetime",
        "d21-control-stream-open-during-session", Draft21GapAspect::ControlStreamLifetime,
        session_probe());
    for (const char* scenario : {"d21-native-quic-datagram-support", "d21-webtransport-h3-datagram-support"})
        add("D21-6-2-MUST-139", scenario, "d21-quic-datagram-extension-supported",
            Draft21GapAspect::DatagramSupport, session_probe());
    // Section 6.2: "MUST be supported and negotiated". Negotiation is visible on
    // the transport independently of what the publisher compiled in: a client
    // that did not offer the extension is refused by the listener and never
    // completes MOQT SETUP, which is a failure of the negotiation duty.
    for (const char* scenario : {"d21-native-quic-without-datagram-negotiation",
                                 "d21-webtransport-h3-without-datagram-negotiation"})
        add("D21-6-2-MUST-140", scenario, "d21-no-moqt-session-without-quic-datagram-negotiation",
            Draft21GapAspect::DatagramNegotiation, session_probe());
    add("D21-3-3-1-MUST-NOT-057", "d21-subscribe-bounded-location-range",
        "d21-subscription-objects-within-effective-location-range", Draft21GapAspect::BoundedRange,
        {new_request(subscribe(fixture, true, Filter::BoundedObject))});
    add("D21-2-2-MUST-NOT-018", "d21-subscribe-single-subgroup",
        "d21-subgroup-stream-splitting-respects-exceptions", Draft21GapAspect::SingleSubgroup,
        {new_request(subscribe(fixture, true, Filter::WholeGroup))});
    add("D21-2-2-MUST-NOT-018", "d21-subgroup-restart-after-reset",
        "d21-subgroup-stream-splitting-respects-exceptions", Draft21GapAspect::SubgroupRestartAfterReset,
        {new_request(subscribe(fixture, true, Filter::WholeGroup)),
         update_while_stream_open(raise_start_update())});
    add("D21-3-3-1-MUST-NOT-057", "d21-update-subscription-location-range",
        "d21-subscription-objects-within-effective-location-range", Draft21GapAspect::UpdatedRange,
        {new_request(subscribe(fixture, false, Filter::None)), update_on_stream_zero(bounded_update())});
    return result;
}

// Only the track identity is configurable; every other byte is regenerated.
std::optional<Fixture> recover_fixture(std::span<const std::byte> input) {
    if (input.size() > kMaximumBytes) return {};
    wire::Cursor cursor(input);
    const auto decoded = d21::decode_request_frame(cursor, true);
    const auto* request = std::get_if<d21::RequestFrame>(&decoded);
    if (!request || request->type.type != 3 || cursor.remaining() != 0) return {};
    wire::Cursor body(request->body);
    const auto id = number(body);
    const auto count = number(body);
    if (id != 1 || !count || *count > 32) return {};
    Fixture fixture;
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
    if (!fetch_first_object_fixture_valid(fixture.ns, fixture.name)) return {};
    return fixture;
}

}  // namespace

std::vector<Draft21GapProbe> draft21_gap_a_probes(std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    return profiles(deadline, {std::move(ns), std::move(name)});
}

std::optional<bool> evaluate_draft21_gap_a_probe(const RawProbeTranscript& t, const Draft21GapProbe& p) {
    if (t.scenario_id != p.definition.id || p.definition.deadline.count() <= 0) return {};
    if (p.aspect == Draft21GapAspect::DatagramSupport ||
        p.aspect == Draft21GapAspect::DatagramNegotiation) {
        // No request is needed to prove DATAGRAM negotiation, and a publisher
        // that lacks it never completes a stimulus; judge the transport events.
        if (t.harness_failed) return {};
        const auto state = datagram_support(t);
        if (state == State::Pass) return true;
        if (state == State::Fail) return false;
        return {};
    }
    if (t.writes.empty() || t.harness_failed) return {};
    const auto fixture = recover_fixture(t.writes.front().write.bytes);
    if (!fixture) return {};
    const auto candidates = profiles(p.definition.deadline, *fixture);
    const auto expected = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return candidate.requirement_id == p.requirement_id && candidate.evaluator_id == p.evaluator_id &&
               candidate.aspect == p.aspect && candidate.definition.id == p.definition.id;
    });
    if (expected == candidates.end()) return {};
    // The window probes end at their deadline, so a timed-out context still
    // carries a full observation. Everything else must have completed.
    const bool window = p.aspect == Draft21GapAspect::BoundedRange ||
                        p.aspect == Draft21GapAspect::UpdatedRange ||
                        p.aspect == Draft21GapAspect::SingleSubgroup ||
                        p.aspect == Draft21GapAspect::SubgroupRestartAfterReset;
    if (!t.complete && !(window && t.timed_out)) return {};
    RawProbeTranscript prefix = t;
    const auto end = std::find_if(t.events.begin(), t.events.end(), terminal);
    prefix.events.assign(t.events.begin(), end);
    prefix.complete = true;
    prefix.timed_out = false;
    if (!raw_probe_stimulus_valid(prefix, expected->definition)) return {};
    const bool ended = window && (t.timed_out || end != t.events.end());
    const auto state = observe(prefix, p.aspect, *fixture, ended);
    if (state == State::Pass) return true;
    if (state == State::Fail) return false;
    return {};
}

}  // namespace moq::interop::scenarios
