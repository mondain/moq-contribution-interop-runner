#include "draft18_gap_a_common.h"

#include <algorithm>
#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

// ---- withhold-use-alias-response-while-publisher-retires-token ------------------
// Section 10.2.2: a sender MUST NOT send DELETE for an alias while any message
// using USE_ALIAS with it has not received a response. The runner acknowledges
// the publisher's registering request and then answers nothing else, so any
// later request that used the alias is still unanswered when a DELETE arrives.
const d18::Parameters* parameters_of(const d18::Message& message) {
    return std::visit([](const auto& value) -> const d18::Parameters* {
        if constexpr (requires { value.parameters; }) return &value.parameters;
        else return nullptr;
    }, message);
}

std::vector<d18::Token> tokens_of(const d18::Message& message) {
    std::vector<d18::Token> result;
    const auto* parameters = parameters_of(message);
    if (!parameters) return result;
    for (const auto& parameter : *parameters)
        if (const auto* token = std::get_if<d18::Token>(&parameter.value)) result.push_back(*token);
    return result;
}

bool opens_with_registered_token(std::span<const std::byte> bytes) {
    wire::Cursor cursor(bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message) return false;
    const auto* request_id = std::visit([](const auto& value) -> const std::uint64_t* {
        if constexpr (requires { requires std::same_as<std::remove_cvref_t<decltype(value.request_id)>, std::uint64_t>; })
            return &value.request_id;
        else return nullptr;
    }, *message);
    // The publisher is the client, so its Request IDs are even (Section 10.1).
    if (!request_id || (*request_id & 1u) != 0) return false;
    const auto tokens = tokens_of(*message);
    return std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.alias_type == d18::TokenAliasType::Register;
    });
}

RawProbeDefinition retire_token_definition(const Fixture&, std::chrono::milliseconds deadline) {
    auto definition = make_definition("withhold-use-alias-response-while-publisher-retires-token",
        setup_frame(), deadline);
    definition.peer_request_ready = opens_with_registered_token;
    definition.writes.push_back(make_write(RawProbeChannel::PeerBidi,
        encode(d18::RequestOkMessage{{}, {}})));
    return definition;
}

struct TokenUse {
    transport::StreamId stream;
    std::size_t event;
};

Observation retire_token_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1 || !transcript.writes[0].stream_id ||
        !transcript.writes[0].delivery_event_count) return observation;
    const auto answered_stream = *transcript.writes[0].stream_id;
    const auto answered_at = *transcript.writes[0].delivery_event_count;
    // Replay arrival order so a DELETE is compared with the USE_ALIAS messages
    // that reached the runner before it, on any request stream.
    std::map<transport::StreamId, Bytes> buffers;
    std::map<transport::StreamId, std::size_t> consumed;
    std::map<std::uint64_t, std::vector<TokenUse>> uses;
    std::set<std::uint64_t> registered;
    bool retired_cleanly = false;
    for (std::size_t index = 0; index < transcript.events.size(); ++index) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&transcript.events[index]);
        if (!data || !(is_peer_bidi(data->stream_id))) continue;
        auto& bytes = buffers[data->stream_id];
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
        const auto split = split_frames(bytes);
        auto& done = consumed[data->stream_id];
        const auto first_new = done;
        done = split.frames.size();
        for (std::size_t frame = first_new; frame < split.frames.size(); ++frame) {
            const auto message = decode_frame(bytes, split.frames[frame]);
            if (!message) continue;
            for (const auto& token : tokens_of(*message)) {
                if (!token.alias) continue;
                if (token.alias_type == d18::TokenAliasType::Register) {
                    registered.insert(*token.alias);
                } else if (token.alias_type == d18::TokenAliasType::UseAlias) {
                    uses[*token.alias].push_back({data->stream_id, index});
                } else if (token.alias_type == d18::TokenAliasType::Delete &&
                           registered.contains(*token.alias)) {
                    for (const auto& use : uses[*token.alias]) {
                        // Only the registering request received a response.
                        const bool responded = use.stream == answered_stream && answered_at <= index;
                        if (!responded) {
                            observation.ready = true;
                            observation.result = false;
                            return observation;
                        }
                    }
                    retired_cleanly = true;
                }
            }
        }
    }
    if (retired_cleanly) {
        observation.ready = true;
        observation.result = true;
    }
    return observation;
}

