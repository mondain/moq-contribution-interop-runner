#pragma once

#include "moq/interop/scenarios/draft21_contribution.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace moq::interop::test {

using Bytes = std::vector<std::byte>;

inline Bytes cbytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

inline Bytes cvi(std::uint64_t value) {
    // Draft-21 vi64: the leading one bits give the length.
    Bytes result;
    if (value < 0x80) return {static_cast<std::byte>(value)};
    if (value < 0x4000) {
        result.push_back(static_cast<std::byte>(0x80 | (value >> 8)));
        result.push_back(static_cast<std::byte>(value & 0xff));
        return result;
    }
    if (value < (1u << 21)) {
        result.push_back(static_cast<std::byte>(0xc0 | (value >> 16)));
        result.push_back(static_cast<std::byte>((value >> 8) & 0xff));
        result.push_back(static_cast<std::byte>(value & 0xff));
        return result;
    }
    if (value < (1u << 28)) {
        result.push_back(static_cast<std::byte>(0xe0 | (value >> 24)));
        for (int shift = 16; shift >= 0; shift -= 8)
            result.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        return result;
    }
    result.push_back(std::byte{0xff});
    for (int shift = 56; shift >= 0; shift -= 8)
        result.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    return result;
}

inline Bytes cframe(std::uint64_t type, const Bytes& body) {
    auto result = cvi(type);
    result.push_back(static_cast<std::byte>(body.size() >> 8));
    result.push_back(static_cast<std::byte>(body.size() & 0xff));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

inline Bytes cconcat(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

inline Bytes subscribe_ok(std::uint64_t alias = 0, const Bytes& parameters = cbytes({0})) {
    auto body = cvi(alias);
    body.insert(body.end(), parameters.begin(), parameters.end());
    return cframe(4, body);
}
inline Bytes request_ok() { return cbytes({7, 0, 1, 0}); }
inline Bytes request_error(std::uint64_t code) {
    auto body = cvi(code);
    body.push_back(std::byte{0});
    body.push_back(std::byte{0});
    return cframe(5, body);
}

// Incrementally builds a transcript that satisfies the raw-probe proof for a
// contribution probe: events and write acceptances are interleaved in the
// order the controller would produce them.
class ContributionRun {
public:
    explicit ContributionRun(const scenarios::Draft21ContributionProbe& probe,
                             Bytes peer_setup = cbytes({0xaf, 0, 0, 0}))
        : definition_(probe.definition) {
        transcript_.scenario_id = definition_.id;
        transcript_.setup = {{scenarios::RawProbeChannel::NewUni, definition_.setup_bytes, false},
                             3, definition_.setup_bytes.size(), false, 1};
        transcript_.transport_established = transcript_.peer_setup_received = true;
        transcript_.max_datagram_payload = 1200;
        for (const auto& write : definition_.writes)
            transcript_.writes.push_back({write, {}, 0, false});
        transcript_.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        transcript_.events.push_back(transport::StreamDataEvent{2, std::move(peer_setup), false});
    }

    void event(transport::TransportEvent event) { transcript_.events.push_back(std::move(event)); }
    void reply(transport::StreamId stream, const Bytes& data, bool fin = false) {
        event(transport::StreamDataEvent{stream, data, fin});
    }
    transport::StreamId stream_of(std::size_t index) const { return *transcript_.writes[index].stream_id; }

    void deliver(std::size_t index) {
        auto& accepted = transcript_.writes[index];
        const auto& write = definition_.writes[index];
        if (write.prepare_bytes) {
            const auto prior = std::span<const scenarios::RawProbeAcceptedWrite>(transcript_.writes).first(index);
            const auto prepared = write.prepare_bytes({prior, transcript_.events});
            if (!prepared) throw std::logic_error("test preparation");
            accepted.write.bytes = *prepared;
            accepted.prepared_event_count = transcript_.events.size();
        }
        if (write.reuse_write_stream) {
            accepted.stream_id = transcript_.writes[*write.reuse_write_stream].stream_id;
        } else if (write.channel == scenarios::RawProbeChannel::PeerBidi) {
            accepted.stream_id = 0;
        } else if (write.channel == scenarios::RawProbeChannel::Datagram) {
            accepted.stream_id = std::nullopt;
        } else if (write.channel == scenarios::RawProbeChannel::NewUni) {
            accepted.stream_id = next_uni_;
            next_uni_ += 4;
        } else {
            accepted.stream_id = next_bidi_;
            next_bidi_ += 4;
        }
        accepted.accepted = accepted.write.bytes.size();
        accepted.fin_accepted = accepted.write.fin;
        accepted.operation_accepted = write.operation == scenarios::RawProbeOperation::StopSending;
        accepted.delivery_event_count = transcript_.events.size();
        delivered_ = transcript_.events.size();
    }

    scenarios::RawProbeTranscript finish() {
        auto result = transcript_;
        result.stimulus_delivered = true;
        result.complete = true;
        result.delivery_event_count = delivered_;
        return result;
    }
    const scenarios::RawProbeTranscript& snapshot() const { return transcript_; }
    // Partial view usable with response_ready before the run completes.
    scenarios::RawProbeTranscript partial() const {
        auto result = transcript_;
        result.stimulus_delivered = true;
        result.delivery_event_count = delivered_;
        return result;
    }

private:
    scenarios::RawProbeDefinition definition_;
    scenarios::RawProbeTranscript transcript_;
    std::uint64_t next_bidi_{1};
    std::uint64_t next_uni_{7};
    std::size_t delivered_{0};
};

inline const scenarios::Draft21ContributionProbe& find_probe(
    const std::vector<scenarios::Draft21ContributionProbe>& probes, const std::string& scenario,
    const std::string& requirement = {}) {
    const auto found = std::find_if(probes.begin(), probes.end(), [&](const auto& probe) {
        return probe.definition.id == scenario &&
               (requirement.empty() || probe.requirement_id == requirement);
    });
    if (found == probes.end()) throw std::logic_error("missing contribution probe " + scenario);
    return *found;
}

}  // namespace moq::interop::test
