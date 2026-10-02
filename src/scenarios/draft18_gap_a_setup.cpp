#include "draft18_gap_a_common.h"

#include <algorithm>
#include <string_view>

namespace moq::interop::scenarios::gap_a {
namespace {

constexpr std::uint64_t kProtocolViolation = 0x3;
constexpr std::uint64_t kAuthorityOption = 0x5;
constexpr std::uint64_t kPathOption = 0x1;
constexpr std::uint64_t kCacheSizeOption = 0x4;
// Section 14: 0x7f * N + 0x9D is reserved for greasing; N = 0 is never
// assigned, so every receiver must ignore it.
constexpr std::uint64_t kGreaseOption = 0x9d;
constexpr std::string_view kUnknownTrack = "interop-unknown-78937ae9d2abc4e0b6c1";
// Section 10.2.2: Token Type 0 is negotiated out of band, so any receiver
// can parse the structure while remaining free to reject the value.
constexpr std::string_view kTokenValue = "intp";
constexpr std::uint64_t kTokenAlias = 1;
// Section 10.3.1.3: 16 bytes plus the Token Value size.
constexpr std::uint64_t kTokenCacheCost = 16 + kTokenValue.size();

Bytes text(std::string_view value) {
    Bytes result;
    for (const auto c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

enum class Kind { None, Ok, Error, Other };
struct Outcome {
    Kind kind{Kind::None};
    std::uint64_t code{0};
};

Outcome outcome_of(const StreamMessages& messages) {
    if (messages.malformed || messages.undecodable) return {Kind::Other, 0};
    const auto* first = first_response(messages);
    if (!first) return {};
    if (std::holds_alternative<d18::RequestOkMessage>(*first) ||
        std::holds_alternative<d18::SubscribeOkMessage>(*first)) return {Kind::Ok, 0};
    if (const auto* error = std::get_if<d18::RequestErrorMessage>(first))
        return {Kind::Error, error->error_code};
    return {Kind::Other, 0};
}

std::optional<Outcome> write_outcome(const Streams& streams, const RawProbeAcceptedWrite& write) {
    const auto messages = messages_of(streams, write);
    if (!messages) return std::nullopt;
    const auto outcome = outcome_of(*messages);
    if (outcome.kind == Kind::None) return std::nullopt;
    return outcome;
}

// ---- receive-setup-with-unknown-option -------------------------------------
RawProbeDefinition unknown_option_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("receive-setup-with-unknown-option",
        setup_frame({{kGreaseOption, d18::ByteValue{{std::byte{0}}}}}), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, subscribe_request(1, fixture)));
    return definition;
}

Observation unknown_option_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto outcome = write_outcome(*streams, transcript.writes[0]);
    // A typed SUBSCRIBE_OK or REQUEST_ERROR shows the session continued.
    if (outcome && (outcome->kind == Kind::Ok || outcome->kind == Kind::Error)) {
        observation.ready = true;
        observation.result = true;
        return observation;
    }
    if (application_close_code(transcript.events) == kProtocolViolation) {
        observation.ready = true;
        observation.result = false;
    }
    return observation;
}

// ---- complete-publisher-requests-while-session-remains-open ----------------
RawProbeDefinition control_open_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("complete-publisher-requests-while-session-remains-open",
        setup_frame(), deadline);
    // TRACK_STATUS closes its stream after the response (Section 10.14), so
    // the request completes without cancellation in either outcome.
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, track_status_request(1, fixture), true));
    return definition;
}

Observation control_open_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 1) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto control = peer_control(*streams);
    if (!control) return observation;
    const auto& control_stream = streams->at(control->stream_id);
    if (control_stream.fin || control_stream.reset) {
        observation.ready = true;
        observation.result = false;
        return observation;
    }
    const auto* request = local_stream(*streams, transcript.writes[0]);
    if (request && (request->fin || request->reset) && !request->bytes.empty()) {
        // The request completed while the control stream stayed open.
        observation.ready = true;
        observation.result = true;
    }
    return observation;
}

// ---- establish-moqt-with-datagram-capable-peer ------------------------------
RawProbeDefinition datagram_definition(const Fixture&, std::chrono::milliseconds deadline) {
    return make_definition("establish-moqt-with-datagram-capable-peer", setup_frame(), deadline);
}

