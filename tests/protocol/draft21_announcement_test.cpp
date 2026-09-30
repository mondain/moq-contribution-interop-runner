#include "moq/interop/scenarios/draft21_announcement.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace moq::interop::scenarios {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

class ScriptedTransport final : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override {
        return {transport::TransportStatus::InvalidState, 0};
    }
    transport::OpenResult open_uni() override {
        return {transport::TransportStatus::Success, 3};
    }
    transport::OperationResult write(transport::StreamId stream,
                                     std::span<const std::byte> data,
                                     bool fin) override {
        if (stream != 0 && stream != 3 && stream != 4) {
            return {transport::TransportStatus::InvalidState, 0, {}};
        }
        if (fin) {
            ++fin_count;
            return {transport::TransportStatus::Success, 0, {}};
        }
        if (stream == 0 && block_response) {
            return {transport::TransportStatus::WouldBlock, 0, {}};
        }
        if (stream == 0 && partial_response_once) {
            partial_response_once = false;
            response_bytes.insert(response_bytes.end(), data.begin(),
                                  data.begin() + 2);
            return {transport::TransportStatus::Partial, 2, {}};
        }
        auto& output = stream == 3 ? setup_bytes : response_bytes;
        output.insert(output.end(), data.begin(), data.end());
        return {transport::TransportStatus::Success, data.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId,
                                      std::uint64_t) override {
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
        close_error = error;
        return {transport::TransportStatus::Success, 0, {}};
    }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        auto result = std::move(inbound);
        inbound.clear();
        return result;
    }

    std::vector<transport::TransportEvent> inbound;
    std::vector<std::byte> setup_bytes;
    std::vector<std::byte> response_bytes;
    std::size_t fin_count{0};
    std::optional<std::uint64_t> close_error;
    bool block_response{false};
    bool partial_response_once{false};
};

transport::ConnectionEstablishedEvent established(unsigned draft = 21) {
    return {draft == 21 ? bytes({'m', 'o', 'q', 't', '-', '2', '1'})
                        : bytes({'m', 'o', 'q', 't', '-', '1', '8'}),
            {}, {}, 1200};
}

transport::StreamDataEvent setup() {
    return {2, bytes({0xaf, 0x00, 0x00, 0x00}), false};
}

transport::StreamDataEvent publish(unsigned request_id = 0,
                                   unsigned last_name_byte = 't') {
    // draft-ietf-moq-transport-21 section 9.8, Figure 12.
    return {0, bytes({0x1d, 0x00, 0x0f,
                      request_id, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                      0x04, 't', 'e', 's', last_name_byte, 0x02, 0x00}), false};
}

transport::StreamDataEvent publish_namespace() {
    return {0, bytes({0x06, 0x00, 0x09, 0x00, 0x01,
                      0x05, 'm', 'e', 'd', 'i', 'a', 0x00}), false};
}

Draft21AnnouncementController make_controller(ScriptedTransport& transport) {
    return {transport, {bytes({'m', 'e', 'd', 'i', 'a'})},
            bytes({'t', 'e', 's', 't'}), std::chrono::milliseconds(100)};
}

TEST(Draft21Announcement, AcceptsExpectedPublishAfterSetup) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(), publish()};
    const auto result = controller.poll(Draft21Clock::time_point{});
    EXPECT_EQ(result.status, Draft21AnnouncementStatus::Passed);
    EXPECT_FALSE(result.harness_failed);
    EXPECT_EQ(transport.setup_bytes, bytes({0xaf, 0x00, 0x00, 0x00}));
    EXPECT_EQ(transport.response_bytes, bytes({0x07, 0x00, 0x01, 0x00}));
    EXPECT_TRUE(controller.context().target_publish_seen);
    EXPECT_TRUE(controller.context().response_delivered);
    EXPECT_TRUE(controller.context().complete);
}

