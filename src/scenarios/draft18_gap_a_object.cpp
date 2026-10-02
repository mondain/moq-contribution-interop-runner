#include "draft18_gap_a_common.h"

#include <algorithm>
#include <set>
#include <string_view>

namespace moq::interop::scenarios::gap_a {
namespace {

constexpr std::string_view kUnknownTrack = "interop-unknown-78937ae9d2abc4e0b6c1";
constexpr std::uint64_t kPublish = 0x1d;
// The fixture track is expected to contain Group 7, Object 9 (docs/scenario-reference.md, FETCH
// first-object profiles); retrieving it proves Objects have been published.
constexpr d18::Location kFetchStart{7, 9};
constexpr d18::Location kFetchEnd{7, 10};

Bytes text(std::string_view value) {
    Bytes result;
    for (const auto c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

bool has_largest(const d18::Parameters& parameters) { return largest_object(parameters).has_value(); }

RawProbeWrite known_fetch(std::uint64_t request_id, const Fixture& fixture) {
    return make_write(RawProbeChannel::NewBidi,
        standalone_fetch_request(request_id, fixture, kFetchStart, kFetchEnd), true);
}

bool object_published(const RawProbeGateInput& input) {
    return !input.prior_writes.empty() && fetch_object_observed(input.prior_writes[0], input.events);
}

RawProbeWrite after_published_object(RawProbeWrite write) {
    write.evidence_ready = object_published;
    return write;
}

const d18::Message* message_at(const StreamMessages& messages, std::size_t index) {
    if (messages.malformed || messages.undecodable || index >= messages.messages.size()) return nullptr;
    return &messages.messages[index];
}

// ---- retrieve-same-object-at-distinct-times -----------------------------------
RawProbeDefinition repeat_fetch_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("retrieve-same-object-at-distinct-times", setup_frame(), deadline);
    definition.writes.push_back(known_fetch(1, fixture));
    definition.writes.push_back(after_published_object(known_fetch(3, fixture)));
    return definition;
}

Observation repeat_fetch_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto first = fetched_object(*streams, transcript.writes[0]);
    const auto second = fetched_object(*streams, transcript.writes[1]);
    if (!first || !second) return observation;
    // Section 2.1: the same Object identity must carry identical bytes however
    // and whenever it is retrieved; the second FETCH starts after the first
    // delivered the Object.
    observation.ready = true;
    observation.result = first->retained_payload == second->retained_payload;
    return observation;
}

// ---- LARGEST_OBJECT after observed publication ---------------------------------
RawProbeDefinition largest_subscribe_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("subscribe-to-track-after-observed-object-publication",
        setup_frame(), deadline);
    definition.writes.push_back(known_fetch(1, fixture));
    definition.writes.push_back(after_published_object(
        make_write(RawProbeChannel::NewBidi, subscribe_request(3, fixture))));
    return definition;
}

Observation largest_subscribe_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto messages = messages_of(*streams, transcript.writes[1]);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    if (!first) return observation;
    if (const auto* ok = std::get_if<d18::SubscribeOkMessage>(first)) {
        observation.ready = true;
        observation.result = has_largest(ok->parameters);
    } else if (std::holds_alternative<d18::RequestErrorMessage>(*first)) {
        observation.ready = true;
    }
    return observation;
}

RawProbeDefinition largest_publish_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("publish-existing-track-after-observed-object-publication",
        setup_frame(), deadline);
    definition.writes.push_back(known_fetch(1, fixture));
    definition.writes.push_back(after_published_object(
        make_write(RawProbeChannel::NewBidi, subscribe_tracks_request(3, fixture.track_namespace, {forward_parameter(0)}))));
    return definition;
}

