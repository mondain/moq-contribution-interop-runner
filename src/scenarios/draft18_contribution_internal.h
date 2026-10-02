#pragma once

#include "draft18_probe_timing.h"
#include "moq/interop/scenarios/draft18_contribution.h"
#include "moq/interop/wire/draft18/messages.h"
#include "moq/interop/wire/draft18/objects.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Shared helpers for the draft-18 publisher-contribution probe families.
namespace moq::interop::scenarios::contribution {

namespace d18 = wire::draft18;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
using Observe = std::function<std::optional<bool>(const RawProbeTranscript&, bool)>;

inline constexpr std::size_t kMaximumFrame = 65546;
inline constexpr std::size_t kMaximumEvents = 4096;
inline constexpr std::uint64_t kSetupType = 0x2F00;
// Section 10.6 and 15.10.2: REQUEST_ERROR codes used by these families.
inline constexpr std::uint64_t kErrorGoingAway = 0x6;
inline constexpr std::uint64_t kErrorDoesNotExist = 0x10;
inline constexpr std::uint64_t kErrorInvalidRange = 0x11;
// Section 15.10.1: session termination codes.
inline constexpr std::uint64_t kCloseProtocolViolation = 0x3;
inline constexpr std::uint64_t kCloseAuthTokenCacheOverflow = 0x13;
inline constexpr std::uint64_t kGreaseOdd = 0x9D;
inline constexpr std::uint64_t kGreaseEven = 0x11C;

struct Fixture {
    Namespace track_namespace;
    Bytes track_name;
};

inline Bytes text(std::string_view value) {
    Bytes result;
    for (const auto c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

inline Bytes encode(const d18::Message& message) {
    wire::ByteWriter output(kMaximumFrame);
    if (!d18::encode_message(message, output).has_value())
        throw std::invalid_argument("unencodable draft-18 contribution message");
    return {output.bytes().begin(), output.bytes().end()};
}

inline d18::KeyValuePair odd_option(std::uint64_t type, Bytes value) {
    return {type, d18::ByteValue{std::move(value)}};
}
inline d18::KeyValuePair even_option(std::uint64_t type, std::uint64_t value) {
    return {type, d18::VarIntValue{value, {}}};
}

inline Bytes setup_message(d18::KeyValuePairs options) {
    return encode(d18::SetupMessage{std::move(options)});
}

inline d18::TrackNamespace track_namespace(const Namespace& fields) {
    return {fields};
}

// SETUP framing only: the Type, the 16-bit length and that many bytes. Returns
// the payload (the Setup Options) when the frame is complete, without
// validating the options, and reports the bytes the frame occupies.
inline std::optional<std::span<const std::byte>> setup_frame_payload(
    std::span<const std::byte> input, std::size_t* consumed = nullptr) {
    wire::Cursor cursor(input);
    const auto type = wire::read_vi64(cursor);
    const auto* value = std::get_if<std::uint64_t>(&type);
    if (!value || *value != kSetupType) return std::nullopt;
    const auto high = wire::read_bytes(cursor, 2);
    const auto* length_bytes = std::get_if<std::span<const std::byte>>(&high);
    if (!length_bytes) return std::nullopt;
    const std::size_t length =
        (std::to_integer<std::size_t>((*length_bytes)[0]) << 8u) |
        std::to_integer<std::size_t>((*length_bytes)[1]);
    if (cursor.remaining() < length) return std::nullopt;
    const auto payload = wire::read_bytes(cursor, length);
    const auto* block = std::get_if<std::span<const std::byte>>(&payload);
    if (!block) return std::nullopt;
    if (consumed) *consumed = cursor.offset();
    return *block;
}

// Parses SETUP framing and options without applying the duplicate-option
// validation of the generic decoder, so that a publisher's repeated option is
// observable instead of hiding its SETUP.
inline bool parse_setup(std::span<const std::byte> input,
                        d18::KeyValuePairs* options = nullptr,
                        std::size_t* consumed = nullptr) {
    const auto payload = setup_frame_payload(input, consumed);
    if (!payload) return false;
    wire::Cursor cursor(*payload);
    const auto decoded = d18::decode_key_value_pairs(cursor, payload->size(), {});
    const auto* pairs = std::get_if<d18::KeyValuePairs>(&decoded);
    if (!pairs) return false;
    if (options) *options = *pairs;
    return true;
}
inline bool setup_ready(std::span<const std::byte> input) { return parse_setup(input); }

inline bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
           std::holds_alternative<transport::LocalCloseEvent>(event) ||
           std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
           std::holds_alternative<transport::TransportErrorEvent>(event) ||
           std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}

// Bounds the evidence an evaluator is willing to inspect.
inline bool bounded(const RawProbeTranscript& transcript) {
    if (transcript.events.size() > kMaximumEvents) return false;
    std::size_t total = 0;
    for (const auto& event : transcript.events) {
        const std::size_t size =
            std::holds_alternative<transport::StreamDataEvent>(event)
                ? std::get<transport::StreamDataEvent>(event).data.size()
            : std::holds_alternative<transport::DatagramEvent>(event)
                ? std::get<transport::DatagramEvent>(event).data.size()
                : 0;
        if (size > 8 * kMaximumFrame - total) return false;
        total += size;
    }
    return true;
}

struct PeerControl {
    d18::KeyValuePairs setup_options;
    transport::StreamId stream{0};
    // Messages following SETUP on the publisher's control stream.
    std::vector<d18::Message> messages;
    bool malformed{false};
};

// The publisher's control stream is its first unidirectional stream whose
// leading bytes are a complete SETUP message.
inline std::optional<PeerControl> peer_control(const RawProbeTranscript& transcript) {
    std::map<transport::StreamId, Bytes> streams;
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 3u) != 2u) continue;
        auto& bytes = streams[data->stream_id];
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
    }
    for (const auto& [id, bytes] : streams) {
        PeerControl result;
        std::size_t consumed = 0;
        if (!parse_setup(bytes, &result.setup_options, &consumed)) continue;
        result.stream = id;
        wire::Cursor cursor(std::span<const std::byte>(bytes).subspan(consumed));
        while (cursor.remaining() != 0) {
            auto decoded = d18::decode_message(d18::StreamRole::Control, cursor, {});
            if (auto* message = std::get_if<d18::Message>(&decoded)) {
                result.messages.push_back(std::move(*message));
            } else {
                result.malformed = !std::holds_alternative<wire::NeedMore>(decoded);
                break;
            }
        }
        return result;
    }
    return std::nullopt;
}

