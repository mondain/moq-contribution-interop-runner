#include "moq/interop/scenarios/draft22_location_range.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/location_filter_param.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft21/objects.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <algorithm>
#include <compare>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {

namespace d21 = wire::draft21;
namespace d22 = wire::draft22;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
using FilterType = d22::LocationFilterType;

constexpr std::size_t kMaximumBytes = 65546;
constexpr std::size_t kMaximumEvents = kRawProbeMaximumEvents;
constexpr std::size_t kMaximumStreams = 64;
constexpr std::uint64_t kGroup = 7;
constexpr std::uint64_t kObject = 9;
constexpr std::uint64_t kLargestValue = std::numeric_limits<std::uint64_t>::max();
constexpr std::chrono::milliseconds kRebuildDeadline{1000};

// Section 9 message and parameter types.
constexpr std::uint64_t kRequestUpdate = 0x02;
constexpr std::uint64_t kSubscribe = 0x03;
constexpr std::uint64_t kSubscribeOk = 0x04;
constexpr std::uint64_t kRequestOk = 0x07;
constexpr std::uint64_t kFetch = 0x16;
constexpr std::uint64_t kFetchOk = 0x18;
constexpr std::uint64_t kFetchHeader = 0x05;
constexpr std::uint64_t kLargestObject = 0x09;
constexpr std::uint64_t kForward = 0x10;
constexpr std::uint64_t kLocationFilter = 0x21;

enum class Kind { Subscribe, Update, Fetch };

struct Fixture {
    Namespace ns;
    Bytes name;
};

// ------------------------------------------------------------------ requests

void integer(Bytes& output, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64 capacity");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}

// Section 9: Type (vi64), Length (16), body.
Bytes frame(std::uint64_t type, const Bytes& body) {
    if (body.size() > 65535) throw std::invalid_argument("location range request too large");
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

// The bytes after LOCATION_FILTER's type delta (Section 9.20.9). The forms the draft 21 field list can
// express go through the wire-aware builder; Relative Start (0x01) has no draft 21 field list, and Next
// Object (0x05) is written explicitly rather than through the builder's {0, 0} mapping.
Bytes filter_value(const d22::LocationFilter& filter) {
    switch (filter.type) {
    case FilterType::Absolute:
        return filter_param_value({filter.start_group, filter.start_object});
    case FilterType::AbsoluteBounded:
        return filter_param_value({filter.start_group, filter.start_object, filter.end_group_delta.value_or(0)});
    case FilterType::AbsoluteRange:
        return filter_param_value({filter.start_group, filter.start_object, filter.end_group_delta.value_or(0),
                                   filter.end_object.value_or(0)});
    case FilterType::None:
    case FilterType::RelativeGroup:
    case FilterType::NextObject:
        break;
    }
    wire::ByteWriter writer(64);
    if (d22::encode_location_filter(filter, writer)) throw std::logic_error("unencodable draft 22 Location filter");
    return {writer.bytes().begin(), writer.bytes().end()};
}

// Section 9.20.18: FORWARD (0x10, a varint) as the first parameter.
void forward(Bytes& params, std::uint64_t value) {
    integer(params, kForward);
    integer(params, value);
}

void location_filter(Bytes& params, std::uint64_t previous, const d22::LocationFilter& filter) {
    integer(params, kLocationFilter - previous);
    const auto value = filter_value(filter);
    params.insert(params.end(), value.begin(), value.end());
}

// Section 9.6: SUBSCRIBE carrying FORWARD and, when given, LOCATION_FILTER (types ascending).
Bytes subscribe(std::uint64_t request_id, const Fixture& fixture, bool forwarding,
                const d22::LocationFilter* filter) {
    Bytes body;
    integer(body, request_id);
    track(body, fixture);
    Bytes params;
    forward(params, forwarding ? 1 : 0);
    if (filter) location_filter(params, kForward, *filter);
    integer(body, filter ? 2 : 1);
    body.insert(body.end(), params.begin(), params.end());
    return frame(kSubscribe, body);
}

// Section 9.5: REQUEST_UPDATE setting FORWARD=1 and `filter` in one message, so the subscription (opened
// with FORWARD=0) sends nothing before the filter applies.
Bytes request_update(std::uint64_t request_id, const d22::LocationFilter& filter) {
    Bytes body;
    integer(body, request_id);
    Bytes params;
    forward(params, 1);
    location_filter(params, kForward, filter);
    integer(body, 2);
    body.insert(body.end(), params.begin(), params.end());
    return frame(kRequestUpdate, body);
}

bool subscribe_ok_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto type = wire::read_vi64(cursor);
    const auto* value = std::get_if<std::uint64_t>(&type);
    return value && *value == kSubscribeOk;
}

// Section 9.11: FETCH whose range is the LOCATION_FILTER, its only parameter.
Bytes fetch(std::uint64_t request_id, const Fixture& fixture, const d22::LocationFilter& filter) {
    Bytes body;
    integer(body, request_id);
    track(body, fixture);
    Bytes params;
    location_filter(params, 0, filter);
    integer(body, 1);
    body.insert(body.end(), params.begin(), params.end());
    return frame(kFetch, body);
}

// Request IDs of the server-parity requests: 1, 3, 5, ...
std::uint64_t request_id(std::size_t index) { return 1 + 2 * static_cast<std::uint64_t>(index); }

bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
}

