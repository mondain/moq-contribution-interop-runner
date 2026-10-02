#include "draft18_gap_a_common.h"

#include <algorithm>

namespace moq::interop::scenarios::gap_a {
namespace {

constexpr std::uint64_t kRequestError = 0x05;
constexpr std::uint64_t kRequestOk = 0x07;
constexpr std::uint64_t kPublishNamespace = 0x06;
constexpr std::uint64_t kPublish = 0x1d;
constexpr std::uint64_t kDuplicateSubscription = 0x19;

// Fields of the Track Namespace at the start of a PUBLISH payload, read from
// the raw frame so that a zero-length field is reported rather than rejected.
struct NamespaceFields {
    bool complete{false};
    bool all_nonempty{true};
    std::size_t count{0};
};

NamespaceFields publish_namespace_fields(std::span<const std::byte> bytes, const Frame& frame) {
    NamespaceFields result;
    if (frame.offset > bytes.size() || frame.size > bytes.size() - frame.offset) return result;
    wire::Cursor cursor(bytes.subspan(frame.offset, frame.size));
    if (!std::holds_alternative<std::uint64_t>(wire::read_vi64(cursor))) return result;
    if (!std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor, 2))) return result;
    // PUBLISH and PUBLISH_NAMESPACE both start with Request ID, Track Namespace.
    if (!std::holds_alternative<std::uint64_t>(wire::read_vi64(cursor))) return result;
    const auto count = wire::read_vi64(cursor);
    const auto* fields = std::get_if<std::uint64_t>(&count);
    if (!fields || *fields > 32) return result;
    for (std::uint64_t i = 0; i < *fields; ++i) {
        const auto length = wire::read_vi64(cursor);
        const auto* size = std::get_if<std::uint64_t>(&length);
        if (!size || *size > 4096) return result;
        if (*size == 0) result.all_nonempty = false;
        if (!std::holds_alternative<std::span<const std::byte>>(
                wire::read_bytes(cursor, static_cast<std::size_t>(*size)))) return result;
    }
    result.count = static_cast<std::size_t>(*fields);
    result.complete = true;
    return result;
}

Namespace without_last_field(const Namespace& fields) {
    Namespace result = fields;
    if (!result.empty()) result.pop_back();
    return result;
}

bool has_namespace_with_suffix(const StreamMessages& messages, const Namespace& suffix) {
    return std::any_of(messages.messages.begin(), messages.messages.end(), [&](const auto& message) {
        const auto* value = std::get_if<d18::NamespaceMessage>(&message);
        return value && value->track_namespace_suffix.fields == suffix;
    });
}

// ---- subscribe-namespace-at-publisher-with-matching-namespace ----------------
RawProbeDefinition namespace_first_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("subscribe-namespace-at-publisher-with-matching-namespace",
        setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_namespace_request(1, fixture.track_namespace)));
    return definition;
}

Observation namespace_first_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto* stream = local_stream(*streams, transcript.writes[0]);
    if (!stream) return observation;
    const auto split = split_frames(stream->bytes);
    if (split.malformed) { observation.ready = true; return observation; }
    if (!split.frames.empty()) {
        // Section 6.1 and 10.18: REQUEST_OK or REQUEST_ERROR must come first,
        // before any NAMESPACE or NAMESPACE_DONE.
        observation.ready = true;
        observation.result = split.frames.front().type == kRequestOk ||
                             split.frames.front().type == kRequestError;
    } else if (stream->fin || stream->reset) {
        observation.ready = true;
    }
    return observation;
}

// ---- discover-authoritative-publisher-namespace-by-exact-and-prefix ----------
// The second subscription's prefix may not overlap the first (Section 10.18),
// so the exact-match request is finished (FIN) before the prefix one is sent.
bool exact_namespace_announced(const RawProbeGateInput& input) {
    if (input.prior_writes.empty()) return false;
    const auto streams = collect_streams(input.events);
    if (!streams) return false;
    const auto messages = messages_of(*streams, input.prior_writes[0]);
    if (!messages || messages->malformed || messages->undecodable || messages->types.empty() ||
        messages->types.front() != kRequestOk) return false;
    return has_namespace_with_suffix(*messages, {});
}

RawProbeDefinition exact_and_prefix_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition(
        "discover-authoritative-publisher-namespace-by-exact-and-prefix-subscriptions",
        setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_namespace_request(1, fixture.track_namespace)));
    auto finish = make_write(RawProbeChannel::NewBidi, {}, true);
    finish.reuse_write_stream = 0;
    finish.evidence_ready = exact_namespace_announced;
    definition.writes.push_back(std::move(finish));
    auto prefix = make_write(RawProbeChannel::NewBidi,
        subscribe_namespace_request(3, without_last_field(fixture.track_namespace)));
    prefix.evidence_ready = [](const RawProbeGateInput& input) {
        return input.prior_writes.size() == 2 && input.prior_writes[1].fin_accepted &&
               input.prior_writes[1].delivery_event_count.has_value();
    };
    definition.writes.push_back(std::move(prefix));
    return definition;
}

