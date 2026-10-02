#include "draft18_contribution_internal.h"

#include <limits>

// Probes that watch what a publisher itself originates: the requests it opens
// (PUBLISH, PUBLISH_NAMESPACE, ...), the Key-Value-Pairs it writes, the order
// in which it closes data streams and ends a subscription, and the Properties
// it puts on Objects. The runner only accepts the requests it is asked to
// accept; every verdict comes from bytes the publisher wrote.
namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kSubscribeId = 1;
constexpr std::uint64_t kAcceptedRequests = 8;
constexpr std::uint64_t kPropertyImmutable = 0x0B;
// Section 2.5.1: Mandatory Track Property types.
constexpr std::uint64_t kMandatoryFirst = 0x4000;
constexpr std::uint64_t kMandatoryLast = 0x7FFF;
constexpr std::string_view kSessionNamespace = ".session";
constexpr std::string_view kSinglePeriod = ".";

enum class Origin {
    Publish,
    PublishNamespace,
    TrackStatus,
    Subscribe,
    Fetch,
    SubscribeNamespace,
    SubscribeTracks,
};

// A request the publisher opened on its own bidirectional stream.
struct Originated {
    Origin origin{Origin::Publish};
    Namespace fields;
    std::optional<Bytes> track_name;
    std::optional<std::uint64_t> alias;
};

std::optional<Originated> describe(const d18::Message& message) {
    if (const auto* publish = std::get_if<d18::PublishMessage>(&message))
        return Originated{Origin::Publish, publish->track_namespace.fields, publish->track_name.bytes,
                          publish->track_alias};
    if (const auto* announce = std::get_if<d18::PublishNamespaceMessage>(&message))
        return Originated{Origin::PublishNamespace, announce->track_namespace.fields, std::nullopt, std::nullopt};
    if (const auto* status = std::get_if<d18::TrackStatusMessage>(&message))
        return Originated{Origin::TrackStatus, status->track_namespace.fields, status->track_name.bytes,
                          std::nullopt};
    if (const auto* subscribe = std::get_if<d18::SubscribeMessage>(&message))
        return Originated{Origin::Subscribe, subscribe->track_namespace.fields, subscribe->track_name.bytes,
                          std::nullopt};
    if (const auto* fetch = std::get_if<d18::FetchMessage>(&message))
        if (const auto* standalone = std::get_if<d18::StandaloneFetch>(&fetch->fetch))
            return Originated{Origin::Fetch, standalone->track_namespace.fields, standalone->track_name.bytes,
                              std::nullopt};
    if (const auto* discovery = std::get_if<d18::SubscribeNamespaceMessage>(&message))
        return Originated{Origin::SubscribeNamespace, discovery->track_namespace_prefix.fields, std::nullopt,
                          std::nullopt};
    if (const auto* tracks = std::get_if<d18::SubscribeTracksMessage>(&message))
        return Originated{Origin::SubscribeTracks, tracks->track_namespace_prefix.fields, std::nullopt,
                          std::nullopt};
    return std::nullopt;
}

// Requests opened by the publisher: the first message on each peer-opened
// bidirectional stream, in the order the streams first carried data.
std::vector<Originated> originated_requests(const RawProbeTranscript& transcript) {
    std::vector<Originated> result;
    std::set<transport::StreamId> seen;
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 3u) != 0u || !seen.insert(data->stream_id).second) continue;
        const auto reply = stream_reply(transcript, data->stream_id);
        if (reply.messages.empty()) continue;
        if (auto request = describe(reply.messages.front())) result.push_back(std::move(*request));
    }
    return result;
}

bool first_field_is(const Namespace& fields, std::string_view value) {
    return !fields.empty() && fields.front() == text(value);
}
bool first_field_starts_with_period(const Namespace& fields) {
    return !fields.empty() && !fields.front().empty() && fields.front().front() == std::byte{'.'};
}

using Scope = bool (*)(const Originated&);