Observation datagram_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (!transcript.transport_established || !transcript.peer_setup_received) return observation;
    // The transport closes any connection that fails to negotiate QUIC
    // DATAGRAM before MOQT bytes flow, so only success leaves evidence.
    std::size_t established = 0;
    std::size_t capacity = 0;
    for (const auto& event : transcript.events) {
        if (const auto* value = std::get_if<transport::ConnectionEstablishedEvent>(&event)) {
            ++established;
            capacity = value->max_datagram_payload;
        }
    }
    observation.ready = true;
    if (established == 1 && transcript.max_datagram_payload == capacity) observation.result = capacity > 0;
    return observation;
}

// ---- native-quic-publisher-client-setup-from-moqt-uri -----------------------
Observation native_setup_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto control = peer_control(*streams);
    if (!control) return observation;
    bool authority = false;
    bool path = false;
    for (const auto& option : control->setup.options) {
        const auto* bytes = std::get_if<d18::ByteValue>(&option.value);
        if (option.type == kAuthorityOption && bytes && !bytes->bytes.empty()) authority = true;
        // PATH is the path-abempty of the URI, so an empty path is valid.
        if (option.type == kPathOption && bytes) path = true;
    }
    observation.ready = true;
    observation.result = authority && path;
    return observation;
}

RawProbeDefinition native_setup_definition(const Fixture&, std::chrono::milliseconds deadline) {
    return make_definition("native-quic-publisher-client-setup-from-moqt-uri", setup_frame(), deadline);
}

// ---- authorization token alias probes -----------------------------------------
d18::Parameter token(d18::TokenAliasType type) {
    d18::Token value;
    value.alias_type = type;
    if (type == d18::TokenAliasType::Register) {
        value.alias = kTokenAlias;
        value.token_type = 0;
        value.token_value = text(kTokenValue);
    } else {
        value.alias = kTokenAlias;
    }
    return token_parameter(std::move(value));
}

Fixture unknown_track_fixture(const Fixture& fixture) {
    return {fixture.track_namespace, text(kUnknownTrack)};
}

// REGISTER only when the publisher advertised enough cache (Section 10.3.1.3);
// otherwise the stimulus is withheld and the context stays unscored.
std::function<std::optional<Bytes>(const RawProbeGateInput&)> register_when_cached(const Fixture& target) {
    return [target](const RawProbeGateInput& input) -> std::optional<Bytes> {
        const auto streams = collect_streams(input.events);
        if (!streams) return std::nullopt;
        const auto control = peer_control(*streams);
        if (!control) return std::nullopt;
        for (const auto& option : control->setup.options) {
            const auto* value = std::get_if<d18::VarIntValue>(&option.value);
            if (option.type == kCacheSizeOption && value && value->value >= kTokenCacheCost)
                return track_status_request(1, target, {token(d18::TokenAliasType::Register)});
        }
        return std::nullopt;
    };
}

std::function<bool(const RawProbeGateInput&)> response_complete(std::size_t index) {
    return [index](const RawProbeGateInput& input) {
        if (index >= input.prior_writes.size()) return false;
        const auto streams = collect_streams(input.events);
        if (!streams) return false;
        const auto outcome = write_outcome(*streams, input.prior_writes[index]);
        return outcome && (outcome->kind == Kind::Ok || outcome->kind == Kind::Error);
    };
}

RawProbeWrite token_write(std::uint64_t request_id, const Fixture& target, d18::TokenAliasType type,
                          std::size_t after) {
    auto write = make_write(RawProbeChannel::NewBidi,
        track_status_request(request_id, target, {token(type)}), true);
    write.evidence_ready = response_complete(after);
    return write;
}

RawProbeWrite register_write(const Fixture& target) {
    RawProbeWrite write;
    write.channel = RawProbeChannel::NewBidi;
    write.fin = true;
    write.prepare_bytes = register_when_cached(target);
    return write;
}

RawProbeDefinition delete_then_use_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("register-delete-then-use-token-alias", setup_frame(), deadline);
    definition.writes.push_back(register_write(fixture));
    definition.writes.push_back(token_write(3, fixture, d18::TokenAliasType::Delete, 0));
    definition.writes.push_back(token_write(5, fixture, d18::TokenAliasType::UseAlias, 1));
    return definition;
}

// Section 10.2.2 names UNKNOWN_AUTH_TOKEN_ALIAS but assigns it no
// REQUEST_ERROR code, so scoring needs the configured compatibility code.
Observation delete_then_use_observe(const RawProbeTranscript& transcript, const Fixture&) {
    Observation observation;
    if (transcript.writes.size() != 3) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto outcome = write_outcome(*streams, transcript.writes[2]);
    if (!outcome) {
        if (application_close_code(transcript.events)) { observation.ready = true; observation.result = false; }
        return observation;
    }
    observation.ready = true;
    if (outcome->kind == Kind::Ok) { observation.result = false; return observation; }
    if (outcome->kind != Kind::Error) return observation;
    if (const auto expected = transcript.unknown_auth_token_alias_compatibility_code)
        observation.result = outcome->code == *expected;
    return observation;
}