// ---- Section 6.1: SUBSCRIBE_TRACKS with no bidirectional stream credit ---------
// The runner grants the peer a single bidirectional stream, which a publisher
// that announces its namespace spends on PUBLISH_NAMESPACE (the runner answers
// it, as a subscriber must, and keeps the stream open). No credit is left for
// PUBLISH, so a publisher with matching tracks reports PUBLISH_BLOCKED on the
// SUBSCRIBE_TRACKS response stream (draft lines 2223-2233). A credit of zero
// would leave a publisher that announces first unable to start at all.
constexpr std::uint64_t kRequestOk = 0x07;
constexpr std::uint64_t kRequestError = 0x05;
constexpr std::uint64_t kPublishBlocked = 0x0f;
constexpr std::uint64_t kCreditGrant = 8;

// Becomes true once `seen` has held for `window` polls. The controller is
// polled about once per millisecond, and counting polls (rather than reading a
// clock) keeps the period identical under a simulated clock; losing `seen`
// restarts the count.
std::function<bool(const RawProbeTranscript&)> settled_after(
    std::function<bool(const RawProbeTranscript&)> seen, std::chrono::milliseconds window) {
    struct State { std::int64_t streak{0}; std::size_t events{0}; };
    auto state = std::make_shared<State>();
    return [seen = std::move(seen), window, state](const RawProbeTranscript& transcript) {
        // A shorter event log means the definition serves a new session.
        if (transcript.events.size() < state->events) state->streak = 0;
        state->events = transcript.events.size();
        if (!seen(transcript)) { state->streak = 0; return false; }
        return ++state->streak >= window.count();
    };
}
std::chrono::milliseconds quiet_window(std::chrono::milliseconds deadline) {
    return std::clamp(deadline / 4, std::chrono::milliseconds{1}, std::chrono::milliseconds{50});
}

RawProbeDefinition no_credit_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("subscribe-tracks-with-no-bidirectional-stream-credit",
        setup_frame(), deadline);
    definition.initial_peer_bidi_streams = 1; definition.acknowledge_publisher_namespace = true;
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_tracks_request(1, fixture.track_namespace, {forward_parameter(1)})));
    return definition;
}

Observation no_credit_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto* stream = local_stream(*streams, transcript.writes[0]);
    if (!stream) return observation;
    const auto split = split_frames(stream->bytes);
    if (split.malformed || split.frames.empty()) return observation;
    // Section 6.1: the first message on the response stream is the single
    // REQUEST_OK or REQUEST_ERROR, whatever PUBLISH_BLOCKED follows.
    observation.ready = true;
    observation.result = split.frames.front().type == kRequestOk ||
                         split.frames.front().type == kRequestError;
    return observation;
}

// ---- restore-bidi-stream-credit-after-publish-blocked --------------------------
// Section 6.1: once PUBLISH_BLOCKED was sent for a Track, the publisher MUST
// NOT send PUBLISH for it. After the publisher blocked, the runner restores
// bidirectional credit while SUBSCRIBE_TRACKS stays active, so a publisher that
// then opens PUBLISH for the blocked Track breaks the rule.
struct Blocked {
    Namespace track_namespace;
    Bytes track_name;
    std::size_t event{0};
};

// Index of the event on which each complete frame of `stream` finished.
std::vector<std::size_t> frame_events(std::span<const transport::TransportEvent> events,
                                      transport::StreamId stream) {
    std::vector<std::size_t> result;
    Bytes bytes;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&events[index]);
        if (!data || data->stream_id != stream) continue;
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
        const auto split = split_frames(bytes);
        while (result.size() < split.frames.size()) result.push_back(index);
    }
    return result;
}