struct Reply {
    std::vector<d18::Message> messages;
    Bytes bytes;
    bool fin{false};
    bool reset{false};
    std::optional<std::uint64_t> reset_code;
    bool malformed{false};
    bool incomplete{false};
    bool overflow{false};
};

// Collects and decodes publisher messages on one request stream.
inline Reply stream_reply(const RawProbeTranscript& transcript, transport::StreamId stream,
                          std::size_t begin = 0) {
    Reply result;
    for (std::size_t i = begin; i < transcript.events.size(); ++i) {
        const auto& event = transcript.events[i];
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && data->stream_id == stream) {
            if (data->data.size() > 8 * kMaximumFrame - result.bytes.size()) {
                result.overflow = true;
                break;
            }
            result.bytes.insert(result.bytes.end(), data->data.begin(), data->data.end());
            result.fin = result.fin || data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
                   reset && reset->stream_id == stream) {
            result.reset = true;
            result.reset_code = reset->application_error;
        }
    }
    wire::Cursor cursor(result.bytes);
    while (cursor.remaining() != 0) {
        auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
        if (auto* message = std::get_if<d18::Message>(&decoded)) {
            result.messages.push_back(std::move(*message));
        } else {
            result.incomplete = std::holds_alternative<wire::NeedMore>(decoded);
            result.malformed = !result.incomplete;
            break;
        }
    }
    return result;
}

inline std::optional<transport::StreamId> write_stream(const RawProbeTranscript& transcript,
                                                       std::size_t index) {
    if (index >= transcript.writes.size()) return std::nullopt;
    return transcript.writes[index].stream_id;
}
inline std::size_t write_marker(const RawProbeTranscript& transcript, std::size_t index) {
    if (index >= transcript.writes.size()) return transcript.events.size();
    return transcript.writes[index].delivery_event_count.value_or(0);
}
inline Reply write_reply(const RawProbeTranscript& transcript, std::size_t index) {
    const auto stream = write_stream(transcript, index);
    if (!stream) return {};
    return stream_reply(transcript, *stream, write_marker(transcript, index));
}

