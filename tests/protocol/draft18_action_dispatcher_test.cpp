#include "moq/interop/scenarios/action_dispatcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace moq::interop::scenarios {
namespace {

class RecordingTransport final : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override {
        ++open_count;
        if (next_open < scripted_opens.size()) {
            return scripted_opens[next_open++];
        }
        return {transport::TransportStatus::Success,
                1 + 4 * (open_count - 1)};
    }
    transport::OpenResult open_uni() override {
        ++open_uni_count;
        return {transport::TransportStatus::Success, 3};
    }
    transport::OperationResult write(transport::StreamId stream_id,
                                     std::span<const std::byte> data,
                                     bool fin) override {
        if ((stream_id & 3u) != 1u && stream_id != 3) {
            return {transport::TransportStatus::InvalidState, 0, {}};
        }
        if (fin) return {transport::TransportStatus::InvalidState, 0, {}};
        const auto result = next_write < scripted_writes.size()
            ? scripted_writes[next_write++]
            : transport::OperationResult{transport::TransportStatus::Success,
                                         data.size(), {}};
        if (result.accepted <= data.size()) {
            written.insert(written.end(), data.begin(),
                           data.begin() + static_cast<std::ptrdiff_t>(result.accepted));
        }
        return result;
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
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    std::vector<transport::TransportEvent> poll(std::size_t) override { return {}; }

    std::vector<std::byte> written;
    std::vector<transport::OperationResult> scripted_writes;
    std::size_t next_write{0};
    std::size_t open_count{0};
    std::size_t open_uni_count{0};
    std::vector<transport::OpenResult> scripted_opens;
    std::size_t next_open{0};
};

void activate(session::PublisherSession& session) {
    session.on_event(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    session.observe_local_stream(3, session::LocalStreamPurpose::Control);
    session.observe_local_message(3, wire::draft18::SetupMessage{}, false);
    constexpr std::array peer_setup{
        std::byte{0xaf}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    session.on_event(transport::StreamDataEvent{
        2, {peer_setup.begin(), peer_setup.end()}, false});
    ASSERT_EQ(session.phase(), session::SessionPhase::Active);
    session.take_evidence(64);
}

TEST(Draft18ActionDispatcher, FullyWrittenSubscribeBecomesDeliveredStimulus) {
    RecordingTransport transport;
    session::PublisherSession session;
    activate(session);
    ActionDispatcher dispatcher(transport, session);
    const auto result = dispatcher.submit(OpenRequestAction{
        wire::draft18::SubscribeMessage{
            1, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false});
    EXPECT_EQ(result.state, DispatchState::Complete);
    EXPECT_TRUE(result.stimulus_delivered);

    wire::Cursor cursor(transport.written);
    const auto decoded = wire::draft18::decode_message(
        wire::draft18::StreamRole::Request, cursor, {});
    ASSERT_TRUE(std::holds_alternative<wire::draft18::Message>(decoded));
    const auto* subscribe = std::get_if<wire::draft18::SubscribeMessage>(
        &std::get<wire::draft18::Message>(decoded));
    ASSERT_NE(subscribe, nullptr);
    EXPECT_EQ(subscribe->request_id, 1u);
    EXPECT_EQ(cursor.offset(), transport.written.size());

    const auto evidence = session.take_evidence(64);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const session::EvidenceEvent& event) {
                               const auto* request = std::get_if<
                                   session::RequestObservedEvidence>(&event.data);
                               return request && request->initiator ==
                                                     session::RequestInitiator::Local &&
                                      request->request_id == 1;
                           }), evidence.end());
}

TEST(Draft18ActionDispatcher, SendsSetupOnServerControlStream) {
    RecordingTransport transport;
    session::PublisherSession session;
    session.on_event(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    ActionDispatcher dispatcher(transport, session);
    const auto result = dispatcher.submit_setup();
    EXPECT_EQ(result.state, DispatchState::Complete);
    EXPECT_TRUE(result.stimulus_delivered);
    EXPECT_EQ(result.stream_id, 3u);
    EXPECT_EQ(transport.open_uni_count, 1u);
    EXPECT_EQ(transport.open_count, 0u);

    wire::Cursor cursor(transport.written);
    const auto decoded = wire::draft18::decode_message(
        wire::draft18::StreamRole::Control, cursor, {});
    ASSERT_TRUE(std::holds_alternative<wire::draft18::Message>(decoded));
    EXPECT_NE(std::get_if<wire::draft18::SetupMessage>(
                  &std::get<wire::draft18::Message>(decoded)), nullptr);
    EXPECT_EQ(cursor.offset(), transport.written.size());
    const auto evidence = session.take_evidence(64);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const session::EvidenceEvent& event) {
                               return event.kind ==
                                      session::EvidenceKind::LocalSetupObserved;
                           }), evidence.end());
    constexpr std::array peer_setup{
        std::byte{0xaf}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    session.on_event(transport::StreamDataEvent{
        2, {peer_setup.begin(), peer_setup.end()}, false});
    EXPECT_EQ(session.phase(), session::SessionPhase::Active);
}

