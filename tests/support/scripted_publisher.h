#pragma once

// A scripted MOQT publisher peer for exercising raw probes end to end: the
// real RawProbeController drives this fake transport, and a reaction callback
// answers what the probe wrote. Streams follow QUIC numbering with the probe
// acting as the server: local bidi 1,5,9..., local uni 3,7,..., peer bidi
// 0,4,8..., peer uni 2,6,10... (stream 2 is the peer control stream).

#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/draft18/messages.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

namespace moq::interop::test {

using Bytes = std::vector<std::byte>;

inline Bytes encode_draft18(const wire::draft18::Message& message) {
    wire::ByteWriter output(65546);
    if (!wire::draft18::encode_message(message, output).has_value())
        throw std::invalid_argument("unencodable test message");
    return {output.bytes().begin(), output.bytes().end()};
}

inline Bytes bytes_of(std::string_view text) {
    Bytes result;
    for (const auto c : text) result.push_back(static_cast<std::byte>(c));
    return result;
}

inline Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

struct Sent {
    Bytes bytes;
    bool fin{false};
    std::size_t writes{0};
    std::optional<std::uint64_t> stop_sending;
};

class ScriptedPublisher : public transport::SessionTransport {
public:
    using Reaction = std::function<void(ScriptedPublisher&)>;

    explicit ScriptedPublisher(Bytes peer_setup, Reaction reaction = {})
        : peer_setup_(std::move(peer_setup)), reaction_(std::move(reaction)) {}

    transport::OpenResult open_bidi() override {
        const auto id = next_bidi_;
        next_bidi_ += 4;
        return {transport::TransportStatus::Success, id};
    }
    transport::OpenResult open_uni() override {
        const auto id = next_uni_;
        next_uni_ += 4;
        return {transport::TransportStatus::Success, id};
    }
    transport::OperationResult write(transport::StreamId id, std::span<const std::byte> bytes,
                                     bool fin) override {
        auto& sent = sent_[id];
        sent.bytes.insert(sent.bytes.end(), bytes.begin(), bytes.end());
        sent.fin = sent.fin || fin;
        ++sent.writes;
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override {
        return {transport::TransportStatus::Success, 0, {}};
    }
    transport::OperationResult stop_sending(transport::StreamId id, std::uint64_t code) override {
        sent_[id].stop_sending = code;
        return {transport::TransportStatus::Success, 0, {}};
    }
    transport::OperationResult send_datagram(std::span<const std::byte> bytes) override {
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult grant_peer_streams(bool bidirectional, std::uint64_t additional) override {
        (bidirectional ? granted_bidi_ : granted_uni_) += additional;
        return {transport::TransportStatus::Success, 0, {}};
    }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override {
        return {transport::TransportStatus::Success, 0, {}};
    }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        if (!started_) {
            started_ = true;
            transport::ConnectionEstablishedEvent established;
            established.max_datagram_payload = datagram_payload;
            events_.push_back(std::move(established));
            if (!peer_setup_.empty()) data(2, peer_setup_);
        } else if (reaction_) {
            reaction_(*this);
        }
        auto result = std::move(events_);
        events_.clear();
        return result;
    }

    // Peer-to-probe events.
    void data(transport::StreamId id, Bytes bytes, bool fin = false) {
        events_.push_back(transport::StreamDataEvent{id, std::move(bytes), fin});
    }
    void fin(transport::StreamId id) { data(id, {}, true); }
    void peer_reset(transport::StreamId id, std::uint64_t code = 1) {
        events_.push_back(transport::PeerResetEvent{id, code});
    }
    void datagram(Bytes bytes) { events_.push_back(transport::DatagramEvent{std::move(bytes)}); }
    void close_session(std::uint64_t code) {
        events_.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, code, {}});
    }

    // What the probe has written to a stream.
    const Sent* sent(transport::StreamId id) const {
        const auto found = sent_.find(id);
        return found == sent_.end() ? nullptr : &found->second;
    }
    bool answered(const std::string& key) const { return answered_.contains(key); }
    void mark(const std::string& key) { answered_.insert(key); }

    std::uint64_t granted_bidi() const { return granted_bidi_; }
    std::uint64_t granted_uni() const { return granted_uni_; }
    std::size_t datagram_payload{1200};

private:
    Bytes peer_setup_;
    Reaction reaction_;
    std::uint64_t granted_bidi_{0};
    std::uint64_t granted_uni_{0};
    bool started_{false};
    std::uint64_t next_bidi_{1};
    std::uint64_t next_uni_{3};
    std::map<transport::StreamId, Sent> sent_;
    std::vector<transport::TransportEvent> events_;
    std::set<std::string> answered_;
};

// Runs a probe to completion or its deadline on a deterministic clock.
inline scenarios::RawProbeTranscript run_probe(ScriptedPublisher& publisher,
                                              scenarios::RawProbeDefinition definition) {
    scenarios::RawProbeController controller(publisher, std::move(definition));
    auto now = scenarios::RawProbeClock::now();
    for (int step = 0; step < 4000; ++step) {
        const auto& transcript = controller.poll(now);
        if (transcript.complete || transcript.harness_failed || transcript.timed_out) break;
        now += std::chrono::milliseconds(1);
    }
    return controller.transcript();
}

}  // namespace moq::interop::test
