#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kSubscribeId = 1;
constexpr std::uint64_t kPaddingStreamType = 0x132B3E28;
constexpr std::uint64_t kPropertyPriorGroupIdGap = 0x3C;
constexpr std::uint64_t kPropertyPriorObjectIdGap = 0x3E;

std::vector<d18::ObjectEvent> subscription_objects(const RawProbeTranscript& transcript) {
    std::vector<d18::ObjectEvent> result;
    const auto alias = subscription_alias(transcript);
    if (!alias) return result;
    for (const auto& stream : subscription_streams(transcript, *alias))
        result.insert(result.end(), stream.objects.begin(), stream.objects.end());
    const auto datagrams = subscription_datagrams(transcript, *alias);
    result.insert(result.end(), datagrams.begin(), datagrams.end());
    return result;
}

// Counts `type` among an Object's mutable Properties and inside its
// Immutable Properties (section 12.7 requires searching both).
std::size_t property_count(const d18::KeyValuePairs& properties, std::uint64_t type) {
    std::size_t count = 0;
    for_each_object_property(properties, [&](const auto& property) {
        if (property.type == type) ++count;
    });
    return count;
}

RawProbeDefinition subscribe_definition(const char* id, const Fixture& fixture, d18::Parameters parameters,
                                        std::function<bool(const RawProbeTranscript&)> ready,
                                        std::chrono::milliseconds deadline) {
    return {id, setup_message({}),
            {{RawProbeChannel::NewBidi, subscribe_request(fixture, kSubscribeId, std::move(parameters)), false}},
            true, setup_ready, deadline, std::move(ready), {}};
}

// Sections 12.8 and 12.9: an Object carries at most one gap Property.
std::optional<bool> at_most_one_gap(const RawProbeTranscript& transcript, std::uint64_t type) {
    if (!bounded(transcript)) return std::nullopt;
    bool exercised = false;
    for (const auto& object : subscription_objects(transcript)) {
        const auto count = property_count(object.properties, type);
        if (count > 1) return false;
        exercised = exercised || count == 1;
    }
    return exercised ? std::optional<bool>{true} : std::nullopt;
}
bool gap_seen(const RawProbeTranscript& transcript, std::uint64_t type) {
    for (const auto& object : subscription_objects(transcript))
        if (property_count(object.properties, type) >= 1) return true;
    return false;
}

// Section 11.2.1.1: an Object with a nonzero status has an empty payload.
std::optional<bool> status_objects_are_empty(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    bool exercised = false;
    for (const auto& object : subscription_objects(transcript)) {
        if (!object.status || *object.status == 0) continue;
        if (object.payload_length != 0 || !object.retained_payload.empty()) return false;
        exercised = true;
    }
    return exercised ? std::optional<bool>{true} : std::nullopt;
}
bool status_object_seen(const RawProbeTranscript& transcript) {
    for (const auto& object : subscription_objects(transcript))
        if (object.status && *object.status != 0) return true;
    return false;
}

std::optional<bool> complete_subgroup_closed_with_fin(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto alias = subscription_alias(transcript);
    if (!alias) return std::nullopt;
    for (const auto& stream : subscription_streams(transcript, *alias)) {
        if (!delivered_final_object(stream)) continue;
        if (stream.fin) return true;
        if (stream.reset) return false;
    }
    return std::nullopt;
}
bool final_subgroup_closed(const RawProbeTranscript& transcript) {
    const auto alias = subscription_alias(transcript);
    if (!alias) return false;
    for (const auto& stream : subscription_streams(transcript, *alias))
        if (delivered_final_object(stream) && (stream.fin || stream.reset)) return true;
    return false;
}

// Section 11.5.1: every byte after the Padding Stream type is zero.
struct PaddingObservation {
    bool nonzero{false};
    std::size_t complete{0};
};
PaddingObservation padding_streams(const RawProbeTranscript& transcript) {
    PaddingObservation result;
    for (const auto& [id, stream] : peer_data_streams(transcript)) {
        (void)id;
        wire::Cursor cursor(stream.bytes);
        const auto type = wire::read_vi64(cursor);
        const auto* value = std::get_if<std::uint64_t>(&type);
        if (!value || *value != kPaddingStreamType) continue;
        wire::draft18::PaddingStreamDecoder decoder;
        const auto pushed = decoder.push(stream.bytes, stream.fin);
        if (pushed.error) continue;
        if (pushed.first_nonzero_offset) result.nonzero = true;
        else if (stream.fin) ++result.complete;
    }
    return result;
}
std::optional<bool> padding_stream_is_zero(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto observed = padding_streams(transcript);
    if (observed.nonzero) return false;
    return observed.complete > 0 ? std::optional<bool>{true} : std::nullopt;
}