std::optional<std::uint64_t> number(wire::Cursor& cursor) {
    const auto decoded = wire::read_vi64(cursor);
    if (const auto* value = std::get_if<std::uint64_t>(&decoded)) return *value;
    return {};
}

// ------------------------------------------------------------------ messages

struct Message {
    std::uint64_t type;
    Bytes frame;  // type, length and body, as received
    Bytes body;
};
struct Messages {
    std::vector<Message> complete;
    bool malformed{false};
};

// Request-stream messages are Type (vi64), Length (16), body (Section 9).
Messages parse_messages(const Bytes& input) {
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

// ------------------------------------------------------------------- streams

struct StreamData {
    Bytes bytes;
    std::size_t first_event{0};
    bool fin{false};
    bool reset{false};
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

// Every stream the publisher wrote before the session ended, within the gap A bounds.
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
            found->second.reset = true;
        }
    }
    return result;
}

// The publisher's control stream is the first peer unidirectional stream whose bytes decode as SETUP
// (Section 6.3).
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

struct Response {
    const StreamData* stream{nullptr};
    Messages messages;
};

// The response direction of write `index`, when it began after the request was fully accepted.
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

// -------------------------------------------------------------------- ranges

struct Location {
    std::uint64_t group;
    std::uint64_t object;
    friend auto operator<=>(const Location&, const Location&) = default;
};

// An inclusive range; no end is open-ended.
struct Range {
    Location start;
    std::optional<Location> end;
    bool contains(Location location) const { return start <= location && (!end || location <= *end); }
};

// What a response said about the Largest Object: `decoded` is false when the response could not be
// read, `value` is empty when it omitted LARGEST_OBJECT (nothing published, Section 9.20.17).
struct LargestReport {
    bool decoded{false};
    std::optional<Location> value;
};

LargestReport largest_of(const Message& message, d21::ResponseContext context) {
    wire::Cursor cursor(message.frame);
    const auto decoded = d21::decode_successful_response(cursor, context);
    const auto* ok = std::get_if<d21::SuccessfulResponse>(&decoded);
    if (!ok || cursor.remaining() != 0) return {};
    for (const auto& parameter : ok->parameters) {
        if (parameter.type != kLargestObject) continue;
        if (const auto* location = std::get_if<d21::Location>(&parameter.value))
            return {true, Location{location->group, location->object}};
        return {};
    }
    return {true, std::nullopt};
}