bool any_origin(const Originated&) { return true; }
bool is_publish(const Originated& request) { return request.origin == Origin::Publish; }
bool is_announcement(const Originated& request) { return request.origin == Origin::PublishNamespace; }
bool is_publication(const Originated& request) { return is_publish(request) || is_announcement(request); }

bool single_period(const Originated& request) { return first_field_is(request.fields, kSinglePeriod); }
bool session_namespace(const Originated& request) { return first_field_is(request.fields, kSessionNamespace); }
// Section 3.2.1: every period-prefixed namespace is reserved. Section 3.2.2
// gives .session its meaning, so only the others are unregistered.
bool unregistered_reserved(const Originated& request) {
    return first_field_starts_with_period(request.fields) && !session_namespace(request);
}

bool in_scope_seen(const RawProbeTranscript& transcript, Scope scope) {
    const auto all = originated_requests(transcript);
    return std::any_of(all.begin(), all.end(), [scope](const auto& request) { return scope(request); });
}

// Violation in the publisher's own requests. A run needs at least one request
// inside the rule's scope; none leaves the row unscored.
std::optional<bool> originated_not(const RawProbeTranscript& transcript, Scope scope, Scope violates) {
    if (!bounded(transcript)) return std::nullopt;
    bool exercised = false;
    for (const auto& request : originated_requests(transcript)) {
        if (!scope(request)) continue;
        exercised = true;
        if (violates(request)) return false;
    }
    return exercised ? std::optional<bool>{true} : std::nullopt;
}

// The runner negotiates no extension (its SETUP carries no options), so a
// .session track or namespace on the wire is the application's, never a
// transport-managed one that section 3.2.2 permits.
Observe originated_rule(Scope scope, Scope violates) {
    return [scope, violates](const RawProbeTranscript& transcript, bool) {
        return originated_not(transcript, scope, violates);
    };
}

// SETUP framing only: the Type, the 16-bit length and that many bytes. The
// Setup Options are not validated, so a publisher whose Key-Value-Pairs break
// the draft is observed rather than hidden behind the harness decoder.
bool setup_frame_complete(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto type = wire::read_vi64(cursor);
    const auto* value = std::get_if<std::uint64_t>(&type);
    if (!value || *value != kSetupType) return false;
    const auto high = wire::read_bytes(cursor, 2);
    const auto* length_bytes = std::get_if<std::span<const std::byte>>(&high);
    if (!length_bytes) return false;
    const std::size_t length = (std::to_integer<std::size_t>((*length_bytes)[0]) << 8u) |
                               std::to_integer<std::size_t>((*length_bytes)[1]);
    return cursor.remaining() >= length;
}

RawProbeDefinition accepting_definition(const char* id, std::vector<RawProbeWrite> writes,
                                        std::function<bool(const RawProbeTranscript&)> ready,
                                        std::chrono::milliseconds deadline, bool settle = true,
                                        std::function<bool(std::span<const std::byte>)> peer_setup = setup_ready) {
    RawProbeDefinition definition{id, setup_message({}), std::move(writes), true, std::move(peer_setup), deadline,
                                  settle ? settled_after(std::move(ready), quiet_window(deadline)) : std::move(ready), {}};
    definition.auto_accept_ready = publisher_opener<d18::PublishMessage, d18::PublishNamespaceMessage>;
    definition.auto_accept_reply = ok_response();
    definition.auto_accept_limit = kAcceptedRequests;
    return definition;
}

RawProbeDefinition observe_originated(const char* id, Scope scope, std::chrono::milliseconds deadline) {
    return accepting_definition(id, {},
        [scope](const RawProbeTranscript& transcript) { return in_scope_seen(transcript, scope); }, deadline);
}