TEST(Draft21Announcement, ServerUriProbesSendExactSetupOptionOverWebTransport) {
    for (const auto& [probe, expected] : {
             std::pair{Draft21SetupProbe::ServerAuthority,
                       bytes({0xaf, 0x00, 0x00, 0x0d, 0x05, 0x0b,
                              'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'o', 'r', 'g'})},
             std::pair{Draft21SetupProbe::ServerPath,
                       bytes({0xaf, 0x00, 0x00, 0x03, 0x01, 0x01, '/'})}}) {
        ScriptedTransport transport;
        Draft21AnnouncementController controller(
            transport, {bytes({'m', 'e', 'd', 'i', 'a'})},
            bytes({'t', 'e', 's', 't'}), std::chrono::milliseconds(100),
            probe, true);
        transport.inbound = {established(), setup()};
        controller.poll(Draft21Clock::time_point{});
        EXPECT_TRUE(controller.context().webtransport);
        EXPECT_EQ(transport.setup_bytes, expected);
    }
}

TEST(Draft21Announcement, WebTransportRejectsForbiddenPublisherSetupOptions) {
    for (const auto& [peer_setup, expected_error] : {
             std::pair{bytes({0xaf, 0x00, 0x00, 0x0d, 0x05, 0x0b,
                              'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'o', 'r', 'g'}),
                       0x19u},
             std::pair{bytes({0xaf, 0x00, 0x00, 0x03, 0x01, 0x01, '/'}),
                       0x8u}}) {
        ScriptedTransport transport;
        Draft21AnnouncementController controller(
            transport, {bytes({'m', 'e', 'd', 'i', 'a'})},
            bytes({'t', 'e', 's', 't'}), std::chrono::milliseconds(100),
            Draft21SetupProbe::None, true);
        transport.inbound = {established(),
            transport::StreamDataEvent{2, peer_setup, false}};
        const auto result = controller.poll(Draft21Clock::time_point{});
        EXPECT_EQ(transport.close_error, expected_error);
        EXPECT_EQ(result.status, Draft21AnnouncementStatus::Failed);
        EXPECT_FALSE(result.harness_failed);
        const auto& evidence = controller.context().evidence;
        const auto setup_event = std::find_if(
            evidence.begin(), evidence.end(), [](const auto& event) {
                return event.kind ==
                    Draft21AnnouncementEventKind::PeerSetupReceived;
            });
        ASSERT_NE(setup_event, evidence.end());
        EXPECT_EQ(setup_event->setup_option_types,
                  std::vector<std::uint64_t>{expected_error == 0x19u ? 5u : 1u});
    }
}

TEST(Draft21Announcement, AcknowledgesNamespaceBeforeTargetPublish) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(), publish_namespace()};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(controller.context().response_delivered);
    EXPECT_EQ(transport.response_bytes, bytes({0x07, 0x00, 0x01, 0x00}));
    auto track = publish(2);
    track.stream_id = 4;
    transport.inbound = {track};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{} +
                              std::chrono::milliseconds(1)).status,
              Draft21AnnouncementStatus::Passed);
    EXPECT_EQ(transport.response_bytes,
              bytes({0x07, 0x00, 0x01, 0x00, 0x07, 0x00, 0x01, 0x00}));
    EXPECT_EQ(std::count_if(controller.context().evidence.begin(),
                            controller.context().evidence.end(),
                            [](const Draft21AnnouncementEvent& event) {
                                return event.kind ==
                                    Draft21AnnouncementEventKind::NamespaceResponseDelivered;
                            }), 1);
}

TEST(Draft21Announcement, RejectsForbiddenDotNamespace) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(),
        transport::StreamDataEvent{0,
            bytes({0x06, 0x00, 0x05, 0x00, 0x01, 0x01, '.', 0x00}), false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(controller.context().response_delivered);
    EXPECT_EQ(transport.fin_count, 1u);
    EXPECT_FALSE(transport.response_bytes.empty());
}

TEST(Draft21Announcement, HoldsEarlyPublishUntilBothSetups) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), publish()};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_TRUE(transport.response_bytes.empty());
    transport.inbound = {setup()};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{} +
                              std::chrono::milliseconds(1)).status,
              Draft21AnnouncementStatus::Passed);
    EXPECT_EQ(transport.response_bytes, bytes({0x07, 0x00, 0x01, 0x00}));
}

TEST(Draft21Announcement, BackpressureDoesNotClaimResponseDelivery) {
    ScriptedTransport transport;
    transport.block_response = true;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(), publish()};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(controller.context().response_delivered);
    transport.block_response = false;
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{} +
                              std::chrono::milliseconds(1)).status,
              Draft21AnnouncementStatus::Passed);
}

