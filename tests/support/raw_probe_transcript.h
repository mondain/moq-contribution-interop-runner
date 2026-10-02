#pragma once

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::test {

inline std::vector<std::byte> probe_bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

inline scenarios::RawProbeTranscript raw_probe_transcript(
    const scenarios::RawProbeDefinition& definition,
    std::span<const std::byte> peer_request = {}) {
    scenarios::RawProbeTranscript result;
    result.scenario_id = definition.id;
    result.setup = {{scenarios::RawProbeChannel::NewUni, definition.setup_bytes, false},
                    3, definition.setup_bytes.size(), false};
    result.transport_established = result.peer_setup_received = true;
    result.stimulus_delivered = result.complete = true;
    result.max_datagram_payload = 1200;
    std::uint64_t bidi = 1;
    std::uint64_t uni = 7;
    for (const auto& write : definition.writes) {
        std::optional<transport::StreamId> id;
        if (write.channel == scenarios::RawProbeChannel::Control) id = 3;
        else if (write.channel == scenarios::RawProbeChannel::PeerBidi) id = 0;
        else if (write.channel == scenarios::RawProbeChannel::NewBidi) { id = bidi; bidi += 4; }
        else if (write.channel == scenarios::RawProbeChannel::NewUni) { id = uni; uni += 4; }
        result.writes.push_back({write, id, write.bytes.size(), write.fin});
    }
    // Literal SETUP fixtures advertise optional receiver capacities. Their
    // parameter encodings are shared by drafts 18 and 21.
    const std::vector fixtures{
        probe_bytes({0xaf, 0, 0, 0}),
        probe_bytes({0xaf, 0, 0, 2, 4, 32}),
        probe_bytes({0xaf, 0, 0, 2, 4, 34}),
        probe_bytes({0xaf, 0, 0, 2, 6, 2})};
    auto peer_setup = fixtures.front();
    for (const auto& fixture : fixtures) {
        if (definition.peer_setup_ready && definition.peer_setup_ready(fixture)) {
            peer_setup = fixture;
            break;
        }
    }
    result.events = {transport::ConnectionEstablishedEvent{{}, {}, {}, 1200},
                    transport::StreamDataEvent{2, std::move(peer_setup), false}};
    if (!peer_request.empty())
        result.events.push_back(transport::StreamDataEvent{0, {peer_request.begin(), peer_request.end()}, false});
    result.delivery_event_count = result.events.size();
    return result;
}

}  // namespace moq::interop::test