// Section 1.4.3: the sum of the previous Type and the Delta Type is compared
// without wrapping, independently of how a receiver would treat it.
enum class Sum { Fits, Overflows, Unreadable };
struct KeyValueScan {
    Sum result{Sum::Fits};
    std::size_t pairs{0};
};
KeyValueScan scan_key_value_types(std::span<const std::byte> payload) {
    KeyValueScan scan;
    wire::Cursor cursor(payload);
    std::uint64_t previous = 0;
    while (cursor.remaining() != 0) {
        const auto delta = wire::read_vi64(cursor);
        const auto* value = std::get_if<std::uint64_t>(&delta);
        if (!value) return {Sum::Unreadable, scan.pairs};
        if (*value > std::numeric_limits<std::uint64_t>::max() - previous) return {Sum::Overflows, scan.pairs};
        previous += *value;
        ++scan.pairs;
        if ((previous & 1u) == 0u) {
            if (!std::holds_alternative<std::uint64_t>(wire::read_vi64(cursor))) return {Sum::Unreadable, scan.pairs};
        } else {
            const auto length = wire::read_vi64(cursor);
            const auto* size = std::get_if<std::uint64_t>(&length);
            if (!size || *size > cursor.remaining()) return {Sum::Unreadable, scan.pairs};
            if (!std::holds_alternative<std::span<const std::byte>>(
                    wire::read_bytes(cursor, static_cast<std::size_t>(*size))))
                return {Sum::Unreadable, scan.pairs};
        }
    }
    return scan;
}

// The Setup Option block of the publisher's SETUP, taken from its control
// stream before any decoder validates the Key-Value-Pairs.
std::optional<Bytes> setup_option_block(const RawProbeTranscript& transcript) {
    std::map<transport::StreamId, Bytes> streams;
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 3u) != 2u) continue;
        auto& bytes = streams[data->stream_id];
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
    }
    for (const auto& [id, bytes] : streams) {
        (void)id;
        wire::Cursor cursor(bytes);
        const auto type = wire::read_vi64(cursor);
        const auto* value = std::get_if<std::uint64_t>(&type);
        if (!value || *value != kSetupType) continue;
        const auto high = wire::read_bytes(cursor, 2);
        const auto* length_bytes = std::get_if<std::span<const std::byte>>(&high);
        if (!length_bytes) continue;
        const std::size_t length = (std::to_integer<std::size_t>((*length_bytes)[0]) << 8u) |
                                   std::to_integer<std::size_t>((*length_bytes)[1]);
        if (cursor.remaining() < length) continue;
        const auto payload = wire::read_bytes(cursor, length);
        const auto* block = std::get_if<std::span<const std::byte>>(&payload);
        if (block) return Bytes(block->begin(), block->end());
    }
    return std::nullopt;
}

// Key-Value-Pair lists the publisher wrote on request streams: Parameters and
// Track Properties. Their Types are summed by the harness decoder, which
// reports an overflow as a protocol violation naming the resolved type.
struct ListEvidence {
    bool overflow{false};
    bool exercised{false};
};
template <class Message>
std::size_t longest_list(const Message& message) {
    std::size_t longest = 0;
    if constexpr (requires { message.parameters.size(); }) longest = std::max(longest, message.parameters.size());
    if constexpr (requires { message.track_properties.entries.size(); })
        longest = std::max(longest, message.track_properties.entries.size());
    return longest;
}
ListEvidence request_stream_lists(const RawProbeTranscript& transcript) {
    ListEvidence evidence;
    std::map<transport::StreamId, Bytes> streams;
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 2u) != 0u) continue;
        auto& bytes = streams[data->stream_id];
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
    }
    for (const auto& [id, bytes] : streams) {
        (void)id;
        wire::Cursor cursor(bytes);
        while (cursor.remaining() != 0) {
            const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
            if (const auto* message = std::get_if<d18::Message>(&decoded)) {
                if (std::visit([](const auto& value) { return longest_list(value); }, *message) >= 2)
                    evidence.exercised = true;
                continue;
            }
            const auto* error = std::get_if<wire::DecodeError>(&decoded);
            if (error && error->code == wire::DecodeErrorCode::ProtocolViolation &&
                error->detail.find("resolved type overflows") != std::string::npos)
                evidence.overflow = true;
            break;
        }
    }
    return evidence;
}