std::optional<Blocked> publish_blocked(std::span<const transport::TransportEvent> events,
                                       const Streams& streams, const RawProbeAcceptedWrite& write,
                                       const Namespace& prefix) {
    const auto* stream = local_stream(streams, write);
    if (!stream) return std::nullopt;
    const auto split = split_frames(stream->bytes);
    if (split.malformed) return std::nullopt;
    const auto completed = frame_events(events, *write.stream_id);
    for (std::size_t index = 0; index < split.frames.size() && index < completed.size(); ++index) {
        if (split.frames[index].type != kPublishBlocked) continue;
        const auto message = decode_frame(stream->bytes, split.frames[index]);
        const auto* blocked = message ? std::get_if<d18::PublishBlockedMessage>(&*message) : nullptr;
        if (!blocked) continue;
        Blocked result{prefix, blocked->track_name.bytes, completed[index]};
        for (const auto& field : blocked->track_namespace_suffix.fields) result.track_namespace.push_back(field);
        return result;
    }
    return std::nullopt;
}

Namespace prefix_of(const RawProbeAcceptedWrite& write) {
    wire::Cursor cursor(write.write.bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    const auto* tracks = message ? std::get_if<d18::SubscribeTracksMessage>(message) : nullptr;
    return tracks ? tracks->track_namespace_prefix.fields : Namespace{};
}

Observation restore_credit_observe(const RawProbeTranscript& transcript, const Fixture& fixture);

RawProbeDefinition restore_credit_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("restore-bidi-stream-credit-after-publish-blocked", setup_frame(), deadline);
    definition.initial_peer_bidi_streams = 1; definition.acknowledge_publisher_namespace = true;
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_tracks_request(1, fixture.track_namespace, {forward_parameter(1)})));
    auto credit = make_write(RawProbeChannel::Credit, {});
    credit.application_error = kCreditGrant;
    credit.evidence_ready = [](const RawProbeGateInput& input) {
        if (input.prior_writes.empty()) return false;
        const auto streams = collect_streams(input.events);
        return streams && publish_blocked(input.events, *streams, input.prior_writes[0],
                                          prefix_of(input.prior_writes[0])).has_value();
    };
    definition.writes.push_back(std::move(credit));
    // A violation ends the context at once; otherwise the publisher gets a
    // quiet period after the credit was granted to open the forbidden PUBLISH.
    auto settled = settled_after([](const RawProbeTranscript& transcript) {
        return transcript.writes.size() == 2 && transcript.writes[1].delivery_event_count.has_value();
    }, quiet_window(deadline));
    definition.response_ready = [settled, fixture](const RawProbeTranscript& transcript) {
        const auto observed = restore_credit_observe(transcript, fixture);
        return observed.result == false || settled(transcript);
    };
    return definition;
}

Observation restore_credit_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 2 || !transcript.writes[1].delivery_event_count) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto blocked = publish_blocked(transcript.events, *streams, transcript.writes[0],
                                         prefix_of(transcript.writes[0]));
    if (!blocked) return observation;
    for (const auto& [id, stream] : *streams) {
        if (!is_peer_bidi(id) || stream.first_event < blocked->event) continue;
        const auto split = split_frames(stream.bytes);
        if (split.malformed || split.frames.empty() || split.frames.front().type != kPublish) continue;
        const auto message = decode_frame(stream.bytes, split.frames.front());
        const auto* publish = message ? std::get_if<d18::PublishMessage>(&*message) : nullptr;
        if (publish && publish->track_namespace.fields == blocked->track_namespace &&
            publish->track_name.bytes == blocked->track_name) {
            observation.ready = true;
            observation.result = false;
            return observation;
        }
    }
    observation.result = true;
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
    result.push_back({"D18-10-2-2-MUST-NOT-002", "withhold-use-alias-response-while-publisher-retires-token",
        "token-delete-not-sent-before-all-use-alias-responses", false, false, std::nullopt,
        retire_token_definition, retire_token_observe});
    result.push_back({"D18-6-1-MUST-004", "subscribe-tracks-with-no-bidirectional-stream-credit",
        "track-subscription-response-precedes-publish-blocked", true, false, FirstWrite::Prefix,
        no_credit_definition, no_credit_observe});
    result.push_back({"D18-6-1-MUST-NOT-001", "restore-bidi-stream-credit-after-publish-blocked",
        "no-publish-for-blocked-track-after-credit-restored", true, false, FirstWrite::Prefix,
        restore_credit_definition, restore_credit_observe});
    return result;
}

}  // namespace moq::interop::scenarios::gap_b_detail

namespace moq::interop::scenarios::gap_a {
std::vector<Entry> gap_b_entries() { return gap_b_detail::entries(); }
}  // namespace moq::interop::scenarios::gap_a