Observation largest_publish_observe(const RawProbeTranscript& transcript, const Fixture& fixture) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    for (const auto& [id, stream] : *streams) {
        if (!is_peer_bidi(id)) continue;
        const auto split = split_frames(stream.bytes);
        if (split.malformed || split.frames.empty() || split.frames.front().type != kPublish) continue;
        const auto message = decode_frame(stream.bytes, split.frames.front());
        const auto* publish = message ? std::get_if<d18::PublishMessage>(&*message) : nullptr;
        if (!publish || publish->track_namespace.fields != fixture.track_namespace ||
            publish->track_name.bytes != fixture.track_name) continue;
        observation.ready = true;
        observation.result = has_largest(publish->parameters);
        return observation;
    }
    return observation;
}

bool subscription_established(const RawProbeGateInput& input) {
    if (input.prior_writes.size() < 2) return false;
    const auto streams = collect_streams(input.events);
    if (!streams) return false;
    const auto messages = messages_of(*streams, input.prior_writes[1]);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    return first && std::holds_alternative<d18::SubscribeOkMessage>(*first);
}

RawProbeDefinition largest_update_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("accepted-subscription-update-after-observed-object-publication",
        setup_frame(), deadline);
    definition.writes.push_back(known_fetch(1, fixture));
    // Forward 0 keeps the subscription quiet; the update only changes priority.
    definition.writes.push_back(after_published_object(make_write(RawProbeChannel::NewBidi,
        subscribe_request(3, fixture, {forward_parameter(0)}))));
    auto update = make_write(RawProbeChannel::NewBidi, request_update(5, {priority_parameter(100)}));
    update.reuse_write_stream = 1;
    update.evidence_ready = subscription_established;
    definition.writes.push_back(std::move(update));
    return definition;
}

Observation largest_update_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 3) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto messages = messages_of(*streams, transcript.writes[2]);
    if (!messages) return observation;
    const auto* update = message_at(*messages, 1);
    if (!update) return observation;
    if (const auto* ok = std::get_if<d18::RequestOkMessage>(update)) {
        observation.ready = true;
        observation.result = has_largest(ok->parameters);
    } else if (std::holds_alternative<d18::RequestErrorMessage>(*update)) {
        observation.ready = true;
    }
    return observation;
}

RawProbeDefinition largest_status_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("accepted-track-status-after-observed-object-publication",
        setup_frame(), deadline);
    definition.writes.push_back(known_fetch(1, fixture));
    auto status = make_write(RawProbeChannel::NewBidi, track_status_request(3, fixture), true);
    definition.writes.push_back(after_published_object(std::move(status)));
    return definition;
}

Observation largest_status_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto messages = messages_of(*streams, transcript.writes[1]);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    if (!first) return observation;
    if (const auto* ok = std::get_if<d18::RequestOkMessage>(first)) {
        observation.ready = true;
        observation.result = has_largest(ok->parameters);
    } else if (std::holds_alternative<d18::RequestErrorMessage>(*first)) {
        observation.ready = true;
    }
    return observation;
}

// ---- subscription Object delivery shape -----------------------------------------
struct Established {
    std::uint64_t alias{0};
    std::optional<d18::Location> largest;
};

std::optional<Established> established_subscription(const Streams& streams, const RawProbeAcceptedWrite& write) {
    const auto messages = messages_of(streams, write);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    const auto* ok = first ? std::get_if<d18::SubscribeOkMessage>(first) : nullptr;
    if (!ok) return std::nullopt;
    return Established{ok->track_alias, largest_object(ok->parameters)};
}

d18::Parameters next_group_filter() {
    return {filter_parameter({d18::SubscriptionFilterType::NextGroupStart, std::nullopt, std::nullopt})};
}

// Streams that begin at or after the filter's start group: every Subgroup in
// them is delivered from its first Object.
std::vector<SubgroupStream> new_group_streams(const Streams& streams, const Established& subscription) {
    std::vector<SubgroupStream> selected;
    for (auto& stream : subgroup_streams(streams, subscription.alias)) {
        if (stream.decode_error) continue;
        if (subscription.largest && stream.header.group_id <= subscription.largest->group) continue;
        selected.push_back(std::move(stream));
    }
    return selected;
}