// Section 9.20.9, Table 6: the range a filter selects on a subscription, given the Largest Object the
// publisher reported for it. No value when the range depends on a Largest Object that is not known.
std::optional<Range> subscription_range(const d22::LocationFilter& filter, const LargestReport& largest) {
    const Location start{filter.start_group, filter.start_object};
    switch (filter.type) {
    case FilterType::None:
        return Range{{0, 0}, std::nullopt};
    case FilterType::RelativeGroup: {
        // {Largest Object.Group + 1 - StartGroup, 0}, clamped to [0, 2^64 - 1].
        if (!largest.decoded || !largest.value) return std::nullopt;
        const auto group = largest.value->group;
        std::uint64_t first = 0;
        if (filter.start_group == 0) first = group == kLargestValue ? kLargestValue : group + 1;
        else if (filter.start_group - 1 <= group) first = group - (filter.start_group - 1);
        return Range{{first, 0}, std::nullopt};
    }
    case FilterType::Absolute:
        return Range{start, std::nullopt};
    case FilterType::AbsoluteBounded:
        // The last Object of Group StartGroup + EndGroupDelta (the encoder refused an overflow).
        return Range{start, Location{filter.start_group + filter.end_group_delta.value_or(0), kLargestValue}};
    case FilterType::AbsoluteRange:
        return Range{start, Location{filter.start_group + filter.end_group_delta.value_or(0),
                                     filter.end_object.value_or(0)}};
    case FilterType::NextObject:
        // Section 3.1.4: {Largest Object.Group, Largest Object.Object + 1}, or {0, 0} with no content.
        if (!largest.decoded) return std::nullopt;
        if (!largest.value) return Range{{0, 0}, std::nullopt};
        if (largest.value->object == kLargestValue) return std::nullopt;
        return Range{{largest.value->group, largest.value->object + 1}, std::nullopt};
    }
    return std::nullopt;
}

// ------------------------------------------------------------ subscriptions

enum class State { Pending, Inconclusive, Pass, Fail };

enum class Answer { Unanswered, Rejected, Established };

struct Subscription {
    Answer answer{Answer::Unanswered};
    std::uint64_t alias{0};
    std::optional<Range> range;  // the effective range; empty when it cannot be known
};

// The subscription opened by write `index`, whose filter is `filter` from the SUBSCRIBE (Kind::Subscribe)
// or from the REQUEST_UPDATE that follows it on the same stream (Kind::Update).
Subscription subscription_of(const RawProbeTranscript& t, const Collected& collected, std::size_t index,
                             const d22::LocationFilter& filter, Kind kind) {
    Subscription result;
    const auto response = response_of(t, collected, index);
    if (!response.stream || response.messages.malformed) return result;
    if (response.messages.complete.empty()) {
        // A request stream closed without any answer establishes nothing.
        if (response.stream->fin || response.stream->reset) result.answer = Answer::Rejected;
        return result;
    }
    const auto& first = response.messages.complete.front();
    // REQUEST_ERROR, or anything that is not a SUBSCRIBE_OK, establishes nothing.
    result.answer = Answer::Rejected;
    if (first.type != kSubscribeOk) return result;
    // Track Alias is the first field of SUBSCRIBE_OK (Section 9.7).
    wire::Cursor body(first.body);
    const auto alias = number(body);
    if (!alias) return result;
    result.alias = *alias;
    if (kind == Kind::Subscribe) {
        result.answer = Answer::Established;
        result.range = subscription_range(filter, largest_of(first, d21::ResponseContext::Subscribe));
        return result;
    }
    // The filter applies only once the update succeeded: REQUEST_OK follows SUBSCRIBE_OK (Section 9.5).
    // Its LARGEST_OBJECT is the Largest Object the publisher resolved the updated filter against.
    if (response.messages.complete.size() < 2) {
        // Awaiting the update's answer: the alias is known, the range is not (yet), so Objects carrying
        // this alias are not judged and no pass is possible, while other aliases still are.
        result.answer = response.stream->fin || response.stream->reset ? Answer::Rejected : Answer::Established;
        return result;
    }
    const auto& update = response.messages.complete[1];
    if (update.type != kRequestOk) return result;  // REQUEST_ERROR: the update never applied
    result.answer = Answer::Established;
    result.range = subscription_range(filter, largest_of(update, d21::ResponseContext::RequestUpdate));
    return result;
}

struct Delivered {
    std::uint64_t alias;
    Location location;
};