Observation compare_alias_outcomes(const RawProbeTranscript& transcript, bool require_rejected_registration) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    const auto registered = write_outcome(*streams, transcript.writes[0]);
    const auto used = write_outcome(*streams, transcript.writes[1]);
    if (!registered || !used) {
        if (registered && application_close_code(transcript.events)) {
            observation.ready = true;
            observation.result = false;
        }
        return observation;
    }
    observation.ready = true;
    if ((registered->kind != Kind::Ok && registered->kind != Kind::Error) ||
        (used->kind != Kind::Ok && used->kind != Kind::Error)) return observation;
    if (require_rejected_registration && registered->kind != Kind::Error) return observation;
    const auto unknown = transcript.unknown_auth_token_alias_compatibility_code;
    if (used->kind == Kind::Error && unknown && used->code == *unknown && registered->code != *unknown) {
        observation.result = false;
        return observation;
    }
    if (registered->kind == Kind::Ok) {
        // The alias stands for the same token, so the same request succeeds.
        observation.result = used->kind == Kind::Ok;
        return observation;
    }
    if (used->kind == Kind::Ok || used->code == registered->code) observation.result = true;
    return observation;
}

RawProbeDefinition register_then_use_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    auto definition = make_definition("register-valid-token-then-use-alias-in-later-request",
        setup_frame(), deadline);
    definition.writes.push_back(register_write(fixture));
    definition.writes.push_back(token_write(3, fixture, d18::TokenAliasType::UseAlias, 0));
    return definition;
}

Observation register_then_use_observe(const RawProbeTranscript& transcript, const Fixture&) {
    return compare_alias_outcomes(transcript, false);
}

RawProbeDefinition rejected_register_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    const auto rejected = unknown_track_fixture(fixture);
    auto definition = make_definition("register-token-in-rejected-request-then-use-alias",
        setup_frame(), deadline);
    definition.writes.push_back(register_write(rejected));
    definition.writes.push_back(token_write(3, rejected, d18::TokenAliasType::UseAlias, 0));
    return definition;
}

Observation rejected_register_observe(const RawProbeTranscript& transcript, const Fixture&) {
    return compare_alias_outcomes(transcript, true);
}

// ---- publisher-sent Message Parameter encoding -----------------------------------
// The publisher's own parameter blocks are inspected: TRACK_STATUS_OK (what a
// SUBSCRIBE_OK would carry, Section 10.14) and PUBLISH (Section 10.10).
enum class ParameterRule { Ascending, NoRepeats };

constexpr std::string_view kDuplicateDetail = "duplicate non-repeatable parameter";
constexpr std::string_view kOverflowDetail = "parameter resolved type overflows uint64";

RawProbeDefinition parameter_definition(std::string id, const Fixture& fixture,
                                        std::chrono::milliseconds deadline) {
    auto definition = make_definition(std::move(id), setup_frame(), deadline);
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi, track_status_request(1, fixture), true));
    definition.writes.push_back(make_write(RawProbeChannel::NewBidi,
        subscribe_tracks_request(3, fixture.track_namespace, {forward_parameter(0)})));
    return definition;
}