struct SessionClose {
    transport::CloseErrorSpace space;
    std::uint64_t code;
};
inline std::optional<SessionClose> peer_close(const RawProbeTranscript& transcript) {
    for (const auto& event : transcript.events)
        if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event))
            return SessionClose{close->error_space, close->error_code};
    return std::nullopt;
}
inline bool application_close(const RawProbeTranscript& transcript) {
    const auto close = peer_close(transcript);
    return close && close->space == transport::CloseErrorSpace::Application;
}

inline bool is_typed_response(const d18::Message& message) {
    return std::holds_alternative<d18::RequestOkMessage>(message) ||
           std::holds_alternative<d18::RequestErrorMessage>(message);
}
inline const d18::RequestErrorMessage* request_error(const Reply& reply, std::size_t index = 0) {
    return index < reply.messages.size()
               ? std::get_if<d18::RequestErrorMessage>(&reply.messages[index])
               : nullptr;
}

inline Draft18ContributionProbe make_probe(std::string requirement, std::string evaluator,
                                           RawProbeDefinition definition, Observe observe,
                                           bool requires_track = false) {
    return {std::move(requirement), std::move(evaluator), std::move(definition),
            requires_track, std::move(observe)};
}

// Typed first response to the request written at `write_index`.
inline std::function<bool(const RawProbeTranscript&)> first_response_or_close(
    std::size_t write_index) {
    return [write_index](const RawProbeTranscript& transcript) {
        const auto reply = write_reply(transcript, write_index);
        return !reply.messages.empty() || application_close(transcript);
    };
}

// Peer-opened unidirectional streams other than the control stream, with the
// bytes and closure observed before the first terminal event.
struct DataStream {
    Bytes bytes;
    bool fin{false};
    bool reset{false};
    std::optional<std::uint64_t> reset_code;
    std::size_t first_event{0};
    // Index of the first event that ended the stream (FIN or reset).
    std::optional<std::size_t> closed_event;
};
inline std::map<transport::StreamId, DataStream> peer_data_streams(const RawProbeTranscript& transcript) {
    std::map<transport::StreamId, DataStream> result;
    const auto control = peer_control(transcript);
    for (std::size_t i = 0; i < transcript.events.size(); ++i) {
        const auto& event = transcript.events[i];
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if ((data->stream_id & 3u) != 2u || (control && control->stream == data->stream_id)) continue;
            auto [it, inserted] = result.try_emplace(data->stream_id);
            if (inserted) it->second.first_event = i;
            it->second.bytes.insert(it->second.bytes.end(), data->data.begin(), data->data.end());
            if (data->fin && !it->second.closed_event) it->second.closed_event = i;
            it->second.fin = it->second.fin || data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if ((reset->stream_id & 3u) != 2u) continue;
            auto [it, inserted] = result.try_emplace(reset->stream_id);
            if (inserted) it->second.first_event = i;
            if (!it->second.closed_event) it->second.closed_event = i;
            it->second.reset = true;
            it->second.reset_code = reset->application_error;
        }
    }
    return result;
}

// A transcript view of what a gate or byte preparer may inspect.
inline RawProbeTranscript view_of(const RawProbeGateInput& input) {
    RawProbeTranscript view;
    view.events.assign(input.events.begin(), input.events.end());
    view.writes.assign(input.prior_writes.begin(), input.prior_writes.end());
    return view;
}

// Request builders. The runner is the MOQT server, so its Request IDs are odd.
inline d18::Parameter forward_parameter(std::uint8_t value) {
    return {0x10, d18::Uint8ParameterValue{value}};
}
inline d18::Parameter priority_parameter(std::uint8_t value) {
    return {0x20, d18::Uint8ParameterValue{value}};
}
inline Bytes subscribe_request(const Fixture& fixture, std::uint64_t id, d18::Parameters parameters = {}) {
    return encode(d18::SubscribeMessage{id, track_namespace(fixture.track_namespace),
                                        d18::TrackName{fixture.track_name}, std::move(parameters)});
}
inline Bytes request_update(std::uint64_t id, d18::Parameters parameters) {
    return encode(d18::RequestUpdateMessage{id, std::move(parameters)});
}
inline Bytes standalone_fetch(const Fixture& fixture, std::uint64_t id, d18::Location start,
                              d18::Location end, d18::Parameters parameters = {}) {
    return encode(d18::FetchMessage{id,
        d18::StandaloneFetch{track_namespace(fixture.track_namespace), d18::TrackName{fixture.track_name},
                             start, end}, std::move(parameters)});
}
inline Bytes relative_joining_fetch(std::uint64_t id, std::uint64_t joining_request, std::uint64_t start) {
    return encode(d18::FetchMessage{id, d18::RelativeJoiningFetch{joining_request, start}, {}});
}