Observation exact_and_prefix_observe(const RawProbeTranscript& transcript, const Fixture& fixture) {
    Observation observation;
    if (transcript.writes.size() != 3) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto exact = messages_of(*streams, transcript.writes[0]);
    const auto prefix = messages_of(*streams, transcript.writes[2]);
    if (!exact || !prefix) return observation;
    const auto* prefix_stream = local_stream(*streams, transcript.writes[2]);
    const Namespace suffix{fixture.track_namespace.back()};
    const bool exact_ok = !exact->malformed && !exact->undecodable && !exact->types.empty() &&
        exact->types.front() == kRequestOk && has_namespace_with_suffix(*exact, {});
    const bool prefix_ok = !prefix->malformed && !prefix->undecodable && !prefix->types.empty() &&
        prefix->types.front() == kRequestOk && has_namespace_with_suffix(*prefix, suffix);
    if (exact_ok && prefix_ok) {
        observation.ready = true;
        observation.result = true;
        return observation;
    }
    // A rejected or closed second request leaves the obligation unobserved.
    const bool prefix_rejected = !prefix->types.empty() && prefix->types.front() == kRequestError;
    if (prefix_rejected || (prefix_stream && (prefix_stream->fin || prefix_stream->reset)))
        observation.ready = true;
    return observation;
}

// ---- publish-track-namespace-fields -------------------------------------------
// FORWARD 0 keeps the resulting PUBLISH subscriptions free of Object data, so
// only control messages are examined (Section 10.19).
RawProbeDefinition tracks_definition(std::string id, const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition(std::move(id), setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_tracks_request(1, fixture.track_namespace, {forward_parameter(0)})));
    return definition;
}

RawProbeDefinition namespace_fields_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    return tracks_definition("publish-track-namespace-fields", fixture, deadline);
}

Observation namespace_fields_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    bool observed = false;
    for (const auto& [id, stream] : *streams) {
        if (!is_peer_bidi(id)) continue;
        const auto split = split_frames(stream.bytes);
        if (split.malformed || split.frames.empty() || split.frames.front().type != kPublish) continue;
        const auto fields = publish_namespace_fields(stream.bytes, split.frames.front());
        if (!fields.complete) continue;
        observed = true;
        if (!fields.all_nonempty) {
            observation.ready = true;
            observation.result = false;
            return observation;
        }
    }
    if (observed) { observation.ready = true; observation.result = true; }
    return observation;
}

// ---- initiate-track-publication -----------------------------------------------
RawProbeDefinition track_publication_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    return tracks_definition("initiate-track-publication", fixture, deadline);
}

struct Placement {
    bool first_message{false};
    bool misplaced{false};
};

// Where a message of `type` appears: first on a peer-initiated request stream
// is correct (Table 5); anywhere else is a placement violation.
Placement placement_of(const Streams& streams, std::uint64_t type) {
    Placement result;
    for (const auto& [id, stream] : streams) {
        const auto split = split_frames(stream.bytes);
        if (split.malformed) continue;
        for (std::size_t index = 0; index < split.frames.size(); ++index) {
            if (split.frames[index].type != type) continue;
            if (is_peer_bidi(id) && index == 0) result.first_message = true;
            else result.misplaced = true;
        }
    }
    return result;
}

Observation placement_observe(const RawProbeTranscript& transcript, std::uint64_t type) {
    Observation observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto placement = placement_of(*streams, type);
    if (placement.misplaced) {
        observation.ready = true;
        observation.result = false;
    } else if (placement.first_message) {
        observation.ready = true;
        observation.result = true;
    }
    return observation;
}

Observation track_publication_observe(const RawProbeTranscript& transcript, const Fixture&) {
    return placement_observe(transcript, kPublish);
}

// ---- initiate-namespace-publication / publish-namespace-for-relay-... -------
RawProbeDefinition namespace_publication_definition(const Fixture&, std::chrono::milliseconds deadline) {
    return make_definition("initiate-namespace-publication", setup_frame(), deadline);
}

Observation namespace_publication_observe(const RawProbeTranscript& transcript, const Fixture&) {
    return placement_observe(transcript, kPublishNamespace);
}

