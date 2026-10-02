#pragma once

#include "moq/interop/scenarios/raw_probe.h"

#include <chrono>
#include <deque>
#include <thread>
#include <functional>
#include <map>
#include <set>

namespace moq::interop::test {

// Scripted publisher peer driving a real RawProbeController through a fake
// transport, so a probe's gating, stimulus proof and evaluation run end to end.
class ScriptTransport : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, next_bidi += 4}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, next_uni += 4}; }
    transport::OperationResult write(transport::StreamId id, std::span<const std::byte> bytes, bool fin) override {
        output[id].insert(output[id].end(), bytes.begin(), bytes.end());
        if (fin) fins.insert(id);
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override { return {transport::TransportStatus::Success, 0, {}}; }
    transport::OperationResult stop_sending(transport::StreamId id, std::uint64_t code) override {
        stops.emplace_back(id, code);
        return {transport::TransportStatus::Success, 0, {}};
    }
    transport::OperationResult send_datagram(std::span<const std::byte> bytes) override {
        datagrams.emplace_back(bytes.begin(), bytes.end());
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override {
        return {transport::TransportStatus::Success, 0, {}};
    }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        auto result = std::move(events);
        events.clear();
        return result;
    }

    std::uint64_t next_bidi = static_cast<std::uint64_t>(-3);
    std::uint64_t next_uni = static_cast<std::uint64_t>(-1);
    std::map<transport::StreamId, std::vector<std::byte>> output;
    std::set<transport::StreamId> fins;
    std::vector<std::vector<std::byte>> datagrams;
    std::vector<std::pair<transport::StreamId, std::uint64_t>> stops;
    std::vector<transport::TransportEvent> events;
};

inline std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
inline std::vector<std::byte> concat(std::vector<std::byte> first, const std::vector<std::byte>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

struct PeerView {
    ScriptTransport& transport;
    std::size_t step;
    std::set<std::string>& fired;
    // Runs the action once, the first time its condition holds.
    void when(const std::string& key, bool condition, const std::function<void()>& action) {
        if (condition && fired.insert(key).second) action();
    }
    void push(transport::TransportEvent event) { transport.events.push_back(std::move(event)); }
    void data(transport::StreamId id, std::vector<std::byte> bytes, bool fin = false) {
        push(transport::StreamDataEvent{id, std::move(bytes), fin});
    }
    bool sent(transport::StreamId id) const {
        const auto found = transport.output.find(id);
        return found != transport.output.end() && !found->second.empty();
    }
};

// Pushes the connection and runs `peer` between controller polls. Returns the
// transcript when the controller completes, times out or fails.
inline scenarios::RawProbeTranscript drive_probe(
    const scenarios::RawProbeDefinition& definition,
    const std::function<void(PeerView&)>& peer, std::size_t datagram_capacity = 1200,
    std::size_t maximum_steps = 400) {
    ScriptTransport transport;
    transport.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, datagram_capacity});
    scenarios::RawProbeController controller(transport, definition);
    std::set<std::string> fired;
    const auto start = scenarios::RawProbeClock::now();
    for (std::size_t step = 0; step < maximum_steps; ++step) {
        PeerView view{transport, step, fired};
        peer(view);
        const auto& transcript = controller.poll(start + std::chrono::milliseconds(step));
        if (transcript.complete || transcript.timed_out || transcript.harness_failed) break;
        // Probes that wait out a quiet window read the real clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return controller.transcript();
}

}  // namespace moq::interop::test