Observation parameter_observe(const RawProbeTranscript& transcript, ParameterRule rule) {
    Observation observation;
    if (transcript.writes.size() != 2) return observation;
    const auto streams = collect_streams(transcript.events);
    if (!streams) return observation;
    std::vector<std::pair<const Bytes*, Frame>> frames;
    const auto* status = local_stream(*streams, transcript.writes[0]);
    bool status_seen = false;
    if (status) {
        const auto split = split_frames(status->bytes);
        if (!split.malformed && !split.frames.empty()) {
            frames.emplace_back(&status->bytes, split.frames.front());
            status_seen = true;
        }
    }
    bool publish_seen = false;
    for (const auto& [id, stream] : *streams) {
        if (!is_peer_bidi(id)) continue;
        const auto split = split_frames(stream.bytes);
        if (split.malformed || split.frames.empty() || split.frames.front().type != 0x1d) continue;
        frames.emplace_back(&stream.bytes, split.frames.front());
        publish_seen = true;
    }
    bool multiple = false;
    for (const auto& [bytes, frame] : frames) {
        const auto message = decode_frame(*bytes, frame);
        if (message) {
            const d18::Parameters* parameters = nullptr;
            if (const auto* value = std::get_if<d18::RequestOkMessage>(&*message)) parameters = &value->parameters;
            else if (const auto* publish = std::get_if<d18::PublishMessage>(&*message)) parameters = &publish->parameters;
            if (parameters && parameters->size() >= 2) multiple = true;
            continue;
        }
        const auto detail = frame_decode_error(*bytes, frame);
        if (detail && ((rule == ParameterRule::NoRepeats && *detail == kDuplicateDetail) ||
                       (rule == ParameterRule::Ascending && *detail == kOverflowDetail))) {
            observation.ready = true;
            observation.result = false;
            return observation;
        }
    }
    // A pass needs a block with several parameters; fewer proves nothing.
    if (multiple) {
        observation.ready = true;
        observation.result = true;
    } else if (status_seen && publish_seen) {
        observation.ready = true;
    }
    return observation;
}

RawProbeDefinition ordered_parameters_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    return parameter_definition("publish-with-multiple-message-parameter-types", fixture, deadline);
}
Observation ordered_parameters_observe(const RawProbeTranscript& transcript, const Fixture&) {
    return parameter_observe(transcript, ParameterRule::Ascending);
}
RawProbeDefinition unique_parameters_definition(const Fixture& fixture, std::chrono::milliseconds deadline) {
    return parameter_definition("publish-with-multiple-configured-parameters", fixture, deadline);
}
Observation unique_parameters_observe(const RawProbeTranscript& transcript, const Fixture&) {
    return parameter_observe(transcript, ParameterRule::NoRepeats);
}

}  // namespace

std::vector<Entry> setup_entries() {
    std::vector<Entry> entries;
    entries.push_back({"D18-10-3-MUST-001", "receive-setup-with-unknown-option",
        "setup-continues-with-unknown-options-ignored", true, false, FirstWrite::Subscribe,
        unknown_option_definition, unknown_option_observe});
    // The catalog names the same scenario for D18-10-3-MUST-002.
    entries.push_back({"D18-10-3-MUST-002", "receive-setup-with-unknown-option",
        "setup-continues-with-unknown-options-ignored", true, false, FirstWrite::Subscribe,
        unknown_option_definition, unknown_option_observe});
    entries.push_back({"D18-3-3-MUST-NOT-002", "complete-publisher-requests-while-session-remains-open",
        "control-stream-not-closed-during-session", true, false, FirstWrite::TrackStatus,
        control_open_definition, control_open_observe});
    entries.push_back({"D18-3-1-MUST-001", "establish-moqt-with-datagram-capable-peer",
        "quic-datagram-extension-negotiated", false, false, std::nullopt,
        datagram_definition, datagram_observe});
    entries.push_back({"D18-3-2-MUST-001", "native-quic-publisher-client-setup-from-moqt-uri",
        "setup-includes-required-authority-and-path-options", false, true, std::nullopt,
        native_setup_definition, native_setup_observe});
    entries.push_back({"D18-10-2-2-MUST-001", "register-delete-then-use-token-alias",
        "deleted-token-alias-rejected-as-unknown", true, false, FirstWrite::TrackStatus,
        delete_then_use_definition, delete_then_use_observe});
    entries.push_back({"D18-10-2-2-MUST-002", "register-valid-token-then-use-alias-in-later-request",
        "registered-alias-resolves-original-token", true, false, FirstWrite::TrackStatus,
        register_then_use_definition, register_then_use_observe});
    entries.push_back({"D18-10-2-2-MUST-009", "register-token-in-rejected-request-then-use-alias",
        "rejected-request-token-alias-still-registered", true, false, FirstWrite::TrackStatus,
        rejected_register_definition, rejected_register_observe});
    entries.push_back({"D18-10-2-MUST-001", "publish-with-multiple-message-parameter-types",
        "message-parameters-encoded-in-ascending-type-order", true, false, FirstWrite::TrackStatus,
        ordered_parameters_definition, ordered_parameters_observe});
    entries.push_back({"D18-10-2-MUST-NOT-001", "publish-with-multiple-configured-parameters",
        "no-unpermitted-duplicate-parameter-types-sent", true, false, FirstWrite::TrackStatus,
        unique_parameters_definition, unique_parameters_observe});
    return entries;
}

}  // namespace moq::interop::scenarios::gap_a