inline std::optional<d18::Location> largest_object(const d18::Parameters& parameters) {
    for (const auto& parameter : parameters)
        if (parameter.type == 0x09)
            if (const auto* location = std::get_if<d18::Location>(&parameter.value)) return *location;
    return std::nullopt;
}
inline const d18::SubscribeOkMessage* subscribe_ok(const Reply& reply, std::size_t index = 0) {
    return index < reply.messages.size()
               ? std::get_if<d18::SubscribeOkMessage>(&reply.messages[index]) : nullptr;
}
inline bool first_message_is_subscribe_ok(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    return message && std::holds_alternative<d18::SubscribeOkMessage>(*message);
}

// Reply to a prior write as seen by a gate or byte preparer.
inline Reply gate_reply(const RawProbeGateInput& input, std::size_t write_index) {
    Reply result;
    if (write_index >= input.prior_writes.size()) return result;
    const auto& write = input.prior_writes[write_index];
    if (!write.stream_id || !write.delivery_event_count ||
        *write.delivery_event_count > input.events.size()) return result;
    RawProbeTranscript view;
    view.events.assign(input.events.begin(), input.events.end());
    return stream_reply(view, *write.stream_id, *write.delivery_event_count);
}

struct SubgroupStream {
    transport::StreamId id{0};
    d18::SubgroupHeader header{};
    std::vector<d18::ObjectEvent> objects;
    bool fin{false};
    bool reset{false};
    bool malformed{false};
    std::size_t first_event{0};
    std::optional<std::size_t> closed_event;
};

inline std::optional<std::uint64_t> subscription_alias(const RawProbeTranscript& transcript,
                                                       std::size_t subscribe_write = 0) {
    const auto reply = write_reply(transcript, subscribe_write);
    if (const auto* ok = subscribe_ok(reply)) return ok->track_alias;
    return std::nullopt;
}

// Subgroup streams of the subscription's Track Alias, decoded from the
// bytes seen before any terminal transport event.
inline std::vector<SubgroupStream> subscription_streams(const RawProbeTranscript& transcript, std::uint64_t alias) {
    std::vector<SubgroupStream> result;
    for (const auto& [id, stream] : peer_data_streams(transcript)) {
        d18::SubgroupDecoder decoder;
        const auto pushed = decoder.push(stream.bytes, stream.fin);
        if (!pushed.header || pushed.header->track_alias != alias) continue;
        SubgroupStream entry;
        entry.id = id;
        entry.header = *pushed.header;
        entry.objects = pushed.objects;
        entry.fin = stream.fin;
        entry.reset = stream.reset;
        entry.first_event = stream.first_event;
        entry.closed_event = stream.closed_event;
        entry.malformed = pushed.error.has_value();
        result.push_back(std::move(entry));
    }
    return result;
}


inline std::vector<d18::ObjectEvent> subscription_datagrams(const RawProbeTranscript& transcript, std::uint64_t alias) {
    std::vector<d18::ObjectEvent> result;
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* datagram = std::get_if<transport::DatagramEvent>(&event);
        if (!datagram) continue;
        const auto decoded = d18::decode_datagram(datagram->data, {});
        if (const auto* object = std::get_if<d18::ObjectEvent>(&decoded))
            if (object->track_alias == alias) result.push_back(*object);
    }
    return result;
}

// Every Object delivered under `alias`, streams first and then datagrams.
inline std::vector<d18::ObjectEvent> alias_objects(const RawProbeTranscript& transcript, std::uint64_t alias) {
    std::vector<d18::ObjectEvent> result;
    for (const auto& stream : subscription_streams(transcript, alias))
        result.insert(result.end(), stream.objects.begin(), stream.objects.end());
    const auto datagrams = subscription_datagrams(transcript, alias);
    result.insert(result.end(), datagrams.begin(), datagrams.end());
    return result;
}

