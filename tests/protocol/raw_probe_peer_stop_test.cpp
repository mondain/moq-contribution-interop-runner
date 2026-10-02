#include "moq/interop/scenarios/raw_probe.h"
#include "../support/contribution_harness.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;

// The first peer-initiated unidirectional stream other than the control stream
// that has delivered data and has not ended.
std::optional<transport::StreamId> open_peer_data_stream(const RawProbeGateInput& input) {
    std::map<transport::StreamId, bool> ended;
    std::set<transport::StreamId> with_data;
    for (const auto& event : input.events) {
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && (data->stream_id & 3u) == 2u && data->stream_id != 2) {
            with_data.insert(data->stream_id);
            if (data->fin) ended[data->stream_id] = true;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            ended[reset->stream_id] = true;
        }
    }
    for (const auto id : with_data)
        if (!ended[id]) return id;
    return std::nullopt;
}

RawProbeDefinition definition() {
    RawProbeWrite request{RawProbeChannel::NewBidi, bytes_of({1}), false};
    RawProbeWrite stop{RawProbeChannel::NewUni, {}, false};
    stop.operation = RawProbeOperation::StopSending;
    stop.application_error = 0x9d;
    stop.select_peer_stream = open_peer_data_stream;
    RawProbeDefinition result{"peer-stop", bytes_of({0xaf, 0, 0, 0}), {request, stop}, true,
        [](std::span<const std::byte> input) { return !input.empty(); }, std::chrono::milliseconds{50},
        [](const RawProbeTranscript& transcript) {
            return transcript.writes.size() == 2 && transcript.writes[1].operation_accepted;
        }, {}};
    return result;
}

TEST(RawProbePeerStop, WaitsForAnOpenPeerStreamThenCancelsItWithTheConfiguredCode) {
    const auto def = definition();
    ScriptTransport* unused = nullptr;
    (void)unused;
    const auto transcript = drive_probe(def, [&](PeerView& v) {
        v.when("setup", v.step == 0, [&] { v.data(2, bytes_of({0xaf, 0, 0, 0})); });
        // A finished stream is never selected; the open one is.
        v.when("done", v.step == 2, [&] { v.data(6, bytes_of({0x30}), true); });
        v.when("open", v.step == 4, [&] { v.data(10, bytes_of({0x30})); });
    });
    EXPECT_TRUE(transcript.complete);
    ASSERT_EQ(transcript.writes.size(), 2u);
    EXPECT_EQ(transcript.writes[1].stream_id, 10u);
    EXPECT_TRUE(transcript.writes[1].operation_accepted);
    EXPECT_TRUE(raw_probe_stimulus_valid(transcript, def));

    // The proof re-derives the stream from the events seen at that point.
    auto forged = transcript;
    forged.writes[1].stream_id = 6;
    EXPECT_FALSE(raw_probe_stimulus_valid(forged, def));
    forged = transcript;
    forged.writes[1].stream_id = 9;  // not a peer unidirectional stream
    EXPECT_FALSE(raw_probe_stimulus_valid(forged, def));
    forged = transcript;
    forged.writes[1].prepared_event_count = std::nullopt;
    EXPECT_FALSE(raw_probe_stimulus_valid(forged, def));
}

TEST(RawProbePeerStop, NeverActsWithoutAQualifyingStream) {
    const auto def = definition();
    ScriptTransport transport;
    transport.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
    RawProbeController controller(transport, def);
    transport.events.push_back(transport::StreamDataEvent{2, bytes_of({0xaf, 0, 0, 0}), false});
    const auto start = RawProbeClock::now();
    for (int step = 0; step < 5; ++step) controller.poll(start + std::chrono::milliseconds(step));
    EXPECT_TRUE(transport.stops.empty());
    EXPECT_FALSE(controller.transcript().writes[1].operation_accepted);
}

TEST(RawProbePeerStop, DefinitionsMixingSelectionWithOtherModesAreRejected) {
    auto def = definition();
    def.writes[1].reuse_write_stream = 0;
    ScriptTransport transport;
    EXPECT_THROW(RawProbeController(transport, def), std::invalid_argument);
    def = definition();
    def.writes[1].operation = RawProbeOperation::Write;
    EXPECT_THROW(RawProbeController(transport, def), std::invalid_argument);
    def = definition();
    def.writes[1].bytes = bytes_of({1});
    EXPECT_THROW(RawProbeController(transport, def), std::invalid_argument);
}

}  // namespace
}  // namespace moq::interop::scenarios
