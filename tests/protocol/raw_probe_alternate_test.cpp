#include "moq/interop/scenarios/raw_probe.h"
#include "../support/contribution_harness.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;

RawProbeDefinition definition(bool alternate) {
    RawProbeWrite request{RawProbeChannel::NewBidi, bytes_of({1}), false};
    RawProbeDefinition result{"alternate", bytes_of({0xaf, 0, 0, 0}), {request}, true,
        [](std::span<const std::byte> input) { return !input.empty(); }, std::chrono::milliseconds{50},
        [](const RawProbeTranscript& transcript) {
            return std::any_of(transcript.alternate_events.begin(), transcript.alternate_events.end(),
                               [](const auto& event) { return std::holds_alternative<transport::StreamDataEvent>(event); });
        }, {}};
    result.alternate_listener = alternate;
    return result;
}

TEST(RawProbeAlternate, RecordsWhatArrivesOnTheSecondListenerAfterTheFirstSessionEnds) {
    ScriptTransport primary;
    ScriptTransport second;
    primary.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
    RawProbeController controller(primary, definition(true), &second);
    primary.events.push_back(transport::StreamDataEvent{2, bytes_of({0xaf, 0, 0, 0}), false});
    const auto start = RawProbeClock::now();
    controller.poll(start);
    controller.poll(start + std::chrono::milliseconds(1));
    EXPECT_TRUE(controller.transcript().stimulus_delivered);
    // The publisher ends the first session; the observation goes on.
    primary.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
    controller.poll(start + std::chrono::milliseconds(2));
    EXPECT_FALSE(controller.transcript().complete);
    EXPECT_FALSE(controller.transcript().timed_out);
    second.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
    controller.poll(start + std::chrono::milliseconds(3));
    EXPECT_FALSE(controller.transcript().complete);
    second.events.push_back(transport::StreamDataEvent{2, bytes_of({0xaf, 0, 0, 0}), false});
    const auto& transcript = controller.poll(start + std::chrono::milliseconds(4));
    EXPECT_TRUE(transcript.complete);
    ASSERT_EQ(transcript.alternate_events.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<transport::ConnectionEstablishedEvent>(transcript.alternate_events[0]));
}

TEST(RawProbeAlternate, WithoutASecondListenerAFirstSessionCloseEndsTheProbe) {
    ScriptTransport primary;
    primary.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
    RawProbeController controller(primary, definition(false));
    primary.events.push_back(transport::StreamDataEvent{2, bytes_of({0xaf, 0, 0, 0}), false});
    const auto start = RawProbeClock::now();
    controller.poll(start);
    controller.poll(start + std::chrono::milliseconds(1));
    primary.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
    const auto& transcript = controller.poll(start + std::chrono::milliseconds(2));
    EXPECT_TRUE(transcript.complete);
    EXPECT_TRUE(transcript.alternate_events.empty());
}

}  // namespace
}  // namespace moq::interop::scenarios
