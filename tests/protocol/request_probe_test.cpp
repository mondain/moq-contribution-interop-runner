#include "moq/interop/scenarios/request_probe.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
RequestProbeProfile profile(unsigned draft) {
    return {draft, "requirement", "request-error", 0x10, false,
        {"request-probe", bytes({0xaf, 0, 0, 0}),
         {{RawProbeChannel::NewBidi, bytes({3, 0, 5, 1, 0, 1, 'x', 0}), false}},
         true, [](auto input) { return input.empty(); },
         std::chrono::milliseconds(1000), request_probe_response_ready}};
}
RawProbeTranscript transcript(const RequestProbeProfile& profile) {
    RawProbeTranscript result;
    result.scenario_id = profile.definition.id;
    result.setup = {{RawProbeChannel::NewUni, profile.definition.setup_bytes, false}, 3,
                    profile.definition.setup_bytes.size(), false};
    result.writes = {{profile.definition.writes.front(), 1,
                     profile.definition.writes.front().bytes.size(), false}};
    result.transport_established = result.peer_setup_received = true;
    result.stimulus_delivered = result.complete = true;
    result.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2, {}, false},
        transport::StreamDataEvent{1, bytes({5, 0, 3, 0x10, 0, 0}), true}};
    result.delivery_event_count = 2;
    return result;
}
TEST(RequestProbe, ExactRequestErrorIsScoredForEachDraft) {
    for (const unsigned draft : {18u, 21u}) {
        const auto definition = profile(draft);
        auto observed = transcript(definition);
        EXPECT_TRUE(request_probe_response_ready(observed));
        EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), true);
        std::get<transport::StreamDataEvent>(observed.events.back()).data[3] = std::byte{0x11};
        EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), false);
    }
}
TEST(RequestProbe, UnknownAliasRequiresExplicitCompatibilityCode) {
    for (const unsigned draft : {18u, 21u}) {
        auto definition = profile(draft);
        definition.compatibility_error = true;
        auto observed = transcript(definition);
        EXPECT_FALSE(evaluate_raw_probe_request_error(observed, definition).has_value());
        observed.unknown_auth_token_alias_compatibility_code = 0x10;
        EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), true);
        observed.unknown_auth_token_alias_compatibility_code = 0x11;
        EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), false);
        definition.compatibility_error = false;
        EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), true);
    }
}
TEST(RequestProbe, ResponseFragmentsMustCompleteOnTheMatchingStream) {
    const auto definition = profile(21);
    auto observed = transcript(definition);
    auto& data = std::get<transport::StreamDataEvent>(observed.events.back());
    data.data = bytes({5, 0, 3, 0x10});
    data.fin = false;
    EXPECT_FALSE(request_probe_response_ready(observed));
    EXPECT_FALSE(evaluate_raw_probe_request_error(observed, definition).has_value());
    observed.events.push_back(transport::StreamDataEvent{5, bytes({0, 0}), true});
    EXPECT_FALSE(request_probe_response_ready(observed));
    observed.events.push_back(transport::StreamDataEvent{1, bytes({0, 0}), true});
    EXPECT_TRUE(request_probe_response_ready(observed));
    EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), true);
}
TEST(RequestProbe, EarlyResponseAndPartialStimulusCannotPass) {
    const auto definition = profile(18);
    auto observed = transcript(definition);
    observed.delivery_event_count = 3;
    EXPECT_FALSE(request_probe_response_ready(observed));
    EXPECT_FALSE(evaluate_raw_probe_request_error(observed, definition).has_value());
    observed = transcript(definition);
    --observed.writes.front().accepted;
    EXPECT_FALSE(evaluate_raw_probe_request_error(observed, definition).has_value());
    observed = transcript(definition);
    observed.events.push_back(transport::EventQueueOverflowEvent{});
    EXPECT_FALSE(evaluate_raw_probe_request_error(observed, definition).has_value());
}
TEST(RequestProbe, TruncatedAndWrongResponseTypesAreFailures) {
    const auto definition = profile(21);
    auto observed = transcript(definition);
    std::get<transport::StreamDataEvent>(observed.events.back()).data = bytes({5, 0, 3, 0x10});
    EXPECT_TRUE(request_probe_response_ready(observed));
    EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), false);
    observed = transcript(definition);
    std::get<transport::StreamDataEvent>(observed.events.back()).data = bytes({7, 0, 1, 0});
    EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), false);
    observed = transcript(definition);
    observed.events.back() = transport::PeerResetEvent{1, 0};
    EXPECT_TRUE(request_probe_response_ready(observed));
    EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), false);
}
TEST(RequestProbe, StopSendingDoesNotTerminateTheResponseDirection) {
    const auto definition = profile(21);
    auto observed = transcript(definition);
    observed.events.back() = transport::PeerStopSendingEvent{1, 0};
    EXPECT_FALSE(request_probe_response_ready(observed));
    EXPECT_FALSE(evaluate_raw_probe_request_error(observed, definition).has_value());
    observed.events.push_back(transport::StreamDataEvent{1, bytes({5, 0, 3, 0x10, 0, 0}), true});
    EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), true);
    observed.events.push_back(transport::PeerResetEvent{1, 0});
    EXPECT_EQ(evaluate_raw_probe_request_error(observed, definition), true);
}

class ResponseTransport : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, 1}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, 3}; }
    transport::OperationResult write(transport::StreamId, std::span<const std::byte> data, bool fin) override {
        if (fin) return {transport::TransportStatus::WouldBlock, 0, {}};
        return {transport::TransportStatus::Success, data.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult send_datagram(std::span<const std::byte>) override { return {}; }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override { return {}; }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        auto result = std::move(events);
        events.clear();
        return result;
    }
    std::vector<transport::TransportEvent> events;
};
TEST(RequestProbe, ControllerCompletesWithoutWaitingForRequesterFinOrSessionClose) {
    const auto definition = profile(21);
    ResponseTransport transport;
    RawProbeController controller(transport, definition.definition);
    transport.events = {transport::ConnectionEstablishedEvent{},
                        transport::StreamDataEvent{2, {}, false}};
    const auto now = RawProbeClock::time_point{};
    ASSERT_TRUE(controller.poll(now).stimulus_delivered);
    EXPECT_FALSE(controller.transcript().complete);
    transport.events = {transport::StreamDataEvent{1, bytes({5, 0, 3, 0x10}), false}};
    EXPECT_FALSE(controller.poll(now).complete);
    transport.events = {transport::StreamDataEvent{1, bytes({0, 0}), true}};
    EXPECT_TRUE(controller.poll(now).complete);
    EXPECT_EQ(evaluate_raw_probe_request_error(controller.transcript(), definition), true);
}
}  // namespace
}  // namespace moq::interop::scenarios