RawProbeDefinition first_object_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("publish-new-subgroup", setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_request(1, fixture, next_group_filter())));
    return definition;
}

Observation first_object_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto subscription = established_subscription(*streams, transcript.writes[0]);
    if (!subscription) return observation;
    const auto selected = new_group_streams(*streams, *subscription);
    if (selected.empty()) return observation;
    // Section 2.2: the Original Publisher sets FIRST_OBJECT (0x40) when it
    // opens a new Subgroup.
    observation.ready = true;
    observation.result = std::all_of(selected.begin(), selected.end(),
        [](const auto& stream) { return stream.header.first_object; });
    return observation;
}

struct ObjectRange {
    std::uint64_t first{0};
    std::uint64_t last{0};
};

std::optional<ObjectRange> object_range(const SubgroupStream& stream) {
    if (stream.objects.empty()) return std::nullopt;
    ObjectRange range{stream.objects.front().object_id, stream.objects.front().object_id};
    for (const auto& object : stream.objects) {
        range.first = std::min(range.first, object.object_id);
        range.last = std::max(range.last, object.object_id);
    }
    return range;
}

std::optional<std::uint64_t> subgroup_key(const SubgroupStream& stream) {
    // Mode 0b01 derives the Subgroup ID from the first Object ID, so two
    // streams cannot be matched to one Subgroup from their headers alone.
    const auto mode = (stream.header.raw_type & 0x06u) >> 1u;
    if (mode == 0) return 0;
    if (mode == 2) return stream.header.subgroup_id;
    return std::nullopt;
}

RawProbeDefinition one_stream_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("subscribe-to-subgroup-without-reset-or-upstream-reordering",
        setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_request(1, fixture, next_group_filter())));
    return definition;
}

Observation one_stream_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto subscription = established_subscription(*streams, transcript.writes[0]);
    if (!subscription) return observation;
    const auto selected = new_group_streams(*streams, *subscription);
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<const SubgroupStream*>> by_subgroup;
    std::map<std::uint64_t, std::vector<const SubgroupStream*>> by_group;
    for (const auto& stream : selected) {
        by_group[stream.header.group_id].push_back(&stream);
        if (const auto key = subgroup_key(stream))
            by_subgroup[{stream.header.group_id, *key}].push_back(&stream);
    }
    for (const auto& [key, group_streams] : by_subgroup) {
        for (std::size_t later = 1; later < group_streams.size(); ++later) {
            for (std::size_t earlier = 0; earlier < later; ++earlier) {
                const auto& a = *group_streams[earlier];
                const auto& b = *group_streams[later];
                // A premature reset explains a second stream.
                if (a.reset || b.reset) continue;
                const auto a_range = object_range(a);
                const auto b_range = object_range(b);
                if (!a_range || !b_range) continue;
                // Object IDs that interleave or run backwards are the
                // out-of-order exception; an in-order continuation is not.
                if (b_range->first > a_range->last) {
                    observation.ready = true;
                    observation.result = false;
                    return observation;
                }
            }
        }
    }
    // Decide only after a whole Group has been delivered and closed.
    if (by_group.size() < 2) return observation;
    const auto& lowest = by_group.begin()->second;
    const bool closed = std::all_of(lowest.begin(), lowest.end(),
        [](const auto* stream) { return stream->fin || stream->reset; });
    if (closed) {
        observation.ready = true;
        observation.result = true;
    }
    return observation;
}

// ---- publisher-rejects-subscribe-request -----------------------------------------
RawProbeDefinition rejected_subscribe_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("publisher-rejects-subscribe-request", setup_frame(), deadline);
    const Fixture missing{fixture.track_namespace, text(kUnknownTrack)};
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, subscribe_request(1, missing)));
    return definition;
}

