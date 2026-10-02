// Announcement controller evidence added for completeness-gap slice A.
#include "moq/interop/scenarios/draft21_announcement.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop::scenarios {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

class Transport final : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::InvalidState, 0}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, 3}; }
    transport::OperationResult write(transport::StreamId stream,
                                     std::span<const std::byte> data, bool fin) override {
        if (fin) return {transport::TransportStatus::Success, 0, {}};
        auto& output = stream == 3 ? setup_bytes : response_bytes;
        output.insert(output.end(), data.begin(), data.end());
        return {transport::TransportStatus::Success, data.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    transport::OperationResult stop_sending(transport::StreamId, std::uint64_t) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    transport::OperationResult send_datagram(std::span<const std::byte>) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    transport::OperationResult close(std::uint64_t error, std::span<const std::byte>) override {
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
    std::optional<std::uint64_t> close_error;
};

transport::ConnectionEstablishedEvent established() {
    return {bytes({'m', 'o', 'q', 't', '-', '2', '1'}), {}, {}, 1200};
}
transport::StreamDataEvent empty_setup() { return {2, bytes({0xaf, 0x00, 0x00, 0x00}), false}; }

// PUBLISH with a one-field namespace whose single field is `field`.
transport::StreamDataEvent publish_in(const std::string& field, unsigned stream = 0) {
    std::vector<unsigned> body{0x00, 0x01, static_cast<unsigned>(field.size())};
    for (const char c : field) body.push_back(static_cast<unsigned char>(c));
    for (const unsigned c : {0x04u, 116u, 101u, 115u, 116u, 0x02u, 0x00u}) body.push_back(c);
    std::vector<unsigned> frame{0x1d, 0x00, static_cast<unsigned>(body.size())};
    frame.insert(frame.end(), body.begin(), body.end());
    std::vector<std::byte> out;
    for (const auto value : frame) out.push_back(static_cast<std::byte>(value));
    return {stream, std::move(out), false};
}

transport::StreamDataEvent publish_namespace_in(const std::string& field, unsigned stream = 0) {
    std::vector<unsigned> body{0x00, 0x01, static_cast<unsigned>(field.size())};
    for (const char c : field) body.push_back(static_cast<unsigned char>(c));
    body.push_back(0x00);
    std::vector<unsigned> frame{0x06, 0x00, static_cast<unsigned>(body.size())};
    frame.insert(frame.end(), body.begin(), body.end());
    std::vector<std::byte> out;
    for (const auto value : frame) out.push_back(static_cast<std::byte>(value));
    return {stream, std::move(out), false};
}

Draft21AnnouncementController make_controller(Transport& transport) {
    return {transport, {bytes({'m', 'e', 'd', 'i', 'a'})},
            bytes({'t', 'e', 's', 't'}), std::chrono::milliseconds(10)};
}

std::size_t count(const Draft21AnnouncementController& controller,
                  Draft21AnnouncementEventKind kind) {
    const auto& evidence = controller.context().evidence;
    return static_cast<std::size_t>(std::count_if(evidence.begin(), evidence.end(),
        [&](const auto& event) { return event.kind == kind; }));
}

TEST(Draft21GapAController, RecordsPeerSetupOptionValues) {
    Transport transport;
    auto controller = make_controller(transport);
    // AUTHORITY (5) = "h:1" then PATH... option types must be ascending, so
    // PATH (1) comes first: delta 1, length 1, '/'; delta 4 -> type 5.
    transport.inbound = {established(), transport::StreamDataEvent{2,
        bytes({0xaf, 0x00, 0x00, 0x08, 0x01, 0x01, '/', 0x04, 0x03, 'h', ':', '1'}), false}};
    controller.poll(Draft21Clock::time_point{});
    const auto& options = controller.context().peer_setup_options;
    ASSERT_EQ(options.size(), 2u);
    EXPECT_EQ(options[0].type, 1u);
    EXPECT_TRUE(options[0].is_bytes);
    EXPECT_EQ(options[0].bytes, bytes({'/'}));
    EXPECT_EQ(options[1].type, 5u);
    EXPECT_EQ(options[1].bytes, bytes({'h', ':', '1'}));
    const auto& event = controller.context().evidence.at(1);
    EXPECT_EQ(event.kind, Draft21AnnouncementEventKind::PeerSetupReceived);
    EXPECT_EQ(event.detail, "1=2f;5=683a31");
}

TEST(Draft21GapAController, PublishEvidenceCarriesTheNamespace) {
    Transport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), empty_setup(), publish_in("media")};
    controller.poll(Draft21Clock::time_point{});
    const auto& evidence = controller.context().evidence;
    const auto found = std::find_if(evidence.begin(), evidence.end(), [](const auto& event) {
        return event.kind == Draft21AnnouncementEventKind::PublishObserved;
    });
    ASSERT_NE(found, evidence.end());
    ASSERT_EQ(found->track_namespace.size(), 1u);
    EXPECT_EQ(found->track_namespace.front(), bytes({'m', 'e', 'd', 'i', 'a'}));
}