TEST(Draft18ActionDispatcher, PartialSetupIsObservedOnlyAfterFullWrite) {
    RecordingTransport transport;
    transport.scripted_writes.push_back(
        {transport::TransportStatus::Partial, 2, {}});
    session::PublisherSession session;
    session.on_event(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    ActionDispatcher dispatcher(transport, session);
    const auto first = dispatcher.submit_setup();
    EXPECT_EQ(first.state, DispatchState::Pending);
    EXPECT_FALSE(first.stimulus_delivered);
    const auto early = session.take_evidence(64);
    EXPECT_EQ(std::count_if(early.begin(), early.end(),
                            [](const session::EvidenceEvent& event) {
                                return event.kind ==
                                       session::EvidenceKind::LocalSetupObserved;
                            }), 0);
    const auto second = dispatcher.flush();
    EXPECT_EQ(second.state, DispatchState::Complete);
    EXPECT_TRUE(second.stimulus_delivered);
    EXPECT_EQ(transport.open_uni_count, 1u);
    const auto late = session.take_evidence(64);
    EXPECT_EQ(std::count_if(late.begin(), late.end(),
                            [](const session::EvidenceEvent& event) {
                                return event.kind ==
                                       session::EvidenceKind::LocalSetupObserved;
                            }), 1);
}

TEST(Draft18ActionDispatcher, SetupWaitsForTransportAndCannotBeRepeated) {
    RecordingTransport transport;
    session::PublisherSession session;
    ActionDispatcher dispatcher(transport, session);
    const auto queued = dispatcher.submit_setup();
    EXPECT_EQ(queued.state, DispatchState::Pending);
    EXPECT_EQ(transport.open_uni_count, 0u);
    session.on_event(transport::ConnectionEstablishedEvent{
        {std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
         std::byte{'-'}, std::byte{'1'}, std::byte{'8'}}, {}, {}, 1200});
    EXPECT_TRUE(dispatcher.flush().stimulus_delivered);
    EXPECT_EQ(dispatcher.submit_setup().state, DispatchState::Failed);
    EXPECT_EQ(transport.open_uni_count, 1u);
}

TEST(Draft18ActionDispatcher, PartialWriteIsNotDeliveredUntilFlushed) {
    RecordingTransport transport;
    transport.scripted_writes.push_back(
        {transport::TransportStatus::Partial, 2, {}});
    session::PublisherSession session;
    activate(session);
    ActionDispatcher dispatcher(transport, session);
    const auto first = dispatcher.submit(OpenRequestAction{
        wire::draft18::SubscribeMessage{
            1, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false});
    EXPECT_EQ(first.state, DispatchState::Pending);
    EXPECT_FALSE(first.stimulus_delivered);
    const auto early_evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(early_evidence.begin(), early_evidence.end(),
                            [](const session::EvidenceEvent& event) {
                                return event.kind ==
                                       session::EvidenceKind::RequestObserved;
                            }), 0);

    const auto completed = dispatcher.flush();
    EXPECT_EQ(completed.state, DispatchState::Complete);
    EXPECT_TRUE(completed.stimulus_delivered);
    const auto late_evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(late_evidence.begin(), late_evidence.end(),
                            [](const session::EvidenceEvent& event) {
                                return event.kind ==
                                       session::EvidenceKind::RequestObserved;
                            }), 1);
}