bool carries_objects(const Streams& streams, const std::optional<PeerControl>& control, std::size_t marker) {
    for (const auto& [id, stream] : streams) {
        if (!is_peer_uni(id) || (control && id == control->stream_id) || stream.first_event < marker ||
            stream.bytes.empty()) continue;
        wire::Cursor cursor(stream.bytes);
        const auto type = wire::read_vi64(cursor);
        const auto* raw = std::get_if<std::uint64_t>(&type);
        if (raw && ((*raw < 128 && (*raw & 0x10u) != 0) || *raw == 0x5)) return true;
    }
    return false;
}

Observation rejected_subscribe_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1 || !transcript.writes[0].delivery_event_count) return observation;
    const auto marker = *transcript.writes[0].delivery_event_count;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto messages = messages_of(*streams, transcript.writes[0]);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    if (!first) return observation;
    if (!std::holds_alternative<d18::RequestErrorMessage>(*first)) {
        // An accepted SUBSCRIBE says nothing about rejected ones.
        observation.ready = true;
        return observation;
    }
    observation.ready = true;
    const bool datagrams = std::any_of(transcript.events.begin() + static_cast<std::ptrdiff_t>(marker),
        transcript.events.end(), [](const auto& event) {
            return std::holds_alternative<transport::DatagramEvent>(event);
        });
    // Section 5.1.1: Objects MUST NOT be sent for requests that end in error.
    observation.result = !datagrams && !carries_objects(*streams, peer_control(*streams), marker);
    return observation;
}

// ---- publish-objects-before-within-and-after-subscription-range -----------------
std::optional<Bytes> range_subscribe(const RawProbeGateInput& input, const Fixture& fixture) {
    if (input.prior_writes.empty()) return std::nullopt;
    const auto streams = collect_streams(input.events);
    if (!streams) return std::nullopt;
    const auto messages = messages_of(*streams, input.prior_writes[0]);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    const auto* ok = first ? std::get_if<d18::RequestOkMessage>(first) : nullptr;
    if (!ok) return std::nullopt;
    const auto largest = largest_object(ok->parameters);
    if (!largest || largest->group == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    // Exactly the Group after the Largest Object: earlier Locations are
    // before the range and later Groups are after it.
    return subscribe_request(3, fixture, {filter_parameter({d18::SubscriptionFilterType::AbsoluteRange,
        d18::Location{largest->group + 1, 0}, std::uint64_t{0}})});
}

RawProbeDefinition range_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("publish-objects-before-within-and-after-subscription-range",
        setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, track_status_request(1, fixture), true));
    RawProbeWrite subscribe;
    subscribe.channel = RawProbeChannel::NewBidi;
    subscribe.prepare_bytes = [fixture](const RawProbeGateInput& input) { return range_subscribe(input, fixture); };
    definition.writes.push_back(std::move(subscribe));
    return definition;
}

Observation range_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto status = messages_of(*streams, transcript.writes[0]);
    const auto* status_first = status ? message_at(*status, 0) : nullptr;
    const auto* status_ok = status_first ? std::get_if<d18::RequestOkMessage>(status_first) : nullptr;
    const auto largest = status_ok ? largest_object(status_ok->parameters) : std::nullopt;
    const auto subscription = established_subscription(*streams, transcript.writes[1]);
    if (!largest) return observation;
    const auto response = messages_of(*streams, transcript.writes[1]);
    const auto* response_first = response ? message_at(*response, 0) : nullptr;
    if (response_first && std::holds_alternative<d18::RequestErrorMessage>(*response_first)) {
        observation.ready = true;
        return observation;
    }
    if (!subscription) return observation;
    const auto in_range = [&](std::uint64_t group) { return group == largest->group + 1; };
    std::size_t inside = 0;
    bool outside = false;
    for (const auto& stream : subgroup_streams(*streams, subscription->alias)) {
        if (!in_range(stream.header.group_id) && !stream.objects.empty()) outside = true;
        else inside += stream.objects.size();
    }
    for (const auto& event : transcript.events) {
        const auto* datagram = std::get_if<transport::DatagramEvent>(&event);
        if (!datagram) continue;
        const auto decoded = d18::decode_datagram(datagram->data, {});
        const auto* object = std::get_if<d18::ObjectEvent>(&decoded);
        if (!object || object->track_alias != subscription->alias) continue;
        if (in_range(object->group_id)) ++inside;
        else outside = true;
    }
    // Section 5.1.2: a publisher MUST NOT send Objects outside the range.
    if (outside) {
        observation.ready = true;
        observation.result = false;
    } else if (inside > 0) {
        observation.ready = true;
        observation.result = true;
    }
    return observation;
}

