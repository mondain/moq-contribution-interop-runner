#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kSubscribeId = 1;
constexpr std::uint64_t kSecondId = 3;
constexpr std::uint64_t kFetchSerializationDatagramBit = 0x40;
constexpr std::uint64_t kCancelled = 0x1;
// Token Type 0 is negotiated out of band (section 10.2.2): the publisher's
// authorization policy is configured to refuse this value.
constexpr std::string_view kDeniedToken = "interop-denied";

std::optional<d18::Location> first_datagram_location(std::span<const transport::TransportEvent> events,
                                                     std::uint64_t alias) {
    for (const auto& event : events) {
        if (terminal(event)) break;
        const auto* datagram = std::get_if<transport::DatagramEvent>(&event);
        if (!datagram) continue;
        const auto decoded = d18::decode_datagram(datagram->data, {});
        const auto* object = std::get_if<d18::ObjectEvent>(&decoded);
        if (object && object->track_alias == alias) return d18::Location{object->group_id, object->object_id};
    }
    return std::nullopt;
}

// The start of the FETCH the probe prepared for the observed datagram Object.
std::optional<d18::Location> decode_fetch_request(const RawProbeTranscript& transcript) {
    if (transcript.writes.size() < 2) return std::nullopt;
    const auto message = [&]() -> std::optional<d18::Message> {
        wire::Cursor cursor(transcript.writes[1].write.bytes);
        auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
        if (auto* value = std::get_if<d18::Message>(&decoded)) return std::move(*value);
        return std::nullopt;
    }();
    if (!message) return std::nullopt;
    const auto* fetch = std::get_if<d18::FetchMessage>(&*message);
    const auto* standalone = fetch ? std::get_if<d18::StandaloneFetch>(&fetch->fetch) : nullptr;
    if (!standalone) return std::nullopt;
    return standalone->start;
}

// Section 11.4.4.1: a fetched Object that was sent as a datagram sets bit 0x40.
std::optional<bool> fetched_datagram_object_flags(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript) || transcript.writes.size() != 2) return std::nullopt;
    const auto request = decode_fetch_request(transcript);
    if (!request) return std::nullopt;
    for (const auto& [id, stream] : peer_data_streams(transcript)) {
        (void)id;
        d18::FetchDecoder decoder([](std::uint64_t request_id) -> std::optional<d18::FetchGroupOrder> {
            return request_id == kSecondId ? std::optional{d18::FetchGroupOrder::Ascending} : std::nullopt;
        });
        const auto pushed = decoder.push(stream.bytes, stream.fin);
        if (!pushed.header || pushed.header->request_id != kSecondId || pushed.events.empty()) continue;
        const auto* object = std::get_if<d18::ObjectEvent>(&pushed.events.front());
        if (!object || !object->serialization_flags || *object->serialization_flags >= 128) return std::nullopt;
        if (object->group_id != request->group || object->object_id != request->object) return std::nullopt;
        return (*object->serialization_flags & kFetchSerializationDatagramBit) != 0;
    }
    return std::nullopt;
}

// Section 11.1: one Track Alias never names two Tracks at once.
struct Assignment {
    d18::TrackNamespace track_namespace;
    d18::TrackName track_name;
    std::uint64_t alias{0};
};
std::optional<Assignment> subscribed_assignment(const RawProbeTranscript& transcript) {
    if (transcript.writes.empty()) return std::nullopt;
    wire::Cursor cursor(transcript.writes[0].write.bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    const auto* subscribe = message ? std::get_if<d18::SubscribeMessage>(message) : nullptr;
    const auto alias = subscription_alias(transcript);
    if (!subscribe || !alias) return std::nullopt;
    return Assignment{subscribe->track_namespace, subscribe->track_name, *alias};
}
std::optional<Assignment> published_assignment(const RawProbeTranscript& transcript, bool& finished) {
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 3u) != 0u) continue;
        const auto reply = stream_reply(transcript, data->stream_id);
        if (reply.messages.empty()) continue;
        const auto* publish = std::get_if<d18::PublishMessage>(&reply.messages.front());
        if (!publish) continue;
        finished = std::any_of(reply.messages.begin(), reply.messages.end(), [](const auto& message) {
            return std::holds_alternative<d18::PublishDoneMessage>(message);
        });
        return Assignment{publish->track_namespace, publish->track_name, publish->track_alias};
    }
    return std::nullopt;
}
std::optional<bool> distinct_tracks_have_distinct_aliases(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto subscribed = subscribed_assignment(transcript);
    bool published_done = false;
    const auto published = published_assignment(transcript, published_done);
    if (!subscribed || !published || published_done) return std::nullopt;
    // The subscription must still be Established: no PUBLISH_DONE on it either.
    const auto reply = write_reply(transcript, 0);
    if (std::any_of(reply.messages.begin(), reply.messages.end(), [](const auto& message) {
            return std::holds_alternative<d18::PublishDoneMessage>(message);
        })) return std::nullopt;
    if (subscribed->track_namespace.fields == published->track_namespace.fields &&
        subscribed->track_name.bytes == published->track_name.bytes) return std::nullopt;
    return subscribed->alias != published->alias;
}
bool both_tracks_assigned(const RawProbeTranscript& transcript) {
    bool done = false;
    return subscribed_assignment(transcript) && published_assignment(transcript, done);
}

