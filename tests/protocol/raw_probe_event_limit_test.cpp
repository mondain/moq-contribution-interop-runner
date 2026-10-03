#include "moq/interop/scenarios/raw_probe.h"
#include <gtest/gtest.h>
#include <chrono>
#include <deque>
#include <map>

namespace moq::interop::scenarios {
namespace {
using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

class FloodPeer : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, next_bidi += 4}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, next_uni += 4}; }
    transport::OperationResult write(transport::StreamId id, std::span<const std::byte> bytes, bool) override {
        output[id].insert(output[id].end(), bytes.begin(), bytes.end());
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult send_datagram(std::span<const std::byte>) override { return {}; }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override { return {}; }
    std::vector<transport::TransportEvent> poll(std::size_t limit) override {
        // Like a real transport, hand out a bounded batch per poll.
        std::vector<transport::TransportEvent> batch;
        while (!events.empty() && batch.size() < limit) {
            batch.push_back(std::move(events.front()));
            events.pop_front();
        }
        return batch;
    }
    std::uint64_t next_bidi = static_cast<std::uint64_t>(-3);
    std::uint64_t next_uni = static_cast<std::uint64_t>(-1);
    std::map<std::uint64_t, Bytes> output;
    std::deque<transport::TransportEvent> events;
};

// A probe that writes one stimulus after the publisher's SETUP and then just waits.
RawProbeDefinition waiting_definition() {
    RawProbeDefinition definition{"flood-probe", b({0xaf, 0, 0, 0}),
        {{RawProbeChannel::NewUni, b({1, 2, 3}), false}}, true,
        [](auto input) { return input.size() == 4; }, 60000ms};
    return definition;
}

// A harness around one controller: SETUP exchanged, stimulus delivered, manual clock.
class Rig {
public:
    explicit Rig(RawProbeDefinition definition = waiting_definition())
        : controller_(peer, std::move(definition)), t0_(RawProbeClock::time_point{} + 1000s) {
        peer.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        peer.events.push_back(transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false});
    }
    const RawProbeTranscript& poll() { return controller_.poll(t0_ + std::chrono::milliseconds(++tick_)); }
    // Polls until the queued events have all been handed over.
    const RawProbeTranscript& drain() {
        for (int i = 0; i < 100 && !peer.events.empty(); ++i) poll();
        return poll();
    }
    void data(transport::StreamId stream, std::size_t size, bool fin = false, std::byte fill = std::byte{0x5a}) {
        peer.events.push_back(transport::StreamDataEvent{stream, Bytes(size, fill), fin});
    }
    const RawProbeTranscript& transcript() const { return controller_.transcript(); }
    std::size_t settled() const { return controller_.settled_event_count(); }
    FloodPeer peer;

private:
    RawProbeController controller_;
    RawProbeClock::time_point t0_;
    int tick_{0};
};

std::vector<std::byte> stream_bytes(const RawProbeTranscript& transcript, transport::StreamId id) {
    std::vector<std::byte> result;
    for (const auto& event : transcript.events)
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event); data && data->stream_id == id)
            result.insert(result.end(), data->data.begin(), data->data.end());
    return result;
}

TEST(RawProbeEventLimit, FloodOfUnmergeableEventsEndsTheContextNotTheRun) {
    Rig rig;
    rig.poll();
    ASSERT_TRUE(rig.transcript().stimulus_delivered);
    // Chunks that alternate between streams cannot be merged, so only the event limit stops them.
    for (std::size_t i = 0; i < kRawProbeMaximumEvents + 904; ++i) rig.data(i % 2 ? 6 : 10, 100);
    const auto& transcript = rig.drain();
    EXPECT_FALSE(transcript.harness_failed);
    EXPECT_TRUE(transcript.event_limit_reached);
    EXPECT_FALSE(transcript.complete);
    EXPECT_FALSE(transcript.timed_out);
    EXPECT_EQ(transcript.events.size(), kRawProbeMaximumEvents);
    EXPECT_EQ(transcript.event_times.size(), transcript.events.size());
    EXPECT_NE(transcript.event_limit_reason.find("more than 4096 transport events"), std::string::npos);
    EXPECT_NE(transcript.event_limit_reason.find("this context is unscored"), std::string::npos);
    // Nothing more is recorded once the limit is reached.
    rig.data(6, 10);
    EXPECT_EQ(rig.poll().events.size(), kRawProbeMaximumEvents);
    EXPECT_TRUE(transcript.harness_failure_reason.empty());
    EXPECT_FALSE(raw_probe_stimulus_valid(transcript, waiting_definition()));
}

