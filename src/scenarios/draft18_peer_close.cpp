#include "moq/interop/scenarios/draft18_peer_close.h"
#include "moq/interop/wire/draft18/messages.h"

#include <utility>

namespace moq::interop::scenarios {
namespace {
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Control,cursor,{});
    const auto* message = std::get_if<wire::draft18::Message>(&decoded);
    return message && std::holds_alternative<wire::draft18::SetupMessage>(*message);
}
template<class Opening>
bool request_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request,cursor,{});
    const auto* message = std::get_if<wire::draft18::Message>(&decoded);
    const auto* opening = message ? std::get_if<Opening>(message) : nullptr;
    // Section 10.1: the publisher is the client and generates even Request IDs.
    return opening && cursor.remaining() == 0 && (opening->request_id & 1u) == 0;
}
template<class Opening>
Draft18PeerCloseProbe profile(std::string requirement, std::string scenario,
                              std::vector<std::byte> response,
                              std::chrono::milliseconds deadline) {
    RawProbeDefinition definition;
    definition.id = std::move(scenario);
    definition.setup_bytes = bytes({0xaf,0,0,0});
    definition.writes.push_back({RawProbeChannel::PeerBidi,std::move(response),false});
    definition.peer_setup_ready = setup_ready;
    definition.peer_request_ready = request_ready<Opening>;
    definition.deadline = deadline;
    return {std::move(requirement),"session-closed-protocol-violation",3,std::move(definition)};
}
}  // namespace

std::vector<Draft18PeerCloseProbe> draft18_peer_close_probes(std::chrono::milliseconds deadline) {
    using wire::draft18::PublishMessage;
    using wire::draft18::PublishNamespaceMessage;
    using wire::draft18::TrackStatusMessage;
    // Sections 1.4.4 and 10.10: UNINTERESTED with a complete 1025-byte reason.
    // The framed payload is 1029 bytes; the reason length alone exceeds its limit.
    auto oversized_reason = bytes({5,4,5,0x20,0,0x84,1});
    oversized_reason.insert(oversized_reason.end(),1025,std::byte{'a'});
    std::vector<Draft18PeerCloseProbe> result;
    result.push_back(profile<PublishMessage>("D18-1-4-4-MUST-001",
        "receive-reason-phrase-length-over-1024",std::move(oversized_reason),deadline));
    // Section 10.5: REQUEST_OK carries no Track Properties in these contexts.
    result.push_back(profile<PublishMessage>("D18-10-5-MUST-001",
        "receive-publish-request-ok-with-track-properties",bytes({7,0,3,0,0x22,1}),deadline));
    result.push_back(profile<PublishNamespaceMessage>("D18-10-5-MUST-004",
        "receive-publish-namespace-ok-with-track-properties",bytes({7,0,3,0,0x22,1}),deadline));
    // Establish PUBLISH before responding to a later publisher update. The
    // narrow gate accepts a complete parameterless update with a fresh client ID.
    auto update = profile<PublishMessage>("D18-10-5-MUST-002",
        "receive-request-update-ok-with-track-properties",bytes({7,0,1,0}),deadline);
    update.definition.peer_request_ready = [](std::span<const std::byte> input) {
        wire::Cursor cursor(input);
        const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<wire::draft18::Message>(&decoded);
        const auto* publish = message ? std::get_if<PublishMessage>(message) : nullptr;
        return publish && publish->request_id == 0 && cursor.remaining() == 0;
    };
    RawProbeWrite reply{RawProbeChannel::PeerBidi,bytes({7,0,3,0,0x22,1}),false};
    reply.reuse_write_stream = 0;
    reply.peer_response_ready = [](std::span<const std::byte> input) {
        wire::Cursor cursor(input);
        const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<wire::draft18::Message>(&decoded);
        const auto* request = message ? std::get_if<wire::draft18::RequestUpdateMessage>(message) : nullptr;
        return request && request->request_id > 0 && request->request_id % 2 == 0 &&
            request->parameters.empty() && cursor.remaining() == 0;
    };
    update.definition.writes.push_back(std::move(reply));
    result.push_back(std::move(update));
    // Section 10.6.1: URI empty, valid namespace, forbidden namespace Track Name.
    result.push_back(profile<PublishNamespaceMessage>("D18-10-6-1-MUST-005",
        "receive-publish-namespace-redirect-with-nonempty-track-name",
        bytes({5,0,9,0x34,0,0,0,1,1,'n',1,'x'}),deadline));
    // Recovery queries are optional publisher behavior (2.4.3). Each definition
    // waits for an actual TRACK_STATUS; absence of that opener cannot prove a row.
    result.push_back(profile<TrackStatusMessage>("D18-12-5-MUST-001",
        "publisher-recovery-track-status-reply-with-invalid-default-group-order",
        bytes({7,0,3,0,0x22,3}),deadline));
    result.push_back(profile<TrackStatusMessage>("D18-12-6-MUST-001",
        "publisher-recovery-track-status-reply-with-dynamic-groups-two",
        bytes({7,0,3,0,0x30,2}),deadline));
    // The catalog intentionally gives 12.5 and mutable lookup the same scenario.
    result.push_back(profile<TrackStatusMessage>("D18-12-7-MUST-004",
        "publisher-recovery-track-status-reply-with-invalid-default-group-order",
        bytes({7,0,3,0,0x22,3}),deadline));
    result.push_back(profile<TrackStatusMessage>("D18-12-7-MUST-005",
        "publisher-recovery-track-status-reply-with-invalid-default-group-order-in-immutable-wrapper",
        bytes({7,0,5,0,0xb,2,0x22,3}),deadline));
    return result;
}
}  // namespace moq::interop::scenarios