// Every complete Object on a subgroup stream the publisher opened after the first request was accepted
// (Section 11.3). Datagrams are not read, as in the draft 21 counterpart.
std::vector<Delivered> delivered_objects(const RawProbeTranscript& t, const Collected& collected) {
    std::vector<Delivered> result;
    if (t.writes.empty() || !t.writes.front().delivery_event_count) return result;
    const auto marker = *t.writes.front().delivery_event_count;
    const auto control = control_stream(collected.streams);
    for (const auto& [id, stream] : collected.streams) {
        if ((id & 3u) != 2u || id == control || stream.first_event < marker) continue;
        d21::SubgroupDecoder decoder;
        // Objects are reported once complete, so a truncated tail is never judged.
        const auto decoded = decoder.push(stream.bytes, false);
        if (!decoded.header) continue;
        for (const auto& object : decoded.objects)
            result.push_back({decoded.header->track_alias, {object.group_id, object.object_id}});
    }
    return result;
}

std::size_t subscription_count() { return draft22_location_range_filters().size(); }

// The writes a probe makes: one request per filter (SUBSCRIBE or FETCH), and for Kind::Update one
// REQUEST_UPDATE each.
std::size_t write_count(Kind kind) { return subscription_count() * (kind == Kind::Update ? 2 : 1); }

// Section 3.3.1 over every subscription of the probe. Subscriptions to one Track may share a Track
// Alias (Section 3.1: "the subscriber re-applies each subscription's filter to determine which
// subscription a received Object belongs to"), so an Object is outside the
// requested range only when it fits none of the subscriptions that carry its alias. No failure is
// claimed while any subscription is unanswered (its alias is unknown) or for an Object whose alias is
// shared with a subscription whose range cannot be known.
State observe_subscriptions(const RawProbeTranscript& t, const Collected& collected, Kind kind,
                            bool window_ended) {
    const auto filters = draft22_location_range_filters();
    // Until every write went out (an update waits for its SUBSCRIBE_OK) nothing can be settled.
    if (t.writes.size() < write_count(kind)) return State::Pending;
    std::vector<Subscription> subscriptions;
    for (std::size_t index = 0; index < subscription_count(); ++index)
        subscriptions.push_back(subscription_of(t, collected, index, filters[index], kind));
    const auto any = [&](auto predicate) {
        return std::any_of(subscriptions.begin(), subscriptions.end(), predicate);
    };
    if (any([](const auto& s) { return s.answer == Answer::Unanswered; }))
        return window_ended ? State::Inconclusive : State::Pending;
    std::size_t in_range = 0;
    for (const auto& object : delivered_objects(t, collected)) {
        bool candidate = false;
        bool unknown = false;
        bool inside = false;
        for (const auto& subscription : subscriptions) {
            if (subscription.answer != Answer::Established || subscription.alias != object.alias) continue;
            candidate = true;
            if (!subscription.range) unknown = true;
            else if (subscription.range->contains(object.location)) inside = true;
        }
        if (!candidate) continue;  // another track's alias
        if (inside) { ++in_range; continue; }
        if (unknown) continue;
        return State::Fail;
    }
    // A rejected request or an unknowable range leaves its Type unexercised: keep watching the others
    // for a violation until the window ends, then settle without a pass.
    if (any([](const auto& s) { return s.answer == Answer::Rejected || !s.range; }))
        return window_ended ? State::Inconclusive : State::Pending;
    if (!window_ended) return State::Pending;
    return in_range != 0 ? State::Pass : State::Inconclusive;
}

// --------------------------------------------------------------------- FETCH

// One FETCH of the probe: how it was answered and the range it requested.
struct FetchRequest {
    Answer answer{Answer::Unanswered};
    // The part of the requested range that is known: an Object outside it is outside the range. Empty
    // when nothing of it is known; `range_complete` when it is the whole requested range.
    std::optional<Range> range;
    bool range_complete{false};
    // The requested range is empty (the Next Object lies past the Largest Object): no Object is in it.
    bool empty{false};
};