// Section 1.4.3. Verdicts cover the Setup Options (scanned from the raw
// bytes) and the Parameters and Track Properties of the publisher's request
// stream messages; Object Properties are outside this probe.
std::optional<bool> key_value_type_sums_fit(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    bool exercised = false;
    if (const auto block = setup_option_block(transcript)) {
        const auto scan = scan_key_value_types(*block);
        if (scan.result == Sum::Overflows) return false;
        // A sum only exists once a second pair follows the first.
        exercised = scan.result == Sum::Fits && scan.pairs >= 2;
    }
    const auto lists = request_stream_lists(transcript);
    if (lists.overflow) return false;
    return exercised || lists.exercised ? std::optional<bool>{true} : std::nullopt;
}

RawProbeWrite forwarding_subscribe(const Fixture& fixture) {
    return {RawProbeChannel::NewBidi, subscribe_request(fixture, kSubscribeId, {forward_parameter(1)}), false};
}

// Section 5.1.1: the stream carrying the subscription's PUBLISH_DONE.
std::optional<std::size_t> publish_done_event(const RawProbeTranscript& transcript) {
    const auto stream = write_stream(transcript, 0);
    if (!stream) return std::nullopt;
    Bytes bytes;
    for (std::size_t i = write_marker(transcript, 0); i < transcript.events.size(); ++i) {
        if (terminal(transcript.events[i])) break;
        const auto* data = std::get_if<transport::StreamDataEvent>(&transcript.events[i]);
        if (!data || data->stream_id != *stream) continue;
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
        wire::Cursor cursor(bytes);
        while (cursor.remaining() != 0) {
            const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
            const auto* message = std::get_if<d18::Message>(&decoded);
            if (!message) break;
            if (std::holds_alternative<d18::PublishDoneMessage>(*message)) return i;
        }
    }
    return std::nullopt;
}

bool publish_done_seen(const RawProbeTranscript& transcript) {
    return subscription_alias(transcript) && publish_done_event(transcript);
}

// Every Object stream of the subscription ended (FIN or reset) before the
// publisher's PUBLISH_DONE reached the runner.
bool streams_ended_before_publish_done(const RawProbeTranscript& transcript) {
    const auto alias = subscription_alias(transcript);
    const auto done = publish_done_event(transcript);
    if (!alias || !done) return false;
    const auto streams = subscription_streams(transcript, *alias);
    return !streams.empty() && std::all_of(streams.begin(), streams.end(), [&](const auto& stream) {
        return !stream.malformed && stream.closed_event && *stream.closed_event < *done;
    });
}

// Section 5.1.1: the publisher MUST NOT send PUBLISH_DONE until it has closed
// all related streams. QUIC orders bytes within one stream only, so a stream
// whose FIN or first byte reaches the runner after PUBLISH_DONE does not show
// that PUBLISH_DONE was sent first (section 10.11 gives PUBLISH_DONE a Stream
// Count because it can overtake the data streams). Only two outcomes are
// therefore decisive: every stream ended before PUBLISH_DONE arrived, or a
// stream never ended at all although PUBLISH_DONE and the whole observation
// window had passed.
std::optional<bool> streams_closed_before_publish_done(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto alias = subscription_alias(transcript);
    const auto done = publish_done_event(transcript);
    if (!alias || !done) return std::nullopt;
    const auto streams = subscription_streams(transcript, *alias);
    if (streams.empty()) return std::nullopt;
    bool never_ended = false;
    bool ended_late = false;
    for (const auto& stream : streams) {
        if (stream.malformed) return std::nullopt;
        if (!stream.closed_event) never_ended = true;
        else if (*stream.closed_event > *done) ended_late = true;
    }
    if (!never_ended && !ended_late) return true;
    if (ended_late || peer_close(transcript)) return std::nullopt;
    return false;
}