// ---- joining-fetch-after-forward-enabled-and-track-advanced ---------------------
// A far-future AbsoluteStart keeps Objects off the subscription (the saved
// Joining Location does not depend on the filter), so no Object payload can
// accumulate while the track advances.
constexpr std::uint64_t kFarFutureGroup = std::uint64_t{1} << 40;

struct JoiningState {
    d18::Location joining;
};

std::optional<JoiningState> joining_state(const Streams& streams, const RawProbeAcceptedWrite& subscribe) {
    const auto messages = messages_of(streams, subscribe);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    if (!first || !std::holds_alternative<d18::SubscribeOkMessage>(*first)) return std::nullopt;
    const auto* update = message_at(*messages, 1);
    const auto* ok = update ? std::get_if<d18::RequestOkMessage>(update) : nullptr;
    const auto joining = ok ? largest_object(ok->parameters) : std::nullopt;
    if (!joining) return std::nullopt;
    return JoiningState{*joining};
}

bool forward_enabled_gate(const RawProbeGateInput& input) {
    if (input.prior_writes.empty()) return false;
    const auto streams = collect_streams(input.events);
    if (!streams) return false;
    const auto messages = messages_of(*streams, input.prior_writes[0]);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    return first && std::holds_alternative<d18::SubscribeOkMessage>(*first);
}

bool joining_location_saved(const RawProbeGateInput& input) {
    if (input.prior_writes.empty()) return false;
    const auto streams = collect_streams(input.events);
    return streams && joining_state(*streams, input.prior_writes[0]).has_value();
}

std::optional<d18::Location> status_largest(const Streams& streams, const RawProbeAcceptedWrite& status) {
    const auto messages = messages_of(streams, status);
    const auto* first = messages ? message_at(*messages, 0) : nullptr;
    const auto* ok = first ? std::get_if<d18::RequestOkMessage>(first) : nullptr;
    return ok ? largest_object(ok->parameters) : std::nullopt;
}

bool track_advanced(const RawProbeGateInput& input) {
    if (input.prior_writes.size() != 3) return false;
    const auto streams = collect_streams(input.events);
    if (!streams) return false;
    const auto state = joining_state(*streams, input.prior_writes[0]);
    const auto current = status_largest(*streams, input.prior_writes[2]);
    return state && current && location_less(state->joining, *current);
}

RawProbeDefinition joining_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("joining-fetch-after-forward-enabled-and-track-advanced",
        setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_request(1, fixture, {forward_parameter(0),
            filter_parameter({d18::SubscriptionFilterType::AbsoluteStart,
                              d18::Location{kFarFutureGroup, 0}, std::nullopt})})));
    // REQUEST_UPDATE_OK to this update communicates the Joining Location
    // (Section 5.1): the Largest Location when Forward State becomes 1.
    auto update = make_write(RawProbeChannel::NewBidi, request_update(3, {forward_parameter(1)}));
    update.reuse_write_stream = 0;
    update.evidence_ready = forward_enabled_gate;
    definition.writes.push_back(std::move(update));
    auto status = make_write(RawProbeChannel::NewBidi, track_status_request(5, fixture), true);
    status.evidence_ready = joining_location_saved;
    definition.writes.push_back(std::move(status));
    auto fetch = make_write(RawProbeChannel::NewBidi, joining_fetch_request(7, 1, 0, true), true);
    fetch.evidence_ready = track_advanced;
    definition.writes.push_back(std::move(fetch));
    return definition;
}