// Section 11.2.1: an Object is sent according to its Forwarding Preference. The
// Object first seen on one subscription is looked for on a later one.
struct Delivery {
    std::uint64_t group{0};
    std::uint64_t object{0};
    d18::ObjectForwardingPreference preference{};
};
std::optional<Delivery> first_delivery(const RawProbeTranscript& transcript) {
    const auto alias = subscription_alias(transcript, 0);
    if (!alias) return std::nullopt;
    const auto objects = alias_objects(transcript, *alias);
    if (objects.empty()) return std::nullopt;
    return Delivery{objects.front().group_id, objects.front().object_id, objects.front().forwarding_preference};
}
std::optional<bool> redelivery_keeps_preference(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript) || transcript.writes.size() != 4) return std::nullopt;
    const auto first = first_delivery(transcript);
    const auto alias = subscription_alias(transcript, 3);
    if (!first || !alias) return std::nullopt;
    for (const auto& object : alias_objects(transcript, *alias))
        if (object.group_id == first->group && object.object_id == first->object)
            return object.forwarding_preference == first->preference;
    return std::nullopt;
}
bool redelivered(const RawProbeTranscript& transcript) {
    const auto first = first_delivery(transcript);
    const auto alias = subscription_alias(transcript, 3);
    if (!first || !alias) return false;
    for (const auto& object : alias_objects(transcript, *alias))
        if (object.group_id == first->group && object.object_id == first->object) return true;
    return false;
}

std::optional<bool> unauthorized_not_accepted(const RawProbeTranscript& transcript, bool configured) {
    // Without an operator-declared denied credential the publisher may legitimately
    // grant the request (an open policy authorizes every subscriber): nothing is provable.
    if (!configured || !bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (reply.messages.empty()) return std::nullopt;
    if (request_error(reply)) return true;
    if (std::holds_alternative<d18::RequestOkMessage>(reply.messages.front())) return false;
    return std::nullopt;
}

d18::Parameters denied_credentials(const std::optional<std::string>& denied) {
    return {{0x03, d18::Token{d18::TokenAliasType::UseValue, std::nullopt, 0, text(denied ? std::string_view(*denied) : kDeniedToken)}}};
}

}  // namespace

