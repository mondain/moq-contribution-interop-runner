#include "moq/interop/scenarios/draft18_close.h"
#include "moq/interop/wire/draft18/messages.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
constexpr std::array<Draft18CloseProfile, 39> kProfiles{{
    {"D18-11-4-2-MUST-002", "receive-subgroup-types-with-subgroup-id-mode-three", "session-closed-protocol-violation", 3, false, false},
    {"D18-11-4-2-MUST-003", "receive-invalid-subgroup-header-type-0x80", "session-closed-protocol-violation", 3, false, false},
    {"D18-11-MUST-001", "receive-unknown-datagram-type", "session-closed", std::nullopt, false, false},
    {"D18-11-3-1-MUST-002", "receive-datagram-types-0x22-0x23-0x26-0x27-0x2a-0x2b-0x2e-0x2f", "session-closed-protocol-violation", 3, false, false},
    {"D18-11-3-1-MUST-003", "receive-invalid-object-datagram-type-0x10", "session-closed-protocol-violation", 3, false, false},
    {"D18-1-4-3-MUST-001", "receive-key-value-type-overflow", "session-closed-protocol-violation", 3, false, false},
    {"D18-1-4-3-MUST-002", "receive-key-value-length-over-65535", "session-closed-protocol-violation", 3, false, false},
    {"D18-1-4-3-MUST-003", "receive-understood-key-value-invalid-serialization", "session-closed-key-value-formatting-error", 6, false, false},
    {"D18-2-4-1-MUST-002", "receive-zero-length-namespace-field", "session-closed-protocol-violation", 3, false, false},
    {"D18-2-4-1-MUST-003", "receive-namespace-with-33-fields", "session-closed-protocol-violation", 3, false, false},
    {"D18-2-4-1-MUST-004", "receive-track-namespace-over-4096-bytes", "session-closed-protocol-violation", 3, false, false},
    {"D18-2-4-1-MUST-005", "receive-full-track-name-over-4096-bytes", "session-closed-protocol-violation", 3, false, false},
    {"D18-3-3-MUST-001", "receive-disallowed-initial-bidirectional-message", "session-closed-protocol-violation", 3, false, false},
    {"D18-3-4-MUST-001", "receive-unknown-unidirectional-stream-type", "session-closed", std::nullopt, false, false},
    {"D18-5-1-2-MUST-002", "receive-undefined-subscription-filter-type", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-MUST-008", "receive-unknown-message-type", "session-closed", std::nullopt, false, false},
    {"D18-10-MUST-009", "receive-known-message-with-mismatched-payload-length", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-1-MUST-001", "receive-request-id-wrong-peer-parity", "session-closed-invalid-request-id", 4, false, false},
    {"D18-10-1-MUST-002", "receive-duplicate-request-id-across-request-streams", "session-closed-invalid-request-id", 4, false, false},
    {"D18-10-2-MUST-002", "receive-message-parameter-type-delta-overflow", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-2-MUST-004", "receive-unnegotiated-unknown-message-parameter", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-2-SHOULD-002", "receive-duplicate-nonrepeatable-message-parameter", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-2-1-MUST-001", "receive-message-parameter-on-disallowed-message-native-quic", "connection-closed-protocol-violation", 3, false, true},
    {"D18-10-2-2-MUST-005", "receive-undecodable-authorization-token-structure", "session-closed-key-value-formatting-error", 6, false, false},
    {"D18-10-2-8-MUST-001", "receive-group-order-zero-or-greater-than-two", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-2-12-MUST-001", "receive-forward-outside-zero-one", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-3-1-1-MUST-001", "receive-server-setup-with-authority", "session-closed-invalid-authority", 25, false, false},
    {"D18-10-3-1-1-MUST-002", "receive-webtransport-setup-with-authority", "session-closed-invalid-authority", 25, true, false},
    {"D18-10-3-1-2-MUST-001", "receive-server-setup-with-path", "session-closed-invalid-path", 8, false, false},
    {"D18-10-3-1-2-MUST-002", "receive-webtransport-setup-with-path", "session-closed-invalid-path", 8, true, false},
    {"D18-10-4-MUST-002", "receive-two-goaways-on-control-stream", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-4-MUST-005", "receive-goaway-uri-length-8193", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-4-MUST-007", "receive-control-goaway-with-wrong-receiver-request-id-parity", "session-closed-invalid-request-id", 4, false, false},
    {"D18-10-12-MUST-001", "receive-fetch-with-unknown-type", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-18-MUST-001", "receive-subscribe-namespace-with-33-prefix-fields", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-19-MUST-001", "receive-subscribe-tracks-with-33-prefix-fields", "session-closed-protocol-violation", 3, false, false},
    {"D18-5-1-2-MUST-001", "absolute-range-end-group-overflow", "session-closed-protocol-violation", 3, false, false},
    {"D18-10-2-2-MUST-006", "register-same-peer-token-alias-twice-without-delete", "session-closed-duplicate-auth-token-alias", 0x14, false, false},
    {"D18-10-2-2-MUST-011", "register-request-token-exceeding-advertised-cache-size", "session-closed-auth-token-cache-overflow", 0x13, false, false},
}};
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
void vi(std::vector<std::byte>& output, std::uint64_t value) {
    wire::ByteWriter writer(16);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("varint failed");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}
std::vector<std::byte> frame(std::uint64_t type, const std::vector<std::byte>& payload) {
    if (payload.size() > 65535) throw std::logic_error("raw probe payload too large");
    std::vector<std::byte> output;
    vi(output, type);
    output.push_back(static_cast<std::byte>(payload.size() >> 8));
    output.push_back(static_cast<std::byte>(payload.size() & 255));
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}
bool peer_setup(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Control, cursor, {});
    const auto* message = std::get_if<wire::draft18::Message>(&decoded);
    return message && std::holds_alternative<wire::draft18::SetupMessage>(*message);
}
bool peer_cache_setup(std::span<const std::byte> input, bool duplicate) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Control,cursor,{});
    const auto* message = std::get_if<wire::draft18::Message>(&decoded);
    const auto* setup = message ? std::get_if<wire::draft18::SetupMessage>(message) : nullptr;
    if (!setup) return false;
    std::uint64_t capacity = 0;
    bool capacity_seen = false;
    for (const auto& option : setup->options) {
        if (option.type != 4) continue;
        const auto* value = std::get_if<wire::draft18::VarIntValue>(&option.value);
        if (!value || capacity_seen) return false;
        capacity_seen = true;
        capacity = value->value;
    }
    // Each one-byte Token Value costs 17 bytes. Allow room for both
    // duplicate registrations so the close code cannot be a cache overflow.
    return duplicate ? capacity >= 34 : capacity < 17;
}
}
std::span<const Draft18CloseProfile> draft18_close_profiles() { return kProfiles; }
RawProbeDefinition draft18_close_probe(std::string_view id, std::chrono::milliseconds deadline,
                                      std::vector<std::vector<std::byte>> track_namespace,
                                      std::vector<std::byte> track_name) {
    if (std::none_of(kProfiles.begin(), kProfiles.end(), [id](const auto& profile) { return profile.scenario_id == id; }))
        throw std::invalid_argument("unknown draft-18 close probe");
    RawProbeDefinition definition;
    definition.id = id;
    definition.deadline = deadline;
    definition.setup_bytes = frame(0x2f00, {});
    definition.peer_setup_ready = peer_setup;
    definition.acknowledge_publisher_namespace = true;
    auto payload = bytes({1,1,1,'n',1,'x',0});
    RawProbeChannel channel = RawProbeChannel::NewBidi;
    std::uint64_t type = 3;
    if (id == "register-same-peer-token-alias-twice-without-delete" ||
        id == "register-request-token-exceeding-advertised-cache-size") {
        const bool duplicate = id == "register-same-peer-token-alias-twice-without-delete";
        definition.peer_setup_ready = [duplicate](std::span<const std::byte> input) {
            return peer_cache_setup(input,duplicate);
        };
        // A GREASE Token Type has an opaque value and needs no application
        // token-type negotiation. REGISTER persists even on request rejection.
        payload = bytes({1,1,1,'n',1,'x',1,3,5,1,7,0x80,0x9d,'x'});
        definition.writes.push_back({RawProbeChannel::NewBidi,frame(3,payload),false});
        if (duplicate) {
            payload.front() = std::byte{3};
            definition.writes.push_back({RawProbeChannel::NewBidi,frame(3,payload),false});
        }
        return definition;
    }
    if (id == "receive-subgroup-types-with-subgroup-id-mode-three" ||
        id == "receive-invalid-subgroup-header-type-0x80") {
        definition.writes.push_back({RawProbeChannel::NewUni,
            id == "receive-subgroup-types-with-subgroup-id-mode-three" ? bytes({0x16}) : bytes({0x80,0x80}),false});
        return definition;
    }
    if (id == "receive-unknown-datagram-type" || id == "receive-invalid-object-datagram-type-0x10" ||
        id == "receive-datagram-types-0x22-0x23-0x26-0x27-0x2a-0x2b-0x2e-0x2f") {
        if (id == "receive-datagram-types-0x22-0x23-0x26-0x27-0x2a-0x2b-0x2e-0x2f") {
            for (auto invalid_type : {0x22u,0x23u,0x26u,0x27u,0x2au,0x2bu,0x2eu,0x2fu})
                definition.writes.push_back({RawProbeChannel::Datagram,bytes({invalid_type}),false});
        } else definition.writes.push_back({RawProbeChannel::Datagram,
            id == "receive-unknown-datagram-type" ? bytes({0x40}) : bytes({0x10}),false});
        return definition;
    }
    if (id == "receive-server-setup-with-authority" || id == "receive-webtransport-setup-with-authority") {
        definition.setup_bytes = frame(0x2f00, bytes({5,1,'a'}));
        definition.start_after_peer_setup = false;
    } else if (id == "receive-server-setup-with-path" || id == "receive-webtransport-setup-with-path") {
        definition.setup_bytes = frame(0x2f00, bytes({1,1,'/'}));
        definition.start_after_peer_setup = false;
    } else if (id == "receive-key-value-type-overflow") {
        payload.clear(); vi(payload, std::numeric_limits<std::uint64_t>::max());
        payload.insert(payload.end(), {std::byte{0},std::byte{1},std::byte{0}});
        definition.setup_bytes = frame(0x2f00, payload);
        definition.start_after_peer_setup = false;
    } else if (id == "receive-key-value-length-over-65535") {
        payload = bytes({9}); vi(payload, 65536);
        definition.setup_bytes = frame(0x2f00, payload);
        definition.start_after_peer_setup = false;
    } else if (id == "receive-understood-key-value-invalid-serialization") {
        definition.setup_bytes = frame(0x2f00, bytes({3,1,4}));
        definition.start_after_peer_setup = false;
    } else if (id == "receive-unknown-unidirectional-stream-type") {
        definition.writes.push_back({RawProbeChannel::NewUni, bytes({0}), false});
        return definition;
    } else if (id == "receive-unknown-message-type") {
        type = 0x3f; payload.clear(); channel = RawProbeChannel::Control;
    } else if (id == "receive-disallowed-initial-bidirectional-message") {
        type = 7; payload = bytes({0});
    } else if (id == "receive-known-message-with-mismatched-payload-length") {
        payload.push_back(std::byte{0});
    } else if (id == "receive-zero-length-namespace-field") {
        payload = bytes({1,1,0,1,'x',0});
    } else if (id == "receive-namespace-with-33-fields") {
        payload = bytes({1,33});
        for (unsigned i=0; i<33; ++i) payload.insert(payload.end(), {std::byte{1},std::byte{'n'}});
        payload.insert(payload.end(), {std::byte{1},std::byte{'x'},std::byte{0}});
    } else if (id == "receive-subscribe-namespace-with-33-prefix-fields" ||
               id == "receive-subscribe-tracks-with-33-prefix-fields") {
        type = id == "receive-subscribe-namespace-with-33-prefix-fields" ? 0x50 : 0x51;
        payload = bytes({1,33});
        for (unsigned i=0;i<33;++i) payload.insert(payload.end(),{std::byte{1},std::byte{'n'}});
        payload.push_back(std::byte{0});
    } else if (id == "receive-track-namespace-over-4096-bytes") {
        payload = bytes({1,1}); vi(payload,4097);
        payload.insert(payload.end(),4097,std::byte{'n'});
        payload.insert(payload.end(),{std::byte{1},std::byte{'x'},std::byte{0}});
    } else if (id == "receive-full-track-name-over-4096-bytes") {
        payload = bytes({1,1,1,'n'}); vi(payload,4096);
        payload.insert(payload.end(),4096,std::byte{'x'}); payload.push_back(std::byte{0});
    } else if (id == "receive-request-id-wrong-peer-parity") {
        payload.front() = std::byte{0};
    } else if (id == "receive-duplicate-request-id-across-request-streams") {
        // Section 10.1 needs the first SUBSCRIBE to be otherwise acceptable; a
        // publisher may reject or abort over an unknown track before it ever
        // sees the repeated Request ID.
        if (!track_namespace.empty() && !track_name.empty()) {
            payload = bytes({1});
            vi(payload,track_namespace.size());
            for (const auto& field : track_namespace) {
                vi(payload,field.size());
                payload.insert(payload.end(),field.begin(),field.end());
            }
            vi(payload,track_name.size());
            payload.insert(payload.end(),track_name.begin(),track_name.end());
            payload.push_back(std::byte{0});
        }
        definition.writes.push_back({channel,frame(type,payload),false});
    } else if (id == "receive-fetch-with-unknown-type") {
        type=0x16; payload=bytes({1,0,0});
    } else if (id == "receive-two-goaways-on-control-stream") {
        auto one = frame(0x10,bytes({0,0,0}));
        auto two = one; two.insert(two.end(),one.begin(),one.end());
        definition.writes.push_back({RawProbeChannel::Control,std::move(two),false});
        return definition;
    } else if (id == "receive-goaway-uri-length-8193") {
        type=0x10; channel=RawProbeChannel::Control; payload.clear(); vi(payload,8193);
        payload.insert(payload.end(),8193,std::byte{'x'}); payload.insert(payload.end(),{std::byte{0},std::byte{0}});
    } else if (id == "receive-control-goaway-with-wrong-receiver-request-id-parity") {
        type=0x10; channel=RawProbeChannel::Control; payload=bytes({0,0,1});
    } else {
        payload.back()=std::byte{1};
        if (id == "receive-message-parameter-type-delta-overflow") {
            payload.back()=std::byte{2};
            payload.insert(payload.end(),{std::byte{2},std::byte{0}});
            vi(payload,std::numeric_limits<std::uint64_t>::max());
        }
        else if (id == "receive-unnegotiated-unknown-message-parameter") {
            payload.insert(payload.end(),{std::byte{0x3f},std::byte{0}});
        }
        else if (id == "receive-duplicate-nonrepeatable-message-parameter") {
            payload.back()=std::byte{2}; auto p=bytes({0x10,1,0,1}); payload.insert(payload.end(),p.begin(),p.end());
        } else if (id == "receive-message-parameter-on-disallowed-message-native-quic") { auto p=bytes({8,0}); payload.insert(payload.end(),p.begin(),p.end()); }
        else if (id == "receive-undecodable-authorization-token-structure") { auto p=bytes({3,0}); payload.insert(payload.end(),p.begin(),p.end()); }
        else if (id == "receive-group-order-zero-or-greater-than-two") { auto p=bytes({0x22,0}); payload.insert(payload.end(),p.begin(),p.end()); }
        else if (id == "receive-forward-outside-zero-one") { auto p=bytes({0x10,2}); payload.insert(payload.end(),p.begin(),p.end()); }
        else if (id == "receive-undefined-subscription-filter-type") { auto p=bytes({0x21,1,0}); payload.insert(payload.end(),p.begin(),p.end()); }
        else if (id == "absolute-range-end-group-overflow") {
            auto filter = bytes({4});
            vi(filter,std::numeric_limits<std::uint64_t>::max());
            filter.insert(filter.end(),{std::byte{0},std::byte{1}});
            payload.push_back(std::byte{0x21});
            vi(payload,filter.size());
            payload.insert(payload.end(),filter.begin(),filter.end());
        }
        else throw std::logic_error("close profile has no stimulus");
    }
    if (definition.start_after_peer_setup) definition.writes.push_back({channel,frame(type,payload),false});
    return definition;
}
RawProbeDefinition draft18_close_probe_for(std::string_view id, const RawProbeTranscript& transcript,
                                          std::chrono::milliseconds deadline) {
    if (id == "receive-duplicate-request-id-across-request-streams" && !transcript.writes.empty()) {
        wire::Cursor cursor(transcript.writes.front().write.bytes);
        const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<wire::draft18::Message>(&decoded);
        const auto* subscribe = message ? std::get_if<wire::draft18::SubscribeMessage>(message) : nullptr;
        if (subscribe)
            return draft18_close_probe(id,deadline,subscribe->track_namespace.fields,subscribe->track_name.bytes);
    }
    return draft18_close_probe(id,deadline);
}
}  // namespace moq::interop::scenarios