// Sections 3.2 and 9.20.9: the range a filter selects on a FETCH. An omitted end is the Largest Object at
// the time the request was processed, which the FETCH_OK End Location reports (Section 9.12: the
// requested End Location "unless the requested range extends beyond Largest Object").
FetchRequest fetch_request(const d22::LocationFilter& filter, std::optional<Location> end_location) {
    FetchRequest request;
    const Location start{filter.start_group, filter.start_object};
    switch (filter.type) {
    case FilterType::RelativeGroup:
        // {Largest Object.Group + 1 - StartGroup, 0} up to the Largest Object.
        if (end_location) {
            if (auto relative = subscription_range(filter, {true, end_location})) {
                relative->end = end_location;
                request.range = relative;
                request.range_complete = true;
            }
        }
        break;
    case FilterType::Absolute:
    case FilterType::None:
        // The start is known at once, the end once FETCH_OK reports it.
        request.range = Range{filter.type == FilterType::None ? Location{0, 0} : start, end_location};
        request.range_complete = end_location.has_value();
        break;
    case FilterType::AbsoluteBounded:
    case FilterType::AbsoluteRange:
        // Fully given by the request (Objects past the Largest Object are not retrieved at all).
        request.range = subscription_range(filter, {});
        request.range_complete = true;
        break;
    case FilterType::NextObject:
        // Starts after the Largest Object and ends at it: Section 3.2 requires FETCH_ERROR INVALID_RANGE
        // ("Start Location is greater than the Largest Object"), and no Object lies in the range.
        request.empty = true;
        request.range_complete = true;
        break;
    }
    return request;
}

// The FETCH of write `index` with `filter`, as answered on its request stream.
FetchRequest fetch_of(const RawProbeTranscript& t, const Collected& collected, std::size_t index,
                      const d22::LocationFilter& filter) {
    const auto response = response_of(t, collected, index);
    std::optional<Location> end_location;
    auto answer = Answer::Unanswered;
    if (response.stream && !response.messages.malformed) {
        if (!response.messages.complete.empty()) {
            const auto& first = response.messages.complete.front();
            answer = Answer::Rejected;  // REQUEST_ERROR, or anything that is not a FETCH_OK
            if (first.type == kFetchOk) {
                answer = Answer::Established;
                wire::Cursor cursor(first.frame);
                const auto decoded = d21::decode_successful_response(cursor, d21::ResponseContext::Fetch);
                const auto* ok = std::get_if<d21::SuccessfulResponse>(&decoded);
                if (ok && ok->end_location && cursor.remaining() == 0)
                    end_location = Location{ok->end_location->group, ok->end_location->object};
            }
        } else if (response.stream->fin || response.stream->reset) {
            answer = Answer::Rejected;
        }
    }
    auto request = fetch_request(filter, end_location);
    request.answer = answer;
    return request;
}

struct FetchDelivery {
    std::vector<Location> objects;
    bool finished{false};  // FIN or reset: nothing more arrives on it
};

// The FETCH data streams (Section 11.4) the publisher opened after the first request was accepted, by
// the Request ID of their FETCH_HEADER. Only complete Objects are reported; End of Range indicators are
// not Objects. Without GROUP_ORDER the order is Ascending (Section 9.20.8).
std::map<std::uint64_t, FetchDelivery> fetch_deliveries(const RawProbeTranscript& t, const Collected& collected) {
    std::map<std::uint64_t, FetchDelivery> result;
    if (t.writes.empty() || !t.writes.front().delivery_event_count) return result;
    const auto marker = *t.writes.front().delivery_event_count;
    for (const auto& [id, stream] : collected.streams) {
        if ((id & 3u) != 2u || stream.first_event < marker) continue;
        wire::Cursor cursor(stream.bytes);
        if (number(cursor) != kFetchHeader) continue;
        d21::FetchDecoder decoder([](std::uint64_t request) -> std::optional<d21::FetchGroupOrder> {
            for (std::size_t index = 0; index < subscription_count(); ++index)
                if (request == request_id(index)) return d21::FetchGroupOrder::Ascending;
            return std::nullopt;
        });
        const auto decoded = decoder.push(stream.bytes, false);
        if (!decoded.header) continue;
        auto& delivery = result[decoded.header->request_id];
        delivery.finished = delivery.finished || stream.fin || stream.reset;
        for (const auto& event : decoded.events)
            if (const auto* object = std::get_if<d21::ObjectEvent>(&event))
                delivery.objects.push_back({object->group_id, object->object_id});
    }
    return result;
}