TEST(Draft21GapAController, SinglePeriodPublishIsRejectedAndNeverCompletesTheRun) {
    // Section 2.4.2: requests referencing "." are rejected with DOES_NOT_EXIST.
    Transport transport;
    Draft21AnnouncementController controller(transport, {bytes({'.'})},
        bytes({'t', 'e', 's', 't'}), std::chrono::milliseconds(10));
    transport.inbound = {established(), empty_setup(), publish_in(".")};
    const auto result = controller.poll(Draft21Clock::time_point{});
    EXPECT_EQ(result.status, Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(controller.context().target_publish_seen);
    EXPECT_FALSE(controller.context().response_delivered);
    EXPECT_FALSE(controller.context().complete);
    // REQUEST_ERROR (0x05) with DOES_NOT_EXIST (0x10).
    ASSERT_FALSE(transport.response_bytes.empty());
    EXPECT_EQ(transport.response_bytes.front(), std::byte{0x05});
    EXPECT_EQ(count(controller, Draft21AnnouncementEventKind::PublishObserved), 1u);
}

TEST(Draft21GapAController, WindowElapsedRequiresALiveFullySetUpSession) {
    Transport transport;
    auto controller = make_controller(transport);
    transport.inbound = {established(), empty_setup()};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(controller.context().window_elapsed);
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{} + std::chrono::milliseconds(11)).status,
              Draft21AnnouncementStatus::TimedOut);
    EXPECT_TRUE(controller.context().window_elapsed);

    Transport lonely_transport;
    auto lonely = make_controller(lonely_transport);
    lonely_transport.inbound = {established()};
    lonely.poll(Draft21Clock::time_point{});
    lonely.poll(Draft21Clock::time_point{} + std::chrono::milliseconds(11));
    EXPECT_FALSE(lonely.context().window_elapsed);
}

TEST(Draft21GapAController, RoutingScenarioCompletesOnAnsweredNamespaceAnnouncement) {
    Transport transport;
    auto controller = make_controller(transport);
    controller.configure_scenario("d21-publisher-namespace-routing-announcement");
    transport.inbound = {established(), empty_setup(), publish_namespace_in("media")};
    EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
              Draft21AnnouncementStatus::Passed);
    EXPECT_TRUE(controller.context().namespace_announced);
    EXPECT_FALSE(controller.context().target_publish_seen);
    EXPECT_TRUE(controller.context().complete);

    // Any other scenario keeps waiting for the target PUBLISH.
    Transport other_transport;
    auto other = make_controller(other_transport);
    other_transport.inbound = {established(), empty_setup(), publish_namespace_in("media")};
    EXPECT_EQ(other.poll(Draft21Clock::time_point{}).status, Draft21AnnouncementStatus::Running);
    EXPECT_TRUE(other.context().namespace_announced);
    EXPECT_FALSE(other.context().complete);

    // A different namespace is not the routed namespace.
    Transport elsewhere_transport;
    auto elsewhere = make_controller(elsewhere_transport);
    elsewhere.configure_scenario("d21-publisher-namespace-routing-announcement");
    elsewhere_transport.inbound = {established(), empty_setup(), publish_namespace_in("other")};
    EXPECT_EQ(elsewhere.poll(Draft21Clock::time_point{}).status, Draft21AnnouncementStatus::Running);
    EXPECT_FALSE(elsewhere.context().namespace_announced);
}

TEST(Draft21GapAController, MalformedPublisherMessagesCarryTheDecoderDetail) {
    {
        // PUBLISH with an empty namespace field (Section 8.7).
        Transport transport;
        auto controller = make_controller(transport);
        transport.inbound = {established(), empty_setup(), publish_in("")};
        EXPECT_EQ(controller.poll(Draft21Clock::time_point{}).status,
                  Draft21AnnouncementStatus::Failed);
        const auto& evidence = controller.context().evidence;
        const auto found = std::find_if(evidence.begin(), evidence.end(), [](const auto& event) {
            return event.kind == Draft21AnnouncementEventKind::MalformedPublisherMessage;
        });
        ASSERT_NE(found, evidence.end());
        EXPECT_EQ(found->detail, "empty draft-21 namespace field");
    }
    {
        // PUBLISH_NAMESPACE with an empty namespace field.
        Transport transport;
        auto controller = make_controller(transport);
        transport.inbound = {established(), empty_setup(), publish_namespace_in("")};
        controller.poll(Draft21Clock::time_point{});
        const auto& evidence = controller.context().evidence;
        EXPECT_TRUE(std::any_of(evidence.begin(), evidence.end(), [](const auto& event) {
            return event.kind == Draft21AnnouncementEventKind::MalformedPublisherMessage &&
                   event.detail == "empty draft-21 namespace field";
        }));
    }
    {
        // SETUP whose second option delta overflows the 64-bit type space.
        Transport transport;
        auto controller = make_controller(transport);
        transport.inbound = {established(), transport::StreamDataEvent{2,
            bytes({0xaf, 0x00, 0x00, 0x0b, 0x02, 0x00,
                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}), false}};
        controller.poll(Draft21Clock::time_point{});
        const auto& evidence = controller.context().evidence;
        const auto found = std::find_if(evidence.begin(), evidence.end(), [](const auto& event) {
            return event.kind == Draft21AnnouncementEventKind::MalformedPublisherMessage;
        });
        ASSERT_NE(found, evidence.end());
        EXPECT_NE(found->detail.find("type overflow"), std::string::npos) << found->detail;
    }
}

}  // namespace
}  // namespace moq::interop::scenarios