// Section 2.5.1: a Mandatory Track Property is malformed as an Object Property.
// Object Properties are searched together with those nested in the Immutable
// Properties Property (section 12.7).
bool has_mandatory_property(const d18::KeyValuePairs& properties) {
    for (const auto& property : properties) {
        if (property.type >= kMandatoryFirst && property.type <= kMandatoryLast) return true;
        if (property.type != kPropertyImmutable) continue;
        const auto* nested = std::get_if<d18::ByteValue>(&property.value);
        if (!nested) continue;
        wire::Cursor cursor(nested->bytes);
        const auto decoded = d18::decode_key_value_pairs(cursor, nested->bytes.size(), {});
        if (const auto* inner = std::get_if<d18::KeyValuePairs>(&decoded))
            if (has_mandatory_property(*inner)) return true;
    }
    return false;
}

std::vector<d18::ObjectEvent> subscribed_objects(const RawProbeTranscript& transcript) {
    const auto alias = subscription_alias(transcript);
    return alias ? alias_objects(transcript, *alias) : std::vector<d18::ObjectEvent>{};
}

std::optional<bool> mandatory_property_only_at_track_scope(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto objects = subscribed_objects(transcript);
    if (objects.empty()) return std::nullopt;
    for (const auto& object : objects)
        if (has_mandatory_property(object.properties)) return false;
    return true;
}

// Section 2.4.3: Tracks that hold different data at the same time must differ
// in name or namespace. Tracks come from the runner's subscription and from
// the publisher's own PUBLISH requests; each is identified by its Full Track Name.
struct ObservedTrack {
    Namespace fields;
    Bytes name;
    std::vector<d18::ObjectEvent> objects;
};

// The track named by the runner's SUBSCRIBE, the first write of the probe.
std::optional<Fixture> subscribed_fixture(const RawProbeTranscript& transcript) {
    if (transcript.writes.empty()) return std::nullopt;
    wire::Cursor cursor(transcript.writes.front().write.bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    const auto* subscribe = message ? std::get_if<d18::SubscribeMessage>(message) : nullptr;
    if (!subscribe) return std::nullopt;
    return Fixture{subscribe->track_namespace.fields, subscribe->track_name.bytes};
}

std::vector<ObservedTrack> observed_tracks(const RawProbeTranscript& transcript) {
    std::vector<ObservedTrack> result;
    if (const auto fixture = subscribed_fixture(transcript)) {
        ObservedTrack track{fixture->track_namespace, fixture->track_name, subscribed_objects(transcript)};
        if (!track.objects.empty()) result.push_back(std::move(track));
    }
    for (const auto& request : originated_requests(transcript)) {
        if (request.origin != Origin::Publish || !request.alias || !request.track_name) continue;
        ObservedTrack track{request.fields, *request.track_name, alias_objects(transcript, *request.alias)};
        if (!track.objects.empty()) result.push_back(std::move(track));
    }
    return result;
}

bool same_object(const d18::ObjectEvent& left, const d18::ObjectEvent& right) {
    return left.payload_length == right.payload_length && left.retained_payload == right.retained_payload &&
           left.status == right.status;
}
// Whether the two Tracks carry different data at some shared location.
bool differ_at_shared_location(const ObservedTrack& left, const ObservedTrack& right) {
    for (const auto& a : left.objects)
        for (const auto& b : right.objects)
            if (a.group_id == b.group_id && a.object_id == b.object_id && !same_object(a, b)) return true;
    return false;
}
bool same_full_name(const ObservedTrack& left, const ObservedTrack& right) {
    return left.fields == right.fields && left.name == right.name;
}

std::optional<bool> distinct_content_distinct_names(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto tracks = observed_tracks(transcript);
    bool distinct_content = false;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        for (std::size_t j = i + 1; j < tracks.size(); ++j) {
            if (!differ_at_shared_location(tracks[i], tracks[j])) continue;
            if (same_full_name(tracks[i], tracks[j])) return false;
            distinct_content = true;
        }
    }
    return distinct_content ? std::optional<bool>{true} : std::nullopt;
}
bool two_tracks_observed(const RawProbeTranscript& transcript) { return observed_tracks(transcript).size() >= 2; }

}  // namespace