// Section 3.3.1 over the probe's FETCHes. Each FETCH has its own data stream naming its Request ID, so
// every Object is attributed exactly and judged at once. A FETCH is settled once rejected (for Next
// Object the required INVALID_RANGE answer) or once its data stream ended; FETCH being finite, a probe
// whose FETCHes all settled needs no more of the window.
State observe_fetches(const RawProbeTranscript& t, const Collected& collected, bool window_ended) {
    const auto filters = draft22_location_range_filters();
    if (t.writes.size() < write_count(Kind::Fetch)) return State::Pending;
    const auto deliveries = fetch_deliveries(t, collected);
    std::size_t in_range = 0;
    bool unexercised = false;
    bool open = false;
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const auto request = fetch_of(t, collected, index, filters[index]);
        const auto found = deliveries.find(request_id(index));
        const auto* delivery = found == deliveries.end() ? nullptr : &found->second;
        if (delivery) {
            for (const auto& object : delivery->objects) {
                if (request.empty) return State::Fail;
                if (!request.range) continue;
                if (!request.range->contains(object)) return State::Fail;
                if (request.range_complete) ++in_range;
            }
        }
        switch (request.answer) {
        case Answer::Unanswered:
            open = true;
            unexercised = true;
            break;
        case Answer::Rejected:
            // The required answer for the empty Next Object range; elsewhere the Type went unexercised.
            if (!request.empty) unexercised = true;
            break;
        case Answer::Established:
            if (!request.range_complete) unexercised = true;
            if (!delivery || !delivery->finished) open = true;
            break;
        }
    }
    if (open && !window_ended) return State::Pending;
    if (unexercised) return State::Inconclusive;
    return in_range != 0 ? State::Pass : State::Inconclusive;
}

State observe(const RawProbeTranscript& t, Kind kind, bool window_ended) {
    if (t.writes.empty() || !t.stimulus_delivered) return State::Pending;
    const auto collected = collect(t.events);
    if (!collected.bounded) return State::Inconclusive;
    if (kind == Kind::Fetch) return observe_fetches(t, collected, window_ended);
    return observe_subscriptions(t, collected, kind, window_ended);
}

// ------------------------------------------------------------------ probes

void require_draft22_wire() {
    if (current_wire_draft() != 22)
        throw std::logic_error("draft 22 location range probes are built on the draft 22 wire only");
}

RawProbeDefinition build(Kind kind, std::chrono::milliseconds deadline, const Fixture& fixture) {
    require_draft22_wire();
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid draft 22 location range fixture or deadline");
    RawProbeDefinition definition;
    definition.setup_bytes = Bytes{std::byte{0xaf}, std::byte{0}, std::byte{0}, std::byte{0}};
    definition.deadline = deadline;
    definition.peer_setup_ready = setup_ready;
    const auto filters = draft22_location_range_filters();
    switch (kind) {
    case Kind::Subscribe:
        definition.id = std::string(kDraft22SubscribeLocationRange);
        for (std::size_t index = 0; index < filters.size(); ++index)
            definition.writes.push_back({RawProbeChannel::NewBidi,
                                         subscribe(request_id(index), fixture, true, &filters[index]), false});
        break;
    case Kind::Update:
        // Section 3.3.1 and 9.5.1: each subscription opens with FORWARD=0 and no filter, so nothing is sent
        // before its REQUEST_UPDATE sets the filter and FORWARD=1 together; every Object seen was sent
        // under the updated filter (the draft 21 counterpart's argument, per subscription).
        definition.id = std::string(kDraft22UpdateLocationRange);
        for (std::size_t index = 0; index < filters.size(); ++index)
            definition.writes.push_back({RawProbeChannel::NewBidi,
                                         subscribe(request_id(index), fixture, false, nullptr), false});
        for (std::size_t index = 0; index < filters.size(); ++index) {
            RawProbeWrite update{RawProbeChannel::NewBidi,
                                 request_update(request_id(filters.size() + index), filters[index]), false, index,
                                 {}};
            update.peer_response_ready = subscribe_ok_ready;
            definition.writes.push_back(std::move(update));
        }
        break;
    case Kind::Fetch:
        // Section 3.3.1: "Fetch requests can also specify a Location filter (see Section 3.2)"; Section
        // 9.20.9 lists FETCH among LOCATION_FILTER's carriers without restricting the Type.
        definition.id = std::string(kDraft22FetchLocationRange);
        for (std::size_t index = 0; index < filters.size(); ++index)
            definition.writes.push_back(
                {RawProbeChannel::NewBidi, fetch(request_id(index), fixture, filters[index]), true});
        break;
    }
    // The whole window is needed unless the evidence already settles the verdict.
    definition.response_ready = [kind](const RawProbeTranscript& t) {
        return observe(t, kind, false) != State::Pending;
    };
    return definition;
}