TEST(RawProbeEventLimit, RealBitratePublisherStaysWithinTheLimitByCoalescing) {
    Rig rig;
    rig.poll();
    // 3000 chunks of 1200 bytes (3.6 MB) of one object stream: far more events than the
    // limit allows one by one, but only about 220 recorded events.
    for (std::size_t i = 0; i < 3000; ++i) rig.data(6, 1200);
    const auto& transcript = rig.drain();
    EXPECT_FALSE(transcript.event_limit_reached);
    EXPECT_FALSE(transcript.harness_failed);
    EXPECT_LT(transcript.events.size(), 400u);
    EXPECT_EQ(stream_bytes(transcript, 6).size(), 3000u * 1200u);
}

TEST(RawProbeEventLimit, ByteLimitTruncatesToo) {
    Rig rig;
    rig.poll();
    for (std::size_t i = 0; i < 300; ++i) rig.data(6, 16 * 1024);  // 4.8 MB, 300 merged-size events
    const auto& transcript = rig.drain();
    EXPECT_TRUE(transcript.event_limit_reached);
    EXPECT_FALSE(transcript.harness_failed);
    EXPECT_NE(transcript.event_limit_reason.find("bytes of stream data"), std::string::npos);
    std::size_t recorded = 0;
    for (const auto& event : transcript.events)
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) recorded += data->data.size();
    EXPECT_LE(recorded, kRawProbeMaximumEvidenceBytes);
}

TEST(RawProbeEventLimit, ChunksOfOneStreamAreMergedWithTheirFinAndNothingElse) {
    Rig rig;
    rig.poll();
    const auto base = rig.transcript().events.size();
    rig.data(6, 600, false, std::byte{1});
    rig.data(6, 600, false, std::byte{2});
    rig.data(10, 600, false, std::byte{3});  // another stream interleaves: no merge across it
    rig.data(6, 600, false, std::byte{4});
    rig.data(6, 600, true, std::byte{5});    // the merged event carries the FIN
    rig.data(6, 600, false, std::byte{6});   // nothing follows a FIN on a stream; never merged
    const auto& transcript = rig.drain();
    ASSERT_EQ(transcript.events.size(), base + 4);
    const auto& first = std::get<transport::StreamDataEvent>(transcript.events[base]);
    EXPECT_EQ(first.data.size(), 1200u);
    EXPECT_EQ(first.data.front(), std::byte{1});
    EXPECT_EQ(first.data.back(), std::byte{2});
    EXPECT_FALSE(first.fin);
    EXPECT_EQ(std::get<transport::StreamDataEvent>(transcript.events[base + 1]).stream_id, 10u);
    const auto& third = std::get<transport::StreamDataEvent>(transcript.events[base + 2]);
    EXPECT_EQ(third.data.size(), 1200u);
    EXPECT_TRUE(third.fin);
    EXPECT_EQ(transcript.event_times.size(), transcript.events.size());
}

TEST(RawProbeEventLimit, SmallProtocolChunksKeepTheirBoundaries) {
    Rig rig;
    rig.poll();
    const auto base = rig.transcript().events.size();
    for (int i = 0; i < 5; ++i) rig.data(6, 20);   // below kRawProbeCoalesceMinimumBytes
    EXPECT_EQ(rig.drain().events.size(), base + 5);
}

TEST(RawProbeEventLimit, MergedEventsNeverExceedTheCoalescedSize) {
    Rig rig;
    rig.poll();
    for (int i = 0; i < 40; ++i) rig.data(6, 1200);
    const auto& transcript = rig.drain();
    for (const auto& event : transcript.events)
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event))
            EXPECT_LE(data->data.size(), kRawProbeMaximumCoalescedBytes);
    EXPECT_EQ(stream_bytes(transcript, 6).size(), 40u * 1200u);
}

