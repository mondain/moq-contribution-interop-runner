#include "moq/interop/scenarios/draft18.h"
#include "moq/interop/scenarios/run_controller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace moq::interop::scenarios {
namespace {

class ScriptedTransport final : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override {
        return {transport::TransportStatus::Success, 1};
    }
    transport::OpenResult open_uni() override {
        return {transport::TransportStatus::Success, 3};
    }
    transport::OperationResult write(transport::StreamId stream_id,
                                     std::span<const std::byte> data,
                                     bool fin) override {
        if (stream_id != 0 && stream_id != 1 && stream_id != 3) {
            return {transport::TransportStatus::InvalidState, 0, {}};
        }
        if (stream_id == 1 && block_request_after_two_bytes) {
            if (request_bytes.empty()) {
                request_bytes.insert(request_bytes.end(), data.begin(),
                                     data.begin() + 2);
                return {transport::TransportStatus::Partial, 2, {}};
            }
            return {transport::TransportStatus::WouldBlock, 0, {}};
        }
        if (fin) ++fin_count;
        auto& output = stream_id == 0 ? response_bytes
                     : stream_id == 1 ? request_bytes : setup_bytes;
        output.insert(output.end(), data.begin(), data.end());
        return {transport::TransportStatus::Success, data.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    transport::OperationResult stop_sending(transport::StreamId,
                                            std::uint64_t) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    transport::OperationResult send_datagram(std::span<const std::byte>) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    transport::OperationResult close(std::uint64_t error,
                                      std::span<const std::byte>) override {
        closed_error = error;
        return {transport::TransportStatus::Success, 0, {}};
    }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        auto result = std::move(inbound);
        inbound.clear();
        return result;
    }

    std::vector<transport::TransportEvent> inbound;
    std::vector<std::byte> setup_bytes;
    std::vector<std::byte> request_bytes;
    std::vector<std::byte> response_bytes;
    std::size_t fin_count{0};
    std::optional<std::uint64_t> closed_error;
    bool block_request_after_two_bytes{false};
};

TEST(Draft18RunController, DrivesSubscribeAfterSetupAndCapturesResponse) {
    ScriptedTransport transport;
    Draft18RunController controller(
        transport,
        subscribe_to_publisher_track(
            {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
            std::chrono::milliseconds(100), std::chrono::milliseconds(20)));
    const auto now = Clock::time_point{};
    transport.inbound.push_back(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    transport.inbound.push_back(transport::StreamDataEvent{
        2, {std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}}, false});

    const auto started = controller.poll(now);
    EXPECT_EQ(started.status, ScenarioStatus::Running);
    EXPECT_FALSE(started.harness_failed);
    EXPECT_FALSE(transport.setup_bytes.empty());
    EXPECT_FALSE(transport.request_bytes.empty());
    EXPECT_TRUE(controller.context().stimulus_delivered);
    EXPECT_EQ(controller.context().scenario_id,
              "subscribe-to-publisher-track");

    transport.inbound.push_back(transport::StreamDataEvent{
        1, {std::byte{0x04}, std::byte{0x00}, std::byte{0x04},
            std::byte{0x05}, std::byte{0x00}, std::byte{0x02},
            std::byte{0x09}}, false});
    const auto responded = controller.poll(now + std::chrono::milliseconds(1));
    EXPECT_EQ(responded.status, ScenarioStatus::Running);
    EXPECT_EQ(responded.step_index, 1u);
    const auto finished = controller.poll(now + std::chrono::milliseconds(22));
    EXPECT_EQ(finished.status, ScenarioStatus::Passed);
    EXPECT_TRUE(controller.context().complete);
    EXPECT_EQ(std::count_if(controller.context().evidence.begin(),
                            controller.context().evidence.end(),
                            [](const session::EvidenceEvent& event) {
                                return event.kind ==
                                       session::EvidenceKind::InitialResponseObserved;
                            }), 1);
}

TEST(Draft18RunController, PeerProtocolViolationClosesTransportWithoutHarnessFailure) {
    ScriptedTransport transport;
    Draft18RunController controller(
        transport,
        subscribe_to_publisher_track(
            {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
            std::chrono::milliseconds(100), std::chrono::milliseconds(20)));
    const auto now = Clock::time_point{};
    transport.inbound.push_back(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    transport.inbound.push_back(transport::StreamDataEvent{
        2, {std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}}, false});
    ASSERT_EQ(controller.poll(now).status, ScenarioStatus::Running);
    transport.inbound.push_back(transport::StreamDataEvent{
        2, {std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}}, false});
    const auto result = controller.poll(now + std::chrono::milliseconds(1));
    EXPECT_FALSE(result.harness_failed);
    EXPECT_EQ(transport.closed_error, 3u);
}

TEST(Draft18RunController, SendsSessionGeneratedRequestErrorOnPeerStream) {
    ScriptedTransport transport;
    Draft18RunController controller(
        transport,
        subscribe_to_publisher_track(
            {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
            std::chrono::milliseconds(100), std::chrono::milliseconds(20)));
    const auto now = Clock::time_point{};
    transport.inbound.push_back(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    transport.inbound.push_back(transport::StreamDataEvent{
        2, {std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}}, false});
    ASSERT_FALSE(controller.poll(now).harness_failed);

    wire::ByteWriter request_wire(256);
    ASSERT_TRUE(wire::draft18::encode_message(
        wire::draft18::PublishMessage{
            0, {{{std::byte{'.'}}}}, {{std::byte{'x'}}}, 7, {}, {}},
        request_wire).has_value());
    transport.inbound.push_back(transport::StreamDataEvent{
        0, {request_wire.bytes().begin(), request_wire.bytes().end()}, false});
    const auto result = controller.poll(now + std::chrono::milliseconds(1));
    EXPECT_FALSE(result.harness_failed);
    EXPECT_FALSE(transport.response_bytes.empty());
    EXPECT_EQ(transport.fin_count, 1u);
    wire::Cursor cursor(transport.response_bytes);
    const auto decoded = wire::draft18::decode_message(
        wire::draft18::StreamRole::Request, cursor, {});
    ASSERT_TRUE(std::holds_alternative<wire::draft18::Message>(decoded));
    EXPECT_NE(std::get_if<wire::draft18::RequestErrorMessage>(
                  &std::get<wire::draft18::Message>(decoded)), nullptr);
}

TEST(Draft18RunController, PeerCloseDuringBlockedStimulusIsNotHarnessFailure) {
    ScriptedTransport transport;
    transport.block_request_after_two_bytes = true;
    Draft18RunController controller(
        transport,
        subscribe_to_publisher_track(
            {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
            std::chrono::milliseconds(100), std::chrono::milliseconds(20)));
    const auto now = Clock::time_point{};
    transport.inbound.push_back(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    transport.inbound.push_back(transport::StreamDataEvent{
        2, {std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}}, false});
    ASSERT_FALSE(controller.poll(now).harness_failed);
    ASSERT_FALSE(controller.context().stimulus_delivered);
    transport.inbound.push_back(transport::StreamDataEvent{
        2, {std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}}, false});
    const auto result = controller.poll(now + std::chrono::milliseconds(1));
    EXPECT_FALSE(result.harness_failed);
    EXPECT_EQ(transport.closed_error, 3u);
    EXPECT_FALSE(controller.context().stimulus_delivered);
}

TEST(Draft18RunController, ResponseDeadlineStartsAfterStimulusDelivery) {
    ScriptedTransport transport;
    transport.block_request_after_two_bytes = true;
    Draft18RunController controller(
        transport,
        subscribe_to_publisher_track(
            {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
            std::chrono::milliseconds(100), std::chrono::milliseconds(20)));
    const auto now = Clock::time_point{};
    transport.inbound.push_back(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    transport.inbound.push_back(transport::StreamDataEvent{
        2, {std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}}, false});
    ASSERT_EQ(controller.poll(now).status, ScenarioStatus::Running);
    EXPECT_EQ(controller.poll(now + std::chrono::milliseconds(150)).status,
              ScenarioStatus::Running);
    EXPECT_FALSE(controller.context().stimulus_delivered);
    transport.block_request_after_two_bytes = false;
    EXPECT_EQ(controller.poll(now + std::chrono::milliseconds(151)).status,
              ScenarioStatus::Running);
    EXPECT_TRUE(controller.context().stimulus_delivered);
    EXPECT_EQ(controller.poll(now + std::chrono::milliseconds(250)).status,
              ScenarioStatus::Running);
    EXPECT_EQ(controller.poll(now + std::chrono::milliseconds(251)).status,
              ScenarioStatus::TimedOut);
    EXPECT_TRUE(controller.context().complete);
}

}  // namespace
}  // namespace moq::interop::scenarios