// Section 11.5.2: every byte after the Padding Datagram type is zero.
PaddingObservation padding_datagrams(const RawProbeTranscript& transcript) {
    PaddingObservation result;
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* datagram = std::get_if<transport::DatagramEvent>(&event);
        if (!datagram) continue;
        const auto decoded = d18::decode_datagram(datagram->data, {});
        const auto* padding = std::get_if<d18::DiscardedPaddingDatagram>(&decoded);
        if (!padding) continue;
        if (padding->first_nonzero_offset) result.nonzero = true;
        else ++result.complete;
    }
    return result;
}
std::optional<bool> padding_datagram_is_zero(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto observed = padding_datagrams(transcript);
    if (observed.nonzero) return false;
    return observed.complete > 0 ? std::optional<bool>{true} : std::nullopt;
}

}  // namespace

std::vector<Draft18ContributionProbe> object_probes(std::chrono::milliseconds deadline, const Fixture& fixture) {
    std::vector<Draft18ContributionProbe> result;
    const auto add = [&](const char* requirement, const char* evaluator, RawProbeDefinition definition,
                         Observe observe, bool track) {
        result.push_back(make_probe(requirement, evaluator, std::move(definition), std::move(observe), track));
    };
    const auto passive = [&](const char* id, std::function<bool(const RawProbeTranscript&)> ready) {
        return RawProbeDefinition{id, setup_message({}), {}, true, setup_ready, deadline, std::move(ready), {}};
    };
    add("D18-11-5-1-MUST-001", "padding-data-bytes-all-zero",
        passive("observe-publisher-padding-stream", [](const RawProbeTranscript& transcript) {
            const auto observed = padding_streams(transcript);
            return observed.nonzero || observed.complete > 0;
        }), padding_stream_is_zero, false);
    add("D18-11-5-2-MUST-001", "padding-data-bytes-all-zero",
        passive("observe-publisher-padding-datagram", [](const RawProbeTranscript& transcript) {
            const auto observed = padding_datagrams(transcript);
            return observed.nonzero || observed.complete > 0;
        }), padding_datagram_is_zero, false);

    const d18::Parameters forwarding{forward_parameter(1)};
    add("D18-12-8-MUST-NOT-004", "object-has-at-most-one-prior-group-id-gap",
        subscribe_definition("publish-object-with-prior-group-id-gap", fixture, forwarding,
            [](const RawProbeTranscript& transcript) { return gap_seen(transcript, kPropertyPriorGroupIdGap); },
            deadline),
        [](const RawProbeTranscript& transcript, bool) { return at_most_one_gap(transcript, kPropertyPriorGroupIdGap); },
        true);
    add("D18-12-9-MUST-NOT-004", "object-has-at-most-one-prior-object-id-gap",
        subscribe_definition("publish-object-with-prior-object-id-gap", fixture, forwarding,
            [](const RawProbeTranscript& transcript) { return gap_seen(transcript, kPropertyPriorObjectIdGap); },
            deadline),
        [](const RawProbeTranscript& transcript, bool) { return at_most_one_gap(transcript, kPropertyPriorObjectIdGap); },
        true);
    add("D18-11-2-1-1-MUST-001", "non-normal-object-status-has-empty-payload",
        subscribe_definition("publish-end-of-group-and-end-of-track-status-objects", fixture, forwarding,
                             status_object_seen, deadline),
        status_objects_are_empty, true);
    // A Largest Object filter starts the subscription inside the current Group.
    const d18::Parameters largest_object_filter{forward_parameter(1),
        {0x21, d18::SubscriptionFilter{d18::SubscriptionFilterType::LargestObject, std::nullopt, std::nullopt}}};
    add("D18-11-4-3-MUST-001", "complete-subgroup-ends-with-fin",
        subscribe_definition("publish-complete-finite-subgroup-with-start-location-filter", fixture,
                             largest_object_filter, final_subgroup_closed, deadline),
        complete_subgroup_closed_with_fin, true);
    return result;
}

}  // namespace moq::interop::scenarios::contribution
