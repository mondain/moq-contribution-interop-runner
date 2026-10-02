#include "draft18_gap_a_common.h"

#include <algorithm>
#include <string_view>

// Draft-18 gap slice B: publisher-observed obligations that need either a
// passive observation of publisher-initiated messages or two retrievals of
// one Object. Every verdict is derived from the draft text cited per entry.
namespace moq::interop::scenarios::gap_b_detail {
using namespace gap_a;
namespace {

constexpr std::uint64_t kTrackStatus = 0x0D;
constexpr std::uint64_t kPublish = 0x1d;
constexpr std::string_view kUnknownParameterDetail = "unknown message parameter";
// The fixture track is expected to contain Group 7, Object 9 (README, FETCH
// first-object profiles), as the other draft-18 Object comparison probes do.
constexpr d18::Location kTargetObject{7, 9};

bool is_target(const d18::ObjectEvent& object) {
    return object.group_id == kTargetObject.group && object.object_id == kTargetObject.object &&
           object.status.value_or(0) == 0 && object.payload_length == object.retained_payload.size();
}

// ---- publisher-queries-track-status-before-resuming-publication -----------------
// Section 10 (Table 5): TRACK_STATUS is a "First" message of a request stream.
// Recovery queries are optional publisher behaviour (Section 2.4.3), so a
// session in which the publisher never sends one stays unscored.
RawProbeDefinition track_status_placement_definition(const Fixture&, std::chrono::milliseconds deadline) {
    auto definition = make_definition("publisher-queries-track-status-before-resuming-publication",
        setup_frame(), deadline);
    definition.acknowledge_publisher_namespace = true;
    return definition;
}

Observation track_status_placement_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    bool first_message = false;
    for (const auto& [id, stream] : *streams) {
        const auto split = split_frames(stream.bytes);
        if (split.malformed) continue;
        for (std::size_t index = 0; index < split.frames.size(); ++index) {
            if (split.frames[index].type != kTrackStatus) continue;
            // Only a publisher-initiated bidirectional stream may open with it.
            if (is_peer_bidi(id) && index == 0) first_message = true;
            else {
                observation.ready = true;
                observation.result = false;
                return observation;
            }
        }
    }
    if (first_message) {
        observation.ready = true;
        observation.result = true;
    }
    return observation;
}

// ---- publish-with-and-without-parameter-extension-negotiation -------------------
// Section 10.2: every Message Parameter must be defined by the negotiated
// version or negotiated through Setup Options. The runner's SETUP carries no
// option, so no extension is negotiated and an undefined type is a violation.
RawProbeDefinition parameter_negotiation_definition(const Fixture& fixture,
                                                    std::chrono::milliseconds deadline) {
    auto definition = make_definition("publish-with-and-without-parameter-extension-negotiation",
        setup_frame(), deadline);
    definition.acknowledge_publisher_namespace = true;
    // SUBSCRIBE_OK (Section 10.8) and TRACK_STATUS_OK (Section 10.14) carry the
    // parameters a publisher sets about the track, such as LARGEST_OBJECT.
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_request(1, fixture, {forward_parameter(1)})));
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, track_status_request(3, fixture), true));
    return definition;
}

const d18::Parameters* message_parameters(const d18::Message& message) {
    if (const auto* value = std::get_if<d18::RequestOkMessage>(&message)) return &value->parameters;
    if (const auto* value = std::get_if<d18::PublishMessage>(&message)) return &value->parameters;
    if (const auto* value = std::get_if<d18::SubscribeOkMessage>(&message)) return &value->parameters;
    return nullptr;
}

Observation parameter_negotiation_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    std::vector<std::pair<const Bytes*, Frame>> frames;
    std::size_t answered = 0;
    for (const auto& write : transcript.writes) {
        const auto* stream = local_stream(*streams, write);
        if (!stream) continue;
        const auto split = split_frames(stream->bytes);
        if (split.malformed || split.frames.empty()) continue;
        frames.emplace_back(&stream->bytes, split.frames.front());
        ++answered;
    }
    for (const auto& [id, stream] : *streams) {
        if (!is_peer_bidi(id)) continue;
        const auto split = split_frames(stream.bytes);
        if (split.malformed || split.frames.empty() || split.frames.front().type != kPublish) continue;
        frames.emplace_back(&stream.bytes, split.frames.front());
    }
    bool parameters_seen = false;
    for (const auto& [bytes, frame] : frames) {
        if (const auto message = decode_frame(*bytes, frame)) {
            const auto* parameters = message_parameters(*message);
            if (parameters && !parameters->empty()) parameters_seen = true;
            continue;
        }
        const auto detail = frame_decode_error(*bytes, frame);
        if (detail && *detail == kUnknownParameterDetail) {
            observation.ready = true;
            observation.result = false;
            return observation;
        }
    }
    // A pass needs at least one parameter that was actually sent and defined.
    if (parameters_seen) {
        observation.ready = true;
        observation.result = true;
    } else if (answered == transcript.writes.size() || application_close_code(transcript.events)) {
        observation.ready = true;
    }
    return observation;
}