std::vector<Draft18ContributionProbe> origination_probes(std::chrono::milliseconds deadline, const Fixture& fixture) {
    std::vector<Draft18ContributionProbe> result;
    const auto add_originated = [&](const char* requirement, const char* evaluator, const char* scenario,
                                    Scope scope, Scope violates) {
        result.push_back(make_probe(requirement, evaluator, observe_originated(scenario, scope, deadline),
                                    originated_rule(scope, violates)));
    };
    add_originated("D18-3-2-1-MUST-NOT-001", "unregistered-reserved-namespace-not-originated",
                   "publish-under-unregistered-period-prefixed-namespace", is_publication, unregistered_reserved);
    add_originated("D18-3-2-1-MUST-NOT-002", "no-originated-request-uses-single-period-namespace",
                   "originate-publisher-operations-under-single-period-namespace", any_origin, single_period);
    add_originated("D18-3-2-1-MUST-NOT-003", "single-period-track-not-published",
                   "publish-track-under-single-period-namespace", is_publish, single_period);
    add_originated("D18-3-2-1-MUST-NOT-004", "single-period-namespace-not-published",
                   "publish-namespace-under-single-period-namespace", is_announcement, single_period);
    add_originated("D18-3-2-2-MUST-NOT-001", "application-session-track-not-published",
                   "application-publish-track-in-session-namespace", is_publish, session_namespace);
    add_originated("D18-3-2-2-MUST-NOT-002", "application-session-namespace-not-published",
                   "application-publish-namespace-in-session-namespace", is_announcement, session_namespace);

    result.push_back(make_probe("D18-1-4-3-MUST-NOT-001", "encoded-key-value-type-sum-within-uint64",
        accepting_definition("publish-key-value-type-boundary", {},
            [](const RawProbeTranscript& transcript) { return transcript.peer_setup_received; }, deadline, true,
            setup_frame_complete),
        key_value_type_sums_fit));

    {
        // Ready once the streams have ended after PUBLISH_DONE, or once a long
        // window after PUBLISH_DONE shows that some stream stays open.
        const auto prompt = settled_after(streams_ended_before_publish_done, quiet_window(deadline));
        const auto patient = settled_after(publish_done_seen, std::max(deadline / 2, quiet_window(deadline)));
        result.push_back(make_probe("D18-5-1-1-MUST-NOT-001", "data-stream-closures-precede-publish-done",
            accepting_definition("finish-subscription-with-open-object-streams", {forwarding_subscribe(fixture)},
                                 [prompt, patient](const RawProbeTranscript& transcript) {
                                     const bool fast = prompt(transcript);
                                     const bool slow = patient(transcript);
                                     return fast || slow;
                                 }, deadline, false),
            streams_closed_before_publish_done, true));
    }

    result.push_back(make_probe("D18-2-5-1-MUST-001", "mandatory-property-only-at-track-scope",
        accepting_definition("publish-track-with-mandatory-property", {forwarding_subscribe(fixture)},
                             [](const RawProbeTranscript& transcript) { return !subscribed_objects(transcript).empty(); },
                             deadline),
        mandatory_property_only_at_track_scope, true));

    result.push_back(make_probe("D18-2-4-3-MUST-001", "distinct-content-tracks-have-distinct-full-names",
        accepting_definition("publish-distinct-content-tracks-in-same-scope", {forwarding_subscribe(fixture)},
                             two_tracks_observed, deadline),
        distinct_content_distinct_names, true));
    return result;
}

}  // namespace moq::interop::scenarios::contribution