inline constexpr std::uint64_t kStatusEndOfGroup = 0x3;
inline constexpr std::uint64_t kStatusEndOfTrack = 0x4;
// A subgroup that delivered its final Object (End of Group or End of Track)
// is complete: section 11.4.3 then requires a FIN rather than a reset.
inline bool delivered_final_object(const SubgroupStream& stream) {
    return !stream.malformed && !stream.objects.empty() && stream.objects.back().status &&
           (*stream.objects.back().status == kStatusEndOfGroup ||
            *stream.objects.back().status == kStatusEndOfTrack);
}

// Section 12.7: Immutable Properties (type 0x0B) carries a Key-Value-Pair list
// that may not itself contain Immutable Properties, so one decoded level is the
// whole search space. Nested containers in a peer's payload stay opaque, which
// keeps the work linear in the payload instead of following the nesting.
inline constexpr std::uint64_t kPropertyImmutable = 0x0B;

// Calls `visit` for every mutable Property and every Property found directly
// inside an Immutable Properties container.
template <typename Visit>
void for_each_object_property(const d18::KeyValuePairs& properties, Visit&& visit) {
    for (const auto& property : properties) {
        visit(property);
        if (property.type != kPropertyImmutable) continue;
        const auto* nested = std::get_if<d18::ByteValue>(&property.value);
        if (!nested) continue;
        wire::Cursor cursor(nested->bytes);
        const auto decoded = d18::decode_key_value_pairs(cursor, nested->bytes.size(), {});
        if (const auto* inner = std::get_if<d18::KeyValuePairs>(&decoded))
            for (const auto& entry : *inner) visit(entry);
    }
}

using probe_timing::quiet_window;
using probe_timing::settled_after;

// A complete first message of type `Message` opening a publisher-initiated
// request stream. Section 10.1: the publisher is the client and uses even IDs.
template <class... Opening>
inline bool publisher_opener(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message || cursor.remaining() != 0) return false;
    return std::visit([](const auto& opening) {
        if constexpr ((std::is_same_v<std::decay_t<decltype(opening)>, Opening> || ...))
            return (opening.request_id & 1u) == 0u;
        else
            return false;
    }, *message);
}

// The discovery request that follows a stimulus to show that the session
// survived it. A typed reply is a survival proof; closing is a failure.
inline Bytes survival_request(std::uint64_t id = 1) {
    return encode(d18::SubscribeNamespaceMessage{id, d18::TrackNamespace{{text("a")}}, {}});
}
inline std::optional<bool> session_survived(const RawProbeTranscript& transcript, std::size_t write_index) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, write_index);
    if (!reply.messages.empty())
        return is_typed_response(reply.messages.front()) ? std::optional<bool>{true} : std::nullopt;
    if (application_close(transcript)) return false;
    return std::nullopt;
}

inline Bytes ok_response() { return encode(d18::RequestOkMessage{{}, {}}); }

// Module entry points.
std::vector<Draft18ContributionProbe> setup_probes(std::chrono::milliseconds deadline);
std::vector<Draft18ContributionProbe> subscription_probes(std::chrono::milliseconds deadline,
                                                          const Fixture& fixture);
std::vector<Draft18ContributionProbe> publisher_initiated_probes(std::chrono::milliseconds deadline);
std::vector<Draft18ContributionProbe> object_probes(std::chrono::milliseconds deadline, const Fixture& fixture);
std::vector<Draft18ContributionProbe> goaway_probes(std::chrono::milliseconds deadline);
std::vector<Draft18ContributionProbe> uri_probes(std::chrono::milliseconds deadline);
std::vector<Draft18ContributionProbe> closure_probes(std::chrono::milliseconds deadline, const Fixture& fixture);
std::vector<Draft18ContributionProbe> exchange_probes(std::chrono::milliseconds deadline, const Fixture& fixture);
std::vector<Draft18ContributionProbe> origination_probes(std::chrono::milliseconds deadline, const Fixture& fixture);

}  // namespace moq::interop::scenarios::contribution