// ---- retrieve-same-object-with-different-subscribe-publish-ok-and-fetch-parameters
// Section 10.2: SUBSCRIBE, PUBLISH_OK and FETCH parameters must not change the
// payload of an Object (Section 2.4.3 uniqueness). Two FETCHes of the same
// Object differ only in SUBSCRIBER_PRIORITY and GROUP_ORDER, and a SUBSCRIBE
// asking for the same start Location differs in the same parameters.
RawProbeDefinition payload_identity_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("retrieve-same-object-with-different-subscribe-publish-ok-and-fetch-parameters",
        setup_frame(), deadline);
    definition.acknowledge_publisher_namespace = true;
    const auto fetch = [&](std::uint64_t request_id, std::uint8_t priority, std::uint8_t order) {
        d18::Parameters parameters{priority_parameter(priority), {0x22, d18::Uint8ParameterValue{order}}};
        return encode(d18::FetchMessage{request_id,
            d18::StandaloneFetch{track_namespace(fixture.track_namespace), {fixture.track_name},
                                 kTargetObject, {kTargetObject.group, kTargetObject.object + 1}},
            std::move(parameters)});
    };
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, fetch(1, 0, 1), true));
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, fetch(3, 255, 2), true));
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, subscribe_request(5, fixture,
        {forward_parameter(1), priority_parameter(128),
         filter_parameter({d18::SubscriptionFilterType::AbsoluteStart, kTargetObject, std::nullopt}),
         {0x22, d18::Uint8ParameterValue{2}}})));
    return definition;
}

bool fetch_finished(const Streams& streams, const RawProbeAcceptedWrite& write) {
    if (fetched_object(streams, write)) return true;
    const auto* stream = local_stream(streams, write);
    if (!stream) return false;
    if (stream->fin || stream->reset) return true;
    const auto messages = stream_messages(*stream);
    const auto* first = first_response(messages);
    return first && std::holds_alternative<d18::RequestErrorMessage>(*first);
}

Observation payload_identity_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 3) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    std::vector<Bytes> payloads;
    for (std::size_t index : {0u, 1u}) {
        if (const auto object = fetched_object(*streams, transcript.writes[index]))
            payloads.push_back(object->retained_payload);
    }
    const auto messages = messages_of(*streams, transcript.writes[2]);
    const auto* first = messages ? first_response(*messages) : nullptr;
    if (const auto* ok = first ? std::get_if<d18::SubscribeOkMessage>(first) : nullptr) {
        for (const auto& stream : subgroup_streams(*streams, ok->track_alias))
            for (const auto& object : stream.objects)
                if (is_target(object)) payloads.push_back(object.retained_payload);
    }
    // Two retrievals with different parameters are required to compare.
    if (payloads.size() >= 2) {
        observation.ready = true;
        observation.result = std::all_of(payloads.begin(), payloads.end(),
            [&](const auto& payload) { return payload == payloads.front(); });
        return observation;
    }
    const bool subscribe_answered = first != nullptr;
    if (fetch_finished(*streams, transcript.writes[0]) && fetch_finished(*streams, transcript.writes[1]) &&
        subscribe_answered)
        observation.ready = true;
    return observation;
}

}  // namespace

std::vector<Entry> entries() {
    std::vector<Entry> result;
    result.push_back({"D18-10-MUST-004", "publisher-queries-track-status-before-resuming-publication",
        "request-is-first-message-on-new-bidirectional-stream", false, false, std::nullopt,
        track_status_placement_definition, track_status_placement_observe});
    result.push_back({"D18-10-2-MUST-003", "publish-with-and-without-parameter-extension-negotiation",
        "sent-message-parameters-defined-or-negotiated", true, false, FirstWrite::Subscribe,
        parameter_negotiation_definition, parameter_negotiation_observe});
    result.push_back({"D18-10-2-MUST-NOT-002",
        "retrieve-same-object-with-different-subscribe-publish-ok-and-fetch-parameters",
        "same-object-payload-independent-of-message-parameters", true, false, FirstWrite::Fetch,
        payload_identity_definition, payload_identity_observe});
    return result;
}

}  // namespace moq::interop::scenarios::gap_b_detail

namespace moq::interop::scenarios::gap_a {
std::vector<Entry> gap_b_entries() { return gap_b_detail::entries(); }
}  // namespace moq::interop::scenarios::gap_a