// Only the track identity is configurable; every other byte is regenerated and compared by the proof.
std::optional<Fixture> recover_fixture(std::span<const std::byte> input, std::uint64_t type) {
    if (input.size() > kMaximumBytes) return {};
    wire::Cursor cursor(input);
    const auto decoded = d21::decode_request_frame(cursor, true);
    const auto* request = std::get_if<d21::RequestFrame>(&decoded);
    if (!request || request->type.type != type || cursor.remaining() != 0) return {};
    wire::Cursor body(request->body);
    const auto id = number(body);
    const auto count = number(body);
    if (id != request_id(0) || !count || *count > 32) return {};
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

std::optional<bool> evaluate(const RawProbeTranscript& t, Kind kind, std::uint64_t first_request_type) {
    // The stimulus is rebuilt on the draft 22 wire; on any other wire nothing is judged.
    if (current_wire_draft() != 22 || t.writes.empty() || t.harness_failed) return {};
    const auto fixture = recover_fixture(t.writes.front().write.bytes, first_request_type);
    if (!fixture) return {};
    const auto expected = build(kind, kRebuildDeadline, *fixture);
    if (t.scenario_id != expected.id) return {};
    // A window probe ends at its deadline, so a timed-out context still carries a full observation.
    if (!t.complete && !t.timed_out) return {};
    RawProbeTranscript prefix = t;
    const auto end = std::find_if(t.events.begin(), t.events.end(), terminal);
    prefix.events.assign(t.events.begin(), end);
    prefix.complete = true;
    prefix.timed_out = false;
    if (!raw_probe_stimulus_valid(prefix, expected)) return {};
    const bool ended = t.timed_out || end != t.events.end();
    const auto state = observe(prefix, kind, ended);
    if (state == State::Pass) return true;
    if (state == State::Fail) return false;
    return {};
}

}  // namespace

std::vector<wire::draft22::LocationFilter> draft22_location_range_filters() {
    return {
        {FilterType::RelativeGroup, 1, 0, std::nullopt, std::nullopt},
        {FilterType::Absolute, kGroup, kObject, std::nullopt, std::nullopt},
        {FilterType::AbsoluteBounded, kGroup, kObject, 0, std::nullopt},
        {FilterType::AbsoluteRange, kGroup, kObject, 0, kObject},
        {FilterType::NextObject, 0, 0, std::nullopt, std::nullopt},
    };
}

RawProbeDefinition draft22_subscribe_location_range_probe(std::chrono::milliseconds deadline,
                                                          std::vector<std::vector<std::byte>> track_namespace,
                                                          std::vector<std::byte> track_name) {
    return build(Kind::Subscribe, deadline, {std::move(track_namespace), std::move(track_name)});
}

RawProbeDefinition draft22_update_location_range_probe(std::chrono::milliseconds deadline,
                                                       std::vector<std::vector<std::byte>> track_namespace,
                                                       std::vector<std::byte> track_name) {
    return build(Kind::Update, deadline, {std::move(track_namespace), std::move(track_name)});
}

RawProbeDefinition draft22_fetch_location_range_probe(std::chrono::milliseconds deadline,
                                                      std::vector<std::vector<std::byte>> track_namespace,
                                                      std::vector<std::byte> track_name) {
    return build(Kind::Fetch, deadline, {std::move(track_namespace), std::move(track_name)});
}

std::optional<bool> evaluate_draft22_fetch_location_range(const RawProbeTranscript& transcript) {
    if (transcript.scenario_id == kDraft22FetchLocationRange) return evaluate(transcript, Kind::Fetch, kFetch);
    return std::nullopt;
}

std::optional<bool> evaluate_draft22_subscription_location_range(const RawProbeTranscript& transcript) {
    if (transcript.scenario_id == kDraft22SubscribeLocationRange)
        return evaluate(transcript, Kind::Subscribe, kSubscribe);
    if (transcript.scenario_id == kDraft22UpdateLocationRange)
        return evaluate(transcript, Kind::Update, kSubscribe);
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