TEST(RawProbeEventLimit, NeverMergesAnEventThatARecordedCountPointsPast) {
    // The stimulus is accepted when 3 events have been observed (CONNECTION, SETUP, chunk A),
    // so chunk B on the same stream arrived after it and must stay after it.
    Rig rig;
    rig.data(6, 600, false, std::byte{0xa});
    const auto& first = rig.poll();
    ASSERT_TRUE(first.stimulus_delivered);
    ASSERT_EQ(first.delivery_event_count, 3u);
    ASSERT_EQ(first.setup.delivery_event_count, 3u);
    rig.data(6, 600, false, std::byte{0xb});
    const auto& transcript = rig.poll();
    ASSERT_EQ(transcript.events.size(), 4u);
    EXPECT_EQ(std::get<transport::StreamDataEvent>(transcript.events[2]).data, Bytes(600, std::byte{0xa}));
    EXPECT_EQ(std::get<transport::StreamDataEvent>(transcript.events[3]).data, Bytes(600, std::byte{0xb}));
    // Once a later event exists behind the marker, further chunks merge into it again.
    rig.data(6, 600, false, std::byte{0xc});
    EXPECT_EQ(rig.poll().events.size(), 4u);
    EXPECT_EQ(std::get<transport::StreamDataEvent>(rig.transcript().events[3]).data.size(), 1200u);
}

TEST(RawProbeEventLimit, OnlyAGrowingTailIsUnsettled) {
    Rig rig;
    rig.poll();
    EXPECT_EQ(rig.transcript().events.size(), 2u);
    EXPECT_EQ(rig.settled(), 2u);   // CONNECTION and SETUP never change
    rig.data(6, 600);
    rig.poll();
    ASSERT_EQ(rig.transcript().events.size(), 3u);
    EXPECT_EQ(rig.settled(), 2u);   // bulk data on a data stream: the next chunk would join it
    rig.data(10, 600);
    rig.poll();
    EXPECT_EQ(rig.settled(), 3u);   // another stream arrived; the first event is final
    rig.data(10, 600, true);
    rig.poll();
    ASSERT_EQ(rig.transcript().events.size(), 4u);
    EXPECT_EQ(rig.settled(), 4u);   // FIN: nothing joins it
    rig.data(6, 20);
    rig.poll();
    EXPECT_EQ(rig.settled(), 5u);   // small protocol chunk: never extended
}

TEST(RawProbeEventLimit, SetupStreamAndRequestStreamsAreNeverMerged) {
    Rig rig;
    rig.poll();
    const auto base = rig.transcript().events.size();
    rig.data(2, 600);   // more bytes on the SETUP stream
    rig.data(2, 600);
    rig.data(4, 600);   // a peer-opened bidirectional request stream
    rig.data(4, 600);
    EXPECT_EQ(rig.drain().events.size(), base + 4);
}

TEST(RawProbeEventLimit, GateInputBeyondItsBoundTruncatesInsteadOfFailing) {
    auto definition = waiting_definition();
    definition.writes.front().evidence_ready = [](const RawProbeGateInput&) { return false; };
    Rig rig(std::move(definition));
    rig.data(6, 40000);
    rig.data(10, 40000);
    const auto& transcript = rig.drain();
    EXPECT_FALSE(transcript.harness_failed);
    EXPECT_TRUE(transcript.event_limit_reached);
    EXPECT_FALSE(transcript.stimulus_delivered);
    EXPECT_NE(transcript.event_limit_reason.find("unscored"), std::string::npos);
}

TEST(RawProbeEventLimit, EveryHarnessFailureStatesItsReason) {
    {
        Rig rig;
        rig.peer.events.push_back(transport::TransportErrorEvent{});
        const auto& transcript = rig.drain();
        EXPECT_TRUE(transcript.harness_failed);
        EXPECT_NE(transcript.harness_failure_reason.find("transport"), std::string::npos);
    }
    {
        Rig rig;
        rig.peer.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        const auto& transcript = rig.drain();
        EXPECT_TRUE(transcript.harness_failed);
        EXPECT_FALSE(transcript.harness_failure_reason.empty());
    }
    {
        Rig rig;
        rig.peer.events.push_back(transport::EventQueueOverflowEvent{});
        EXPECT_FALSE(rig.drain().harness_failure_reason.empty());
    }
}

TEST(RawProbeEventLimit, TruncatedTranscriptIsNeverAValidProof) {
    Rig rig;
    rig.poll();
    auto transcript = rig.transcript();
    const auto definition = waiting_definition();
    // The definition above never completes; give the proof every other property it needs.
    transcript.complete = true;
    transcript.event_limit_reached = true;
    EXPECT_FALSE(raw_probe_stimulus_valid(transcript, definition));
    transcript.event_limit_reached = false;
    EXPECT_TRUE(raw_probe_stimulus_valid(transcript, definition));
}

}  // namespace
}  // namespace moq::interop::scenarios