TEST(Draft21Announcement, PartialResponseMustFinishBeforePassing) {
    ScriptedTransport transport;
    transport.partial_response_once = true;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(), publish()};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(controller.context().response_delivered);
    EXPECT_EQ(transport.response_bytes, bytes({0x07, 0x00}));
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{} +
                              std::chrono::milliseconds(1)).status,
              Draft21AnnouncementStatus::Passed);
    EXPECT_EQ(transport.response_bytes, bytes({0x07, 0x00, 0x01, 0x00}));
}

TEST(Draft21Announcement, WrongClientRequestIdClosesSession) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(), publish(1)};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Failed);
    EXPECT_EQ(transport.close_error, 0x4u);
    EXPECT_FALSE(controller.context().complete);
}

TEST(Draft21Announcement, RejectsWrongDraftBeforeSendingSetup) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(18)};
    const auto result = controller.poll(Draft21Clock::time_point{});
    EXPECT_EQ(result.status, Draft21AnnouncementStatus::Failed);
    EXPECT_EQ(transport.close_error, 3u);
    EXPECT_TRUE(transport.setup_bytes.empty());
    EXPECT_FALSE(controller.context().complete);
}

TEST(Draft21Announcement, WrongTrackDoesNotPass) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(), publish(0, 'x')};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(controller.context().target_publish_seen);
    EXPECT_EQ(transport.response_bytes, bytes({0x07, 0x00, 0x01, 0x00}));
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{} +
                              std::chrono::milliseconds(101)).status,
              Draft21AnnouncementStatus::TimedOut);
    EXPECT_FALSE(controller.context().complete);
}

TEST(Draft21Announcement, KnownNonControlStreamsAreNotUnknownTypes) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(),
                         transport::StreamDataEvent{6, bytes({0x05}), false},
                         transport::StreamDataEvent{10, bytes({0x10}), false},
                         transport::StreamDataEvent{
                             14, bytes({0xf0, 0x13, 0x2b, 0x3e, 0x28}), false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(transport.close_error);
}

TEST(Draft21Announcement, WaitsForCompleteFragmentedUnidirectionalType) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(),
                         transport::StreamDataEvent{6, bytes({0x80}), false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(transport.close_error);
    transport.inbound = {transport::StreamDataEvent{6, bytes({0x09}), false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{} +
                              std::chrono::milliseconds(1)).status,
              Draft21AnnouncementStatus::Failed);
    EXPECT_EQ(transport.close_error, 0x3u);
}

TEST(Draft21Announcement, UnknownUnidirectionalTypeClosesSession) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(),
                         transport::StreamDataEvent{6, bytes({0x09}), false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Failed);
    EXPECT_EQ(transport.close_error, 0x3u);
    EXPECT_FALSE(controller.context().complete);
}

TEST(Draft21Announcement, SecondPeerControlStreamClosesSession) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), setup(),
                         transport::StreamDataEvent{6, setup().data, false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Failed);
    EXPECT_EQ(transport.close_error, 0x3u);
    EXPECT_FALSE(controller.context().complete);
}

TEST(Draft21Announcement, ReservedRequestOpenerRecordsPublisherViolation) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {
        established(),
        transport::StreamDataEvent{0, bytes({0x1e, 0x00, 0x00}), false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Failed);
    EXPECT_EQ(transport.close_error, 0x3u);
    EXPECT_TRUE(std::any_of(
        controller.context().evidence.begin(),
        controller.context().evidence.end(),
        [](const Draft21AnnouncementEvent& event) {
            return event.kind ==
                       Draft21AnnouncementEventKind::InvalidRequestOpener &&
                   event.stream_id == 0;
        }));
}

TEST(Draft21Announcement, AllowedUnsupportedRequestOpenerIsNotViolation) {
    ScriptedTransport transport;
    auto controller = make_controller(transport);
    transport.inbound = {
        established(),
        transport::StreamDataEvent{0, bytes({0x03, 0x00, 0x00}), false}};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(transport.close_error);
    EXPECT_FALSE(std::any_of(
        controller.context().evidence.begin(),
        controller.context().evidence.end(),
        [](const Draft21AnnouncementEvent& event) {
            return event.kind ==
                   Draft21AnnouncementEventKind::InvalidRequestOpener;
        }));
}

}  // namespace
}  // namespace moq::interop::scenarios