TEST(Draft18ActionDispatcher, WouldBlockRetriesSameStreamWithoutRequestEvidence) {
    RecordingTransport transport;
    transport.scripted_writes.push_back(
        {transport::TransportStatus::WouldBlock, 0, {}});
    session::PublisherSession session;
    activate(session);
    ActionDispatcher dispatcher(transport, session);
    const auto first = dispatcher.submit(OpenRequestAction{
        wire::draft18::SubscribeMessage{
            1, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false});
    EXPECT_EQ(first.state, DispatchState::Pending);
    EXPECT_FALSE(first.stimulus_delivered);
    EXPECT_TRUE(dispatcher.has_pending());
    EXPECT_TRUE(transport.written.empty());
    const auto early = session.take_evidence(64);
    EXPECT_EQ(std::count_if(early.begin(), early.end(),
                            [](const session::EvidenceEvent& event) {
                                return event.kind ==
                                       session::EvidenceKind::RequestObserved;
                            }), 0);

    const auto second = dispatcher.flush();
    EXPECT_EQ(second.state, DispatchState::Complete);
    EXPECT_TRUE(second.stimulus_delivered);
    EXPECT_EQ(transport.open_count, 1u);
}

TEST(Draft18ActionDispatcher, StreamCreditWaitDoesNotInventARequest) {
    RecordingTransport transport;
    transport.scripted_opens.push_back(
        {transport::TransportStatus::StreamLimit, 0});
    session::PublisherSession session;
    activate(session);
    ActionDispatcher dispatcher(transport, session);
    const auto first = dispatcher.submit(OpenRequestAction{
        wire::draft18::SubscribeMessage{
            1, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false});
    EXPECT_EQ(first.state, DispatchState::Pending);
    EXPECT_FALSE(first.stream_id.has_value());
    EXPECT_FALSE(first.stimulus_delivered);
    EXPECT_TRUE(session.take_evidence(64).empty());

    const auto second = dispatcher.flush();
    EXPECT_EQ(second.state, DispatchState::Complete);
    EXPECT_TRUE(second.stimulus_delivered);
    EXPECT_EQ(transport.open_count, 2u);
}

TEST(Draft18ActionDispatcher, EvenLocalRequestIdIsRejectedBeforeWireWrite) {
    RecordingTransport transport;
    session::PublisherSession session;
    activate(session);
    ActionDispatcher dispatcher(transport, session);
    const auto result = dispatcher.submit(OpenRequestAction{
        wire::draft18::SubscribeMessage{
            0, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false});
    EXPECT_EQ(result.state, DispatchState::Failed);
    EXPECT_FALSE(result.stimulus_delivered);
    EXPECT_EQ(transport.open_count, 0u);
    EXPECT_TRUE(transport.written.empty());
    EXPECT_TRUE(session.take_evidence(64).empty());
}

TEST(Draft18ActionDispatcher, RequestWaitsForActiveMoqSession) {
    RecordingTransport transport;
    session::PublisherSession session;
    ActionDispatcher dispatcher(transport, session);
    const auto result = dispatcher.submit(OpenRequestAction{
        wire::draft18::SubscribeMessage{
            1, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false});
    EXPECT_EQ(result.state, DispatchState::Pending);
    EXPECT_FALSE(result.stimulus_delivered);
    EXPECT_EQ(transport.open_count, 0u);
    EXPECT_TRUE(transport.written.empty());
    activate(session);
    EXPECT_TRUE(dispatcher.flush().stimulus_delivered);
}

TEST(Draft18ActionDispatcher, PendingRequestFailsWhenSessionCloses) {
    RecordingTransport transport;
    session::PublisherSession session;
    ActionDispatcher dispatcher(transport, session);
    EXPECT_EQ(dispatcher.submit(OpenRequestAction{
                  wire::draft18::SubscribeMessage{
                      1, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}},
                  false}).state, DispatchState::Pending);
    session.on_event(transport::LocalCloseEvent{
        transport::CloseErrorSpace::Application, 3, {}});
    const auto result = dispatcher.flush();
    EXPECT_EQ(result.state, DispatchState::Failed);
    EXPECT_FALSE(result.stimulus_delivered);
    EXPECT_FALSE(dispatcher.has_pending());
    EXPECT_EQ(transport.open_count, 0u);
}

TEST(Draft18ActionDispatcher, ReusedLocalRequestIdIsRejectedBeforeWireWrite) {
    RecordingTransport transport;
    session::PublisherSession session;
    activate(session);
    ActionDispatcher dispatcher(transport, session);
    const OpenRequestAction action{
        wire::draft18::SubscribeMessage{
            1, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false};
    ASSERT_EQ(dispatcher.submit(action).state, DispatchState::Complete);
    const auto sent_bytes = transport.written.size();
    const auto second = dispatcher.submit(action);
    EXPECT_EQ(second.state, DispatchState::Failed);
    EXPECT_FALSE(second.stimulus_delivered);
    EXPECT_EQ(transport.open_count, 1u);
    EXPECT_EQ(transport.written.size(), sent_bytes);
}

TEST(Draft18ActionDispatcher, RequestIdsAdvanceByTwoWithoutGaps) {
    RecordingTransport transport;
    session::PublisherSession session;
    activate(session);
    ActionDispatcher dispatcher(transport, session);
    const auto request = [](std::uint64_t id) {
        return OpenRequestAction{
            wire::draft18::SubscribeMessage{
                id, {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, {}}, false};
    };
    ASSERT_TRUE(dispatcher.submit(request(1)).stimulus_delivered);
    const auto sent_bytes = transport.written.size();
    const auto skipped = dispatcher.submit(request(5));
    EXPECT_EQ(skipped.state, DispatchState::Failed);
    EXPECT_EQ(transport.open_count, 1u);
    EXPECT_EQ(transport.written.size(), sent_bytes);

    const auto next = dispatcher.submit(request(3));
    EXPECT_EQ(next.state, DispatchState::Complete);
    EXPECT_TRUE(next.stimulus_delivered);
    EXPECT_EQ(next.stream_id, 5u);
    EXPECT_EQ(transport.open_count, 2u);
}

}  // namespace
}  // namespace moq::interop::scenarios
