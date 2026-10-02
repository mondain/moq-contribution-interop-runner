#pragma once

// Internal: the opt-in courtesy responder of the raw probe controller. It answers
// requests a draft-21 publisher opens on its own request streams according to a
// RawProbeCourtesy policy and remembers what it sent. It is driven only by the
// transport events it is shown and by the clock it is given.

#include "moq/interop/scenarios/raw_probe.h"

#include <deque>
#include <map>
#include <set>
#include <vector>

namespace moq::interop::scenarios {

class PublisherCourtesy {
public:
    explicit PublisherCourtesy(RawProbeCourtesy policy);

    // The clock reading of the poll that is about to feed events.
    void set_now(RawProbeClock::time_point now) { now_ = now; }
    // Feeds one transport event, in arrival order.
    void on_event(const transport::TransportEvent& event);
    // Attempts every response that is due at `now`. `event_count` is the number of
    // transport events observed so far; it stamps responses that complete.
    // Returns the responses that completed during this call.
    std::vector<RawProbeCourtesyWrite> step(transport::SessionTransport& transport,
                                            RawProbeClock::time_point now,
                                            std::size_t event_count);

private:
    struct Response {
        std::vector<std::byte> bytes;
        std::size_t written{0};
        bool fin{false};
        bool fin_sent{false};
        RawProbeCourtesyKind kind{RawProbeCourtesyKind::PublishOk};
        RawProbeClock::time_point release{};
    };
    struct Stream {
        std::vector<std::byte> bytes;
        std::size_t consumed{0};
        std::size_t frames{0};
        std::deque<Response> queue;
    };
    struct AwaitingObject {
        transport::StreamId stream_id{0};
        std::uint64_t alias{0};
    };

    void parse(transport::StreamId id, Stream& stream);
    void enqueue(Stream& stream, std::vector<std::byte> bytes, bool fin, RawProbeCourtesyKind kind,
                 RawProbeClock::time_point release);
    void note_alias(std::uint64_t alias);

    // Per-controller budgets on frames parsed and responses queued.
    static constexpr std::size_t kMaximumFrames = 256;
    static constexpr std::size_t kMaximumResponses = 256;
    std::size_t frames_parsed_{0};
    std::size_t responses_enqueued_{0};

    RawProbeCourtesy policy_;
    std::map<transport::StreamId, Stream> streams_;
    std::map<transport::StreamId, std::vector<std::byte>> data_prefixes_;
    std::set<std::uint64_t> aliases_with_objects_;
    std::vector<AwaitingObject> awaiting_;
    // Release time for a frame completed at the instant parse() ran.
    RawProbeClock::time_point now_{};
};

}  // namespace moq::interop::scenarios