Observation joining_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 4) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto state = joining_state(*streams, transcript.writes[0]);
    if (!state) return observation;
    const auto response = messages_of(*streams, transcript.writes[3]);
    const auto* first = response ? message_at(*response, 0) : nullptr;
    if (!first) return observation;
    if (std::holds_alternative<d18::RequestErrorMessage>(*first)) {
        observation.ready = true;
        return observation;
    }
    const auto* ok = std::get_if<d18::FetchOkMessage>(first);
    if (!ok) return observation;
    observation.ready = true;
    // Section 10.12.2.1: End Location = {Joining Location.Group,
    // Joining Location.Object + 1}, although the track has since advanced.
    const d18::Location expected{state->joining.group, state->joining.object + 1};
    observation.result = ok->end_location == expected;
    return observation;
}

}  // namespace

std::vector<Entry> object_entries() {
    std::vector<Entry> entries;
    entries.push_back({"D18-2-1-MUST-NOT-001", "retrieve-same-object-at-distinct-times",
        "object-payload-immutable", true, false, FirstWrite::Fetch,
        repeat_fetch_definition, repeat_fetch_observe});
    entries.push_back({"D18-10-2-11-MUST-001", "subscribe-to-track-after-observed-object-publication",
        "largest-object-included-after-object-publication", true, false, FirstWrite::Fetch,
        largest_subscribe_definition, largest_subscribe_observe});
    entries.push_back({"D18-10-2-11-MUST-002", "publish-existing-track-after-observed-object-publication",
        "largest-object-included-after-object-publication", true, false, FirstWrite::Fetch,
        largest_publish_definition, largest_publish_observe});
    entries.push_back({"D18-10-2-11-MUST-003", "accepted-subscription-update-after-observed-object-publication",
        "request-ok-to-request-update-includes-largest-object", true, false, FirstWrite::Fetch,
        largest_update_definition, largest_update_observe});
    entries.push_back({"D18-10-2-11-MUST-004", "accepted-track-status-after-observed-object-publication",
        "request-ok-to-track-status-includes-largest-object", true, false, FirstWrite::Fetch,
        largest_status_definition, largest_status_observe});
    entries.push_back({"D18-2-2-MUST-001", "publish-new-subgroup", "new-subgroup-first-object-bit-set",
        true, false, FirstWrite::Subscribe, first_object_definition, first_object_observe});
    entries.push_back({"D18-2-2-MUST-NOT-002", "subscribe-to-subgroup-without-reset-or-upstream-reordering",
        "subgroup-uses-one-subscription-stream", true, false, FirstWrite::Subscribe,
        one_stream_definition, one_stream_observe});
    entries.push_back({"D18-5-1-1-MUST-NOT-002", "publisher-rejects-subscribe-request",
        "no-objects-sent-for-rejected-subscribe", true, false, FirstWrite::Subscribe,
        rejected_subscribe_definition, rejected_subscribe_observe});
    entries.push_back({"D18-5-1-2-MUST-NOT-001", "publish-objects-before-within-and-after-subscription-range",
        "all-delivered-objects-within-subscription-range", true, false, FirstWrite::TrackStatus,
        range_definition, range_observe});
    entries.push_back({"D18-5-1-MUST-003", "joining-fetch-after-forward-enabled-and-track-advanced",
        "joining-fetch-ends-at-saved-joining-location", true, false, FirstWrite::Subscribe,
        joining_definition, joining_observe});
    return entries;
}

}  // namespace moq::interop::scenarios::gap_a
