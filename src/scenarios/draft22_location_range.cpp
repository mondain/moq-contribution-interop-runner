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
constexpr std::uint64_t kSubscribe = 0x03;
constexpr std::uint64_t kSubscribeOk = 0x04;
constexpr std::uint64_t kLargestObject = 0x09;
constexpr std::uint64_t kForward = 0x10;
constexpr std::uint64_t kLocationFilter = 0x21;

enum class Kind { Subscribe };

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

// The subscription opened by write `index` with `filter`.
Subscription subscription_of(const RawProbeTranscript& t, const Collected& collected, std::size_t index,
                             const d22::LocationFilter& filter) {
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
    result.answer = Answer::Established;
    result.alias = *alias;
    result.range = subscription_range(filter, largest_of(first, d21::ResponseContext::Subscribe));
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

std::size_t request_count(Kind) { return draft22_location_range_filters().size(); }

// Section 3.3.1 over every subscription of the probe. Subscriptions to one Track may share a Track
// Alias (Section 3.1: "the subscriber re-applies each subscription's filter to determine which
// subscription a received Object belongs to"), so an Object is outside the
// requested range only when it fits none of the subscriptions that carry its alias. No failure is
// claimed while any subscription is unanswered (its alias is unknown) or for an Object whose alias is
// shared with a subscription whose range cannot be known.
State observe(const RawProbeTranscript& t, Kind kind, bool window_ended) {
    if (t.writes.empty() || !t.stimulus_delivered) return State::Pending;
    const auto collected = collect(t.events);
    if (!collected.bounded) return State::Inconclusive;
    const auto filters = draft22_location_range_filters();
    const auto count = request_count(kind);
    if (t.writes.size() < count) return State::Pending;
    std::vector<Subscription> subscriptions;
    for (std::size_t index = 0; index < count; ++index)
        subscriptions.push_back(subscription_of(t, collected, index, filters[index]));
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

std::optional<bool> evaluate_draft22_subscription_location_range(const RawProbeTranscript& transcript) {
    if (transcript.scenario_id == kDraft22SubscribeLocationRange)
        return evaluate(transcript, Kind::Subscribe, kSubscribe);
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