RawProbeDefinition routing_definition(const Fixture&, std::chrono::milliseconds deadline) {
    return make_definition("publish-namespace-for-relay-subscription-routing", setup_frame(), deadline);
}

Observation routing_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    for (const auto& [id, stream] : *streams) {
        if (!is_peer_bidi(id)) continue;
        const auto split = split_frames(stream.bytes);
        if (split.malformed || split.frames.empty() || split.frames.front().type != kPublishNamespace) continue;
        const auto message = decode_frame(stream.bytes, split.frames.front());
        if (message && std::holds_alternative<d18::PublishNamespaceMessage>(*message)) {
            // An explicit PUBLISH_NAMESPACE on its own request stream is the
            // routing request; a PUBLISH for one Track would not be.
            observation.ready = true;
            observation.result = true;
            return observation;
        }
    }
    return observation;
}

// ---- receive-subscribe-before-outstanding-publish-response -------------------
std::optional<Bytes> subscribe_for_pending_publish(const RawProbeGateInput& input) {
    const auto streams = collect_streams(input.events);
    if (!streams) return std::nullopt;
    for (const auto& [id, stream] : *streams) {
        if (!is_peer_bidi(id)) continue;
        const auto split = split_frames(stream.bytes);
        if (split.malformed || split.frames.empty() || split.frames.front().type != kPublish) continue;
        const auto message = decode_frame(stream.bytes, split.frames.front());
        const auto* publish = message ? std::get_if<d18::PublishMessage>(&*message) : nullptr;
        if (!publish) continue;
        return subscribe_request(3, {publish->track_namespace.fields, publish->track_name.bytes});
    }
    return std::nullopt;
}

RawProbeDefinition pending_publish_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = tracks_definition("receive-subscribe-before-outstanding-publish-response",
        fixture, deadline);
    RawProbeWrite subscribe;
    subscribe.channel = RawProbeChannel::NewBidi;
    subscribe.prepare_bytes = subscribe_for_pending_publish;
    definition.writes.push_back(std::move(subscribe));
    return definition;
}

Observation pending_publish_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto messages = messages_of(*streams, transcript.writes[1]);
    if (!messages || messages->malformed || messages->undecodable) return observation;
    const auto* first = first_response(*messages);
    if (!first) return observation;
    // No PUBLISH_OK was sent, so the publisher's subscription is still
    // Pending and the crossed SUBSCRIBE must fail with DUPLICATE_SUBSCRIPTION.
    if (const auto* error = std::get_if<d18::RequestErrorMessage>(first)) {
        observation.ready = true;
        observation.result = error->error_code == kDuplicateSubscription;
    } else if (std::holds_alternative<d18::SubscribeOkMessage>(*first)) {
        observation.ready = true;
        observation.result = false;
    }
    return observation;
}

}  // namespace

std::vector<Entry> discovery_entries() {
    std::vector<Entry> entries;
    entries.push_back({"D18-6-1-MUST-002", "subscribe-namespace-at-publisher-with-matching-namespace",
        "namespace-subscription-response-precedes-namespace-messages", true, false, FirstWrite::Prefix,
        namespace_first_definition, namespace_first_observe});
    entries.push_back({"D18-6-2-MUST-001",
        "discover-authoritative-publisher-namespace-by-exact-and-prefix-subscriptions",
        "namespace-message-sent-to-each-matching-subscriber", true, false, FirstWrite::Prefix,
        exact_and_prefix_definition, exact_and_prefix_observe});
    entries.push_back({"D18-2-4-1-MUST-001", "publish-track-namespace-fields", "namespace-fields-nonempty",
        true, false, FirstWrite::Prefix, namespace_fields_definition, namespace_fields_observe});
    entries.push_back({"D18-10-MUST-002", "initiate-track-publication",
        "request-is-first-message-on-new-bidirectional-stream", true, false, FirstWrite::Prefix,
        track_publication_definition, track_publication_observe});
    entries.push_back({"D18-10-MUST-005", "initiate-namespace-publication",
        "request-is-first-message-on-new-bidirectional-stream", false, false, std::nullopt,
        namespace_publication_definition, namespace_publication_observe});
    entries.push_back({"D18-9-5-MUST-003", "publish-namespace-for-relay-subscription-routing",
        "explicit-publish-namespace-sent", false, false, std::nullopt,
        routing_definition, routing_observe});
    entries.push_back({"D18-5-1-MUST-005", "receive-subscribe-before-outstanding-publish-response",
        "duplicate-subscription-rejected", true, false, FirstWrite::Prefix,
        pending_publish_definition, pending_publish_observe});
    return entries;
}

}  // namespace moq::interop::scenarios::gap_a