std::vector<Draft18ContributionProbe> exchange_probes(std::chrono::milliseconds deadline, const Fixture& fixture,
                                                      const std::optional<std::string>& denied_token) {
    std::vector<Draft18ContributionProbe> result;
    const d18::Parameters forwarding{forward_parameter(1)};
    const auto subscribe = [&](std::uint64_t id, d18::Parameters parameters) {
        return RawProbeWrite{RawProbeChannel::NewBidi, subscribe_request(fixture, id, std::move(parameters)), false};
    };
    {
        RawProbeWrite fetch{RawProbeChannel::NewBidi, {}, false};
        fetch.prepare_bytes = [fixture](const RawProbeGateInput& input) -> std::optional<Bytes> {
            const auto view = view_of(input);
            const auto alias = subscription_alias(view);
            if (!alias) return std::nullopt;
            const auto location = first_datagram_location(view.events, *alias);
            if (!location) return std::nullopt;
            return standalone_fetch(fixture, kSecondId, *location, {location->group, location->object + 1});
        };
        result.push_back(make_probe("D18-11-4-4-1-MUST-007", "fetch-datagram-object-sets-bit-0x40",
            RawProbeDefinition{"fetch-object-previously-observed-as-datagram", setup_message({}),
                {subscribe(kSubscribeId, forwarding), std::move(fetch)}, true, setup_ready, deadline,
                [](const RawProbeTranscript& transcript) {
                    return fetched_datagram_object_flags(transcript, false).has_value();
                }, {}},
            fetched_datagram_object_flags, true));
    }
    {
        RawProbeWrite accept{RawProbeChannel::PeerBidi, ok_response(), false};
        result.push_back(make_probe("D18-11-1-MUST-NOT-001", "no-simultaneous-track-alias-reuse-for-distinct-tracks",
            RawProbeDefinition{"publish-two-simultaneous-tracks", setup_message({}),
                {subscribe(kSubscribeId, {forward_parameter(0)}), std::move(accept)}, true, setup_ready, deadline,
                both_tracks_assigned, publisher_opener<d18::PublishMessage>},
            distinct_tracks_have_distinct_aliases, true));
    }
    {
        RawProbeWrite finish{RawProbeChannel::NewBidi, {}, true, 0};
        finish.evidence_ready = [](const RawProbeGateInput& input) { return first_delivery(view_of(input)).has_value(); };
        RawProbeWrite stop{RawProbeChannel::NewBidi, {}, false, 1};
        stop.operation = RawProbeOperation::StopSending;
        stop.application_error = kCancelled;
        // Subscribe again from the observed Object once the first subscription has ended.
        RawProbeWrite again{RawProbeChannel::NewBidi, {}, false};
        again.prepare_bytes = [fixture](const RawProbeGateInput& input) -> std::optional<Bytes> {
            const auto view = view_of(input);
            const auto first = first_delivery(view);
            if (!first) return std::nullopt;
            const auto stream = write_stream(view, 0);
            const auto reply = stream_reply(view, *stream, write_marker(view, 0));
            if (!reply.fin && !reply.reset) return std::nullopt;
            return subscribe_request(fixture, kSecondId, {forward_parameter(1),
                {0x21, d18::SubscriptionFilter{d18::SubscriptionFilterType::AbsoluteStart,
                                               d18::Location{first->group, first->object}, std::nullopt}}});
        };
        result.push_back(make_probe("D18-11-2-1-MUST-001",
            "subscription-delivery-preserves-observed-object-forwarding-preference",
            RawProbeDefinition{"redeliver-previously-observed-object-in-later-subscription", setup_message({}),
                {subscribe(kSubscribeId, forwarding), std::move(finish), std::move(stop), std::move(again)},
                true, setup_ready, deadline, redelivered, {}},
            redelivery_keeps_preference, true));
    }
    const auto prefix = d18::TrackNamespace{fixture.track_namespace.empty() ? Namespace{text("a")} : fixture.track_namespace};
    result.push_back(make_probe("D18-10-18-MUST-004", "unauthorized-namespace-subscription-not-accepted",
        RawProbeDefinition{"receive-subscribe-namespace-denied-by-configured-authorization-policy", setup_message({}),
            {{RawProbeChannel::NewBidi, encode(d18::SubscribeNamespaceMessage{kSubscribeId, prefix, denied_credentials(denied_token)}), false}},
            true, setup_ready, deadline, first_response_or_close(0), {}},
        [configured = denied_token.has_value()](const RawProbeTranscript& transcript, bool) {
            return unauthorized_not_accepted(transcript, configured);
        }, true));
    result.push_back(make_probe("D18-10-19-MUST-004", "unauthorized-namespace-subscription-not-accepted",
        RawProbeDefinition{"receive-subscribe-tracks-denied-by-configured-authorization-policy", setup_message({}),
            {{RawProbeChannel::NewBidi, encode(d18::SubscribeTracksMessage{kSubscribeId, prefix, denied_credentials(denied_token)}), false}},
            true, setup_ready, deadline, first_response_or_close(0), {}},
        [configured = denied_token.has_value()](const RawProbeTranscript& transcript, bool) {
            return unauthorized_not_accepted(transcript, configured);
        }, true));
    return result;
}

}  // namespace moq::interop::scenarios::contribution
