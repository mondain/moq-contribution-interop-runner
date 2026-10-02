#pragma once

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

// Parses SETUP framing and options without applying the duplicate-option
// validation of the generic decoder, so that a publisher's repeated option is
// observable instead of hiding its SETUP.
inline bool parse_setup(std::span<const std::byte> input,
                        d18::KeyValuePairs* options = nullptr,
                        std::size_t* consumed = nullptr) {
    wire::Cursor cursor(input);
    const auto type = wire::read_vi64(cursor);
    const auto* value = std::get_if<std::uint64_t>(&type);
    if (!value || *value != kSetupType) return false;
    const auto high = wire::read_bytes(cursor, 2);
    const auto* length_bytes = std::get_if<std::span<const std::byte>>(&high);
    if (!length_bytes) return false;
    const std::size_t length =
        (std::to_integer<std::size_t>((*length_bytes)[0]) << 8u) |
        std::to_integer<std::size_t>((*length_bytes)[1]);
    if (cursor.remaining() < length) return false;
    const auto decoded = d18::decode_key_value_pairs(cursor, length, {});
    const auto* pairs = std::get_if<d18::KeyValuePairs>(&decoded);
    if (!pairs) return false;
    if (options) *options = *pairs;
    if (consumed) *consumed = cursor.offset();
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
            it->second.fin = it->second.fin || data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if ((reset->stream_id & 3u) != 2u) continue;
            auto [it, inserted] = result.try_emplace(reset->stream_id);
            if (inserted) it->second.first_event = i;
            it->second.reset = true;
            it->second.reset_code = reset->application_error;
        }
    }
    return result;
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

// Becomes true once `seen` has held for `window`. The first sighting is
// forgotten whenever `seen` is false, so a definition reused for a new
// session starts its window afresh.
inline std::function<bool(const RawProbeTranscript&)> settled_after(
    std::function<bool(const RawProbeTranscript&)> seen, std::chrono::milliseconds window) {
    auto first = std::make_shared<std::optional<RawProbeClock::time_point>>();
    return [seen = std::move(seen), window, first](const RawProbeTranscript& transcript) {
        if (!seen(transcript)) {
            first->reset();
            return false;
        }
        if (!*first) *first = RawProbeClock::now();
        return RawProbeClock::now() - **first >= window;
    };
}
inline std::chrono::milliseconds quiet_window(std::chrono::milliseconds deadline) {
    return std::clamp(deadline / 4, std::chrono::milliseconds{1}, std::chrono::milliseconds{50});
}

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

}  // namespace moq::interop::scenarios::contribution
