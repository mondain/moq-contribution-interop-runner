#include "transport/quiche_connection_internal.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace moq::interop::transport::detail {
namespace {

struct FakeApiState {
    std::int64_t stream_send_result = 0;
    std::uint64_t stream_send_error = 0;
    int shutdown_result = 0;
    std::int64_t datagram_max = 1200;
    std::int64_t datagram_send_result = 0;
    StreamId stream_id = 0;
    std::vector<std::byte> sent_bytes;
    bool fin = false;
    quiche_shutdown shutdown_direction = QUICHE_SHUTDOWN_READ;
    std::uint64_t shutdown_error = 0;
    std::vector<std::byte> datagram_bytes;
    bool datagram_pointer_nonnull = false;
    int free_calls = 0;
    std::uint64_t peer_bidi_left = 8;
    std::uint64_t peer_uni_left = 8;
    std::unordered_set<StreamId> materialized_streams;
    std::uint64_t opened_bidi = 0;
    std::uint64_t opened_uni = 0;
    int bidi_credit_calls = 0;
    int uni_credit_calls = 0;
    int close_result = 0;
    bool close_application = false;
    std::uint64_t close_error = 0;
    std::vector<std::byte> close_reason;
};

FakeApiState* fake_state = nullptr;

std::int64_t fake_stream_send(quiche_conn*, std::uint64_t stream_id,
                              const std::uint8_t* data, std::size_t size,
                              bool fin, std::uint64_t* out_error_code) {
    fake_state->stream_id = stream_id;
    fake_state->sent_bytes.clear();
    if (size != 0) {
        fake_state->sent_bytes.assign(
            reinterpret_cast<const std::byte*>(data),
            reinterpret_cast<const std::byte*>(data) + size);
    }
    fake_state->fin = fin;
    *out_error_code = fake_state->stream_send_error;
    if ((fake_state->stream_send_result >= 0 ||
         fake_state->stream_send_result == QUICHE_ERR_DONE) &&
        (stream_id & 1u) != 0u &&
        fake_state->materialized_streams.insert(stream_id).second) {
        const auto opened = (stream_id >> 2u) + 1u;
        if ((stream_id & 2u) == 0u) {
            const auto newly_opened = opened - fake_state->opened_bidi;
            fake_state->peer_bidi_left -= newly_opened;
            fake_state->opened_bidi = opened;
        } else {
            const auto newly_opened = opened - fake_state->opened_uni;
            fake_state->peer_uni_left -= newly_opened;
            fake_state->opened_uni = opened;
        }
    }
    return fake_state->stream_send_result;
}

int fake_shutdown(quiche_conn*, std::uint64_t stream_id,
                  quiche_shutdown direction, std::uint64_t error) {
    fake_state->stream_id = stream_id;
    fake_state->shutdown_direction = direction;
    fake_state->shutdown_error = error;
    return fake_state->shutdown_result;
}

std::uint64_t fake_peer_bidi_left(const quiche_conn*) {
    ++fake_state->bidi_credit_calls;
    return fake_state->peer_bidi_left;
}

std::uint64_t fake_peer_uni_left(const quiche_conn*) {
    ++fake_state->uni_credit_calls;
    return fake_state->peer_uni_left;
}

std::int64_t fake_datagram_max(const quiche_conn*) {
    return fake_state->datagram_max;
}

std::int64_t fake_datagram_send(quiche_conn*, const std::uint8_t* data,
                                std::size_t size) {
    fake_state->datagram_pointer_nonnull = data != nullptr;
    fake_state->datagram_bytes.clear();
    if (size != 0) {
        fake_state->datagram_bytes.assign(
            reinterpret_cast<const std::byte*>(data),
            reinterpret_cast<const std::byte*>(data) + size);
    }
    return fake_state->datagram_send_result;
}

void fake_connection_free(quiche_conn*) { ++fake_state->free_calls; }

int fake_connection_close(quiche_conn*, bool application,
                          std::uint64_t error, const std::uint8_t* reason,
                          std::size_t reason_size) {
    fake_state->close_application = application;
    fake_state->close_error = error;
    fake_state->close_reason.clear();
    if (reason_size != 0) {
        fake_state->close_reason.assign(
            reinterpret_cast<const std::byte*>(reason),
            reinterpret_cast<const std::byte*>(reason) + reason_size);
    }
    return fake_state->close_result;
}

QuicheApi fake_api() {
    return {fake_stream_send, fake_shutdown, fake_peer_bidi_left,
            fake_peer_uni_left, fake_datagram_max, fake_datagram_send,
            fake_connection_close, fake_connection_free};
}

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> output;
    for (const auto value : values) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

std::unique_ptr<QuicheConnection> make_connection(
    FakeApiState& state, std::size_t max_events = 8,
    std::size_t max_payload_bytes = 1024, std::uint64_t bidi_credit = 8,
    std::uint64_t uni_credit = 8) {
    fake_state = &state;
    state.peer_bidi_left = bidi_credit;
    state.peer_uni_left = uni_credit;
    auto created = QuicheConnection::create(
        reinterpret_cast<quiche_conn*>(std::uintptr_t{1}), fake_api(),
        EventLimits{max_events, max_payload_bytes});
    EXPECT_EQ(created.error, std::nullopt);
    return std::move(created.connection);
}

TEST(QuicheConnectionConstruction, RejectsZeroEventLimits) {
    FakeApiState state;
    fake_state = &state;
    auto count = QuicheConnection::create(
        reinterpret_cast<quiche_conn*>(std::uintptr_t{1}), fake_api(),
        EventLimits{0, 1});
    EXPECT_EQ(count.error, ConstructionError::InvalidEventLimits);
    EXPECT_EQ(count.connection, nullptr);

    auto bytes_limit = QuicheConnection::create(
        reinterpret_cast<quiche_conn*>(std::uintptr_t{1}), fake_api(),
        EventLimits{1, 0});
    EXPECT_EQ(bytes_limit.error, ConstructionError::InvalidEventLimits);
    EXPECT_EQ(bytes_limit.connection, nullptr);
}

TEST(QuicheConnectionStreams, AllocatesIndependentServerStreamSequences) {
    FakeApiState state;
    auto connection = make_connection(state);

    EXPECT_EQ(connection->open_bidi().stream_id, 1u);
    EXPECT_EQ(connection->open_bidi().stream_id, 5u);
    EXPECT_EQ(connection->open_uni().stream_id, 3u);
    EXPECT_EQ(connection->open_bidi().stream_id, 9u);
    EXPECT_EQ(connection->open_uni().stream_id, 7u);
}

TEST(QuicheConnectionStreams, CountsReservationsAgainstPeerCredit) {
    FakeApiState state;
    auto connection = make_connection(state, 8, 1024, 2, 1);

    EXPECT_EQ(connection->open_bidi().status, TransportStatus::Success);
    EXPECT_EQ(connection->open_bidi().status, TransportStatus::Success);
    EXPECT_EQ(connection->open_bidi().status, TransportStatus::StreamLimit);
    EXPECT_EQ(connection->open_uni().status, TransportStatus::Success);
    EXPECT_EQ(connection->open_uni().status, TransportStatus::StreamLimit);

    state.peer_bidi_left = 3;
    state.peer_uni_left = 2;
    EXPECT_EQ(connection->open_bidi().stream_id, 9u);
    EXPECT_EQ(connection->open_uni().stream_id, 7u);
}

TEST(QuicheConnectionStreams, RejectsIdIncrementOverflowWithoutWrapping) {
    FakeApiState state;
    fake_state = &state;
    auto created = QuicheConnection::create(
        reinterpret_cast<quiche_conn*>(std::uintptr_t{1}), fake_api(),
        EventLimits{8, 1024},
        InitialStreamIds{std::numeric_limits<StreamId>::max() - 2,
                         std::numeric_limits<StreamId>::max()});
    ASSERT_EQ(created.error, std::nullopt);
    auto connection = std::move(created.connection);

    EXPECT_EQ(connection->open_bidi().stream_id,
              std::numeric_limits<StreamId>::max() - 2);
    EXPECT_EQ(connection->open_bidi().status, TransportStatus::InvalidState);
    EXPECT_EQ(connection->open_uni().stream_id,
              std::numeric_limits<StreamId>::max());
    EXPECT_EQ(connection->open_uni().status, TransportStatus::InvalidState);
}

TEST(QuicheConnectionWrite, MapsCountsFinAndWouldBlockWithoutLosingReservation) {
    FakeApiState state;
    auto connection = make_connection(state, 8, 1024, 1, 1);
    const auto opened = connection->open_bidi();
    const auto payload = bytes({1, 2, 3});

    state.stream_send_result = QUICHE_ERR_DONE;
    EXPECT_EQ(connection->write(opened.stream_id, payload, true).status,
              TransportStatus::WouldBlock);
    EXPECT_EQ(state.peer_bidi_left, 0u);
    EXPECT_EQ(connection->open_bidi().status, TransportStatus::StreamLimit);

    state.stream_send_result = 2;
    const auto partial = connection->write(opened.stream_id, payload, true);
    EXPECT_EQ(partial.status, TransportStatus::Partial);
    EXPECT_EQ(partial.accepted, 2u);
    EXPECT_EQ(state.stream_id, opened.stream_id);
    EXPECT_EQ(state.sent_bytes, payload);
    EXPECT_TRUE(state.fin);

    state.stream_send_result = 3;
    const auto complete = connection->write(opened.stream_id, payload, false);
    EXPECT_EQ(complete.status, TransportStatus::Success);
    EXPECT_EQ(complete.accepted, 3u);
    EXPECT_FALSE(state.fin);

    state.stream_send_result = 0;
    EXPECT_EQ(connection->write(opened.stream_id, payload, false).status,
              TransportStatus::WouldBlock);
    EXPECT_EQ(connection->write(opened.stream_id, {}, true).status,
              TransportStatus::Success);
    EXPECT_TRUE(state.fin);
}

TEST(QuicheConnectionWrite, MapsEveryDocumentedStreamSendError) {
    FakeApiState state;
    auto connection = make_connection(state);
    const StreamId stream = 0;
    const auto payload = bytes({9});

    struct Case {
        std::int64_t raw;
        TransportStatus expected;
    };
    for (const Case item : {
             Case{QUICHE_ERR_DONE, TransportStatus::WouldBlock},
             Case{QUICHE_ERR_STREAM_LIMIT, TransportStatus::StreamLimit},
             Case{QUICHE_ERR_INVALID_STATE, TransportStatus::InvalidState},
             Case{QUICHE_ERR_INVALID_STREAM_STATE,
                  TransportStatus::InvalidState},
             Case{QUICHE_ERR_BUFFER_TOO_SHORT,
                  TransportStatus::InternalError},
             Case{-999, TransportStatus::InternalError},
         }) {
        state.stream_send_result = item.raw;
        EXPECT_EQ(connection->write(stream, payload, false).status,
                  item.expected)
            << item.raw;
    }

    state.stream_send_error = 77;
    state.stream_send_result = QUICHE_ERR_STREAM_STOPPED;
    const auto stopped = connection->write(stream, payload, false);
    EXPECT_EQ(stopped.status, TransportStatus::PeerStopped);
    ASSERT_TRUE(stopped.application_error.has_value());
    EXPECT_EQ(*stopped.application_error, 77u);
    auto stopped_events = connection->poll(8);
    ASSERT_EQ(stopped_events.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<PeerStopSendingEvent>(
        stopped_events.front()));
    EXPECT_EQ(std::get<PeerStopSendingEvent>(stopped_events.front()).stream_id,
              stream);
    EXPECT_EQ(std::get<PeerStopSendingEvent>(stopped_events.front())
                  .application_error,
              77u);
    EXPECT_EQ(connection->write(stream, bytes({3}), false).status,
              TransportStatus::PeerStopped);
    EXPECT_TRUE(connection->poll(8).empty());

    state.stream_send_error = 88;
    state.stream_send_result = QUICHE_ERR_STREAM_RESET;
    const auto reset = connection->write(stream, payload, false);
    EXPECT_EQ(reset.status, TransportStatus::PeerReset);
    ASSERT_TRUE(reset.application_error.has_value());
    EXPECT_EQ(*reset.application_error, 88u);

    state.stream_send_result = QUICHE_ERR_INVALID_STATE;
    EXPECT_EQ(connection->write(stream, payload, false).application_error,
              std::nullopt);
    state.stream_send_result = -999;
    EXPECT_EQ(connection->write(stream, payload, false).status,
              TransportStatus::InternalError);
    EXPECT_EQ(connection->last_transport_diagnostic(),
              (TransportDiagnostic{TransportOperation::StreamWrite, -999}));
}

TEST(QuicheConnectionWrite, BurnsReservedIdAfterHardFirstWriteFailure) {
    FakeApiState state;
    auto connection = make_connection(state, 8, 1024, 1, 1);
    const auto first = connection->open_bidi();
    state.stream_send_result = QUICHE_ERR_INVALID_STREAM_STATE;
    EXPECT_EQ(connection->write(first.stream_id, bytes({1}), false).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(connection->open_bidi().status, TransportStatus::StreamLimit);
    state.peer_bidi_left = 2;
    const auto second = connection->open_bidi();
    EXPECT_EQ(second.stream_id, 5u);
    state.stream_send_result = 1;
    EXPECT_EQ(connection->write(first.stream_id, bytes({2}), false).status,
              TransportStatus::InvalidState);
}

TEST(QuicheConnectionWrite, MaterializationAndHardFailureUpdateReservations) {
    FakeApiState state;
    auto connection = make_connection(state, 8, 1024, 3, 1);
    const auto first = connection->open_bidi();
    state.stream_send_result = QUICHE_ERR_INVALID_STREAM_STATE;
    EXPECT_EQ(connection->write(first.stream_id, bytes({1}), false).status,
              TransportStatus::InvalidState);
    const auto second = connection->open_bidi();
    const auto third = connection->open_bidi();
    EXPECT_EQ(second.stream_id, 5u);
    EXPECT_EQ(third.stream_id, 9u);
    EXPECT_EQ(connection->open_bidi().status, TransportStatus::StreamLimit);

    state.stream_send_result = 1;
    EXPECT_EQ(connection->write(third.stream_id, bytes({3}), false).status,
              TransportStatus::Success);
    EXPECT_EQ(state.peer_bidi_left, 0u);
    EXPECT_EQ(connection->write(first.stream_id, bytes({1}), false).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(connection->open_bidi().status, TransportStatus::StreamLimit);

    state.peer_bidi_left = 1;
    const auto after_max_streams = connection->open_bidi();
    EXPECT_EQ(after_max_streams.status, TransportStatus::Success);
    EXPECT_EQ(after_max_streams.stream_id, 13u);
}

TEST(QuicheConnectionWrite, UnidirectionalGapsConsumePeerCreditBySequence) {
    FakeApiState state;
    auto connection = make_connection(state, 8, 1024, 1, 2);
    const auto first = connection->open_uni();
    state.stream_send_result = QUICHE_ERR_INVALID_STREAM_STATE;
    EXPECT_EQ(connection->write(first.stream_id, bytes({1}), false).status,
              TransportStatus::InvalidState);
    const auto second = connection->open_uni();
    EXPECT_EQ(second.stream_id, 7u);
    EXPECT_EQ(connection->open_uni().status, TransportStatus::StreamLimit);

    state.stream_send_result = 1;
    EXPECT_EQ(connection->write(second.stream_id, bytes({2}), false).status,
              TransportStatus::Success);
    EXPECT_EQ(state.peer_uni_left, 0u);
    EXPECT_EQ(connection->open_uni().status, TransportStatus::StreamLimit);

    state.peer_uni_left = 1;
    EXPECT_EQ(connection->open_uni().stream_id, 11u);
}

TEST(QuicheConnectionWrite, RejectsUnreservedServerInitiatedStreamIds) {
    FakeApiState state;
    auto connection = make_connection(state);
    state.stream_send_result = 1;
    EXPECT_EQ(connection->write(5, bytes({1}), false).status,
              TransportStatus::InvalidState);
    EXPECT_TRUE(state.sent_bytes.empty());
    EXPECT_EQ(connection->write(0, bytes({1}), false).status,
              TransportStatus::Success);
}

TEST(QuicheConnectionShutdown, UsesExactWriteAndReadDirectionsAndErrorCodes) {
    FakeApiState state;
    auto connection = make_connection(state);

    state.shutdown_result = 0;
    EXPECT_EQ(connection->reset(13, 91).status, TransportStatus::Success);
    EXPECT_EQ(state.stream_id, 13u);
    EXPECT_EQ(state.shutdown_direction, QUICHE_SHUTDOWN_WRITE);
    EXPECT_EQ(state.shutdown_error, 91u);

    EXPECT_EQ(connection->stop_sending(17, 92).status,
              TransportStatus::Success);
    EXPECT_EQ(state.stream_id, 17u);
    EXPECT_EQ(state.shutdown_direction, QUICHE_SHUTDOWN_READ);
    EXPECT_EQ(state.shutdown_error, 92u);

    for (const auto& [raw, expected] : {
             std::pair<int, TransportStatus>{QUICHE_ERR_DONE,
                                             TransportStatus::InvalidState},
             std::pair<int, TransportStatus>{QUICHE_ERR_INVALID_STATE,
                                             TransportStatus::InvalidState},
             std::pair<int, TransportStatus>{
                 QUICHE_ERR_INVALID_STREAM_STATE,
                 TransportStatus::InvalidState},
             std::pair<int, TransportStatus>{QUICHE_ERR_STREAM_STOPPED,
                                             TransportStatus::PeerStopped},
             std::pair<int, TransportStatus>{QUICHE_ERR_STREAM_RESET,
                                             TransportStatus::PeerReset},
             std::pair<int, TransportStatus>{QUICHE_ERR_STREAM_LIMIT,
                                             TransportStatus::InternalError},
             std::pair<int, TransportStatus>{-999,
                                             TransportStatus::InternalError},
         }) {
        state.shutdown_result = raw;
        EXPECT_EQ(connection->reset(13, 1).status, expected) << raw;
        EXPECT_EQ(connection->stop_sending(17, 1).status, expected) << raw;
    }
}

TEST(QuicheConnectionDatagram, ChecksSizeBeforeSendAndMapsReturnValues) {
    FakeApiState state;
    auto connection = make_connection(state);
    const auto payload = bytes({1, 2, 3});

    state.datagram_max = 2;
    state.datagram_send_result = 3;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::DatagramTooLarge);
    EXPECT_TRUE(state.datagram_bytes.empty());

    state.datagram_max = 3;
    state.datagram_send_result = 3;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::Success);
    EXPECT_EQ(state.datagram_bytes, payload);

    state.datagram_send_result = 2;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::Partial);
    state.datagram_send_result = QUICHE_ERR_DONE;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::WouldBlock);
    state.datagram_send_result = 0;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::WouldBlock);
    state.datagram_send_result = QUICHE_ERR_BUFFER_TOO_SHORT;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::DatagramTooLarge);
    state.datagram_send_result = QUICHE_ERR_INVALID_STATE;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::InvalidState);
    state.datagram_send_result = -999;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::InternalError);

    state.datagram_max = QUICHE_ERR_DONE;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::InvalidState);
    state.datagram_max = QUICHE_ERR_INVALID_STATE;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::InvalidState);
    state.datagram_max = -999;
    EXPECT_EQ(connection->send_datagram(payload).status,
              TransportStatus::InternalError);
    EXPECT_EQ(connection->last_transport_diagnostic(),
              (TransportDiagnostic{TransportOperation::DatagramCapacity,
                                   -999}));
}

TEST(QuicheConnectionDatagram, PassesNonNullPointerForEmptyDatagram) {
    FakeApiState state;
    auto connection = make_connection(state);
    state.datagram_max = 3;
    state.datagram_send_result = 0;

    const auto result = connection->send_datagram({});
    EXPECT_EQ(result.status, TransportStatus::Success);
    EXPECT_EQ(result.accepted, 0u);
    EXPECT_TRUE(state.datagram_pointer_nonnull);
    EXPECT_TRUE(state.datagram_bytes.empty());
}

TEST(QuicheConnectionClose, MapsResultsAndOwnsNoCallerMemory) {
    FakeApiState state;
    auto connection = make_connection(state);
    auto reason = bytes({1, 0, 2});

    EXPECT_EQ(connection->close(71, reason).status, TransportStatus::Success);
    EXPECT_TRUE(state.close_application);
    EXPECT_EQ(state.close_error, 71u);
    EXPECT_EQ(state.close_reason, reason);

    state.close_result = QUICHE_ERR_DONE;
    EXPECT_EQ(connection->close(72, {}).status, TransportStatus::Success);
    EXPECT_TRUE(state.close_reason.empty());
    state.close_result = QUICHE_ERR_INVALID_STATE;
    EXPECT_EQ(connection->close(73, {}).status, TransportStatus::InvalidState);
    state.close_result = QUICHE_ERR_TLS_FAIL;
    EXPECT_EQ(connection->close(74, {}).status, TransportStatus::InternalError);
}

TEST(QuicheConnectionClose, RejectsCloseAfterTerminalEvidence) {
    FakeApiState state;
    auto connection = make_connection(state);
    ASSERT_TRUE(connection->notify_idle_timeout());
    EXPECT_EQ(connection->close(1, {}).status,
              TransportStatus::ConnectionClosed);
}

TEST(QuicheConnectionEvents, PreservesFifoAndOwnsPayloadAfterCallerMutation) {
    FakeApiState state;
    auto connection = make_connection(state);
    auto alpn = bytes({'m', 'o', 'q'});
    auto data = bytes({1, 2});
    auto datagram = bytes({3, 4, 5});

    EXPECT_TRUE(connection->notify_established(
        alpn, bytes({0, 1}), bytes({2, 0}), 1200));
    EXPECT_TRUE(connection->notify_stream_data(7, data, true));
    EXPECT_TRUE(connection->notify_datagram(datagram));
    alpn[0] = std::byte{0};
    data[0] = std::byte{0};
    datagram[0] = std::byte{0};

    auto first = connection->poll(2);
    ASSERT_EQ(first.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<ConnectionEstablishedEvent>(first[0]));
    EXPECT_EQ(std::get<ConnectionEstablishedEvent>(first[0]).alpn,
              bytes({'m', 'o', 'q'}));
    EXPECT_EQ(std::get<ConnectionEstablishedEvent>(first[0])
                  .local_connection_id,
              bytes({0, 1}));
    EXPECT_EQ(std::get<ConnectionEstablishedEvent>(first[0])
                  .peer_connection_id,
              bytes({2, 0}));
    EXPECT_EQ(std::get<ConnectionEstablishedEvent>(first[0])
                  .max_datagram_payload,
              1200u);
    ASSERT_TRUE(std::holds_alternative<StreamDataEvent>(first[1]));
    const auto& stream = std::get<StreamDataEvent>(first[1]);
    EXPECT_EQ(stream.stream_id, 7u);
    EXPECT_EQ(stream.data, bytes({1, 2}));
    EXPECT_TRUE(stream.fin);

    auto second = connection->poll(9);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(std::get<DatagramEvent>(second[0]).data, bytes({3, 4, 5}));
    EXPECT_TRUE(connection->poll(1).empty());
}

TEST(QuicheConnectionEvents, CountOverflowIsTerminalExactOnceAndPreservesQueue) {
    FakeApiState state;
    auto connection = make_connection(state, 2, 100);
    EXPECT_TRUE(connection->notify_peer_reset(1, 10));
    EXPECT_TRUE(connection->notify_peer_stop_sending(2, 11));
    EXPECT_FALSE(connection->notify_datagram(bytes({1})));
    EXPECT_FALSE(connection->notify_datagram(bytes({2})));

    auto events = connection->poll(10);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<PeerResetEvent>(events[0]));
    EXPECT_TRUE(std::holds_alternative<PeerStopSendingEvent>(events[1]));
    EXPECT_EQ(std::get<PeerResetEvent>(events[0]).stream_id, 1u);
    EXPECT_EQ(std::get<PeerResetEvent>(events[0]).application_error, 10u);
    EXPECT_EQ(std::get<PeerStopSendingEvent>(events[1]).stream_id, 2u);
    EXPECT_EQ(std::get<PeerStopSendingEvent>(events[1]).application_error,
              11u);
    EXPECT_TRUE(std::holds_alternative<EventQueueOverflowEvent>(events[2]));
    EXPECT_TRUE(connection->poll(10).empty());
    EXPECT_EQ(connection->open_bidi().status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(connection->write(1, bytes({1}), false).status,
              TransportStatus::ConnectionClosed);
}

TEST(QuicheConnectionEvents, ByteOverflowAccountsOnlyUndrainedOwnedPayload) {
    FakeApiState state;
    auto connection = make_connection(state, 10, 3);
    EXPECT_TRUE(connection->notify_datagram(bytes({1, 2, 3})));
    auto drained = connection->poll(1);
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_TRUE(connection->notify_datagram(bytes({4, 5, 6})));
    EXPECT_FALSE(connection->notify_stream_data(1, bytes({7}), false));

    auto events = connection->poll(10);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(std::get<DatagramEvent>(events[0]).data, bytes({4, 5, 6}));
    EXPECT_TRUE(std::holds_alternative<EventQueueOverflowEvent>(events[1]));
}

TEST(QuicheConnectionEvents, EmitsEachTerminalKindAtMostOnce) {
    FakeApiState state;
    for (const auto terminal : {0, 1, 2, 3}) {
        auto connection = make_connection(state);
        if (terminal == 0) {
            EXPECT_TRUE(connection->notify_peer_close(
                CloseErrorSpace::Application, 9, bytes({1})));
            EXPECT_FALSE(connection->notify_peer_close(
                CloseErrorSpace::Transport, 10, bytes({2})));
        } else if (terminal == 1) {
            EXPECT_TRUE(connection->notify_local_close(
                CloseErrorSpace::Application, 9, bytes({1})));
            EXPECT_FALSE(connection->notify_local_close(
                CloseErrorSpace::Transport, 10, bytes({2})));
        } else if (terminal == 2) {
            EXPECT_TRUE(connection->notify_idle_timeout());
            EXPECT_FALSE(connection->notify_idle_timeout());
        } else {
            EXPECT_TRUE(connection->notify_transport_error(
                TransportError::ProtocolFailure));
            EXPECT_FALSE(connection->notify_transport_error(
                TransportError::InternalFailure));
        }
        const auto events = connection->poll(8);
        ASSERT_EQ(events.size(), 1u);
        if (terminal == 0) {
            EXPECT_TRUE(std::holds_alternative<PeerCloseEvent>(events[0]));
            const auto& close = std::get<PeerCloseEvent>(events[0]);
            EXPECT_EQ(close.error_space, CloseErrorSpace::Application);
            EXPECT_EQ(close.error_code, 9u);
            EXPECT_EQ(close.reason, bytes({1}));
        } else if (terminal == 1) {
            EXPECT_TRUE(std::holds_alternative<LocalCloseEvent>(events[0]));
            const auto& close = std::get<LocalCloseEvent>(events[0]);
            EXPECT_EQ(close.error_space, CloseErrorSpace::Application);
            EXPECT_EQ(close.error_code, 9u);
            EXPECT_EQ(close.reason, bytes({1}));
        } else if (terminal == 2) {
            EXPECT_TRUE(std::holds_alternative<IdleTimeoutEvent>(events[0]));
        } else {
            EXPECT_TRUE(
                std::holds_alternative<TransportErrorEvent>(events[0]));
            EXPECT_EQ(std::get<TransportErrorEvent>(events[0]).error,
                      TransportError::ProtocolFailure);
        }
        EXPECT_TRUE(connection->poll(8).empty());
        const auto credit_calls = state.bidi_credit_calls;
        EXPECT_EQ(connection->open_bidi().status,
                  TransportStatus::ConnectionClosed);
        EXPECT_EQ(state.bidi_credit_calls, credit_calls);
        EXPECT_EQ(connection->send_datagram(bytes({1})).status,
                  TransportStatus::ConnectionClosed);
        EXPECT_EQ(connection->reset(1, 1).status,
                  TransportStatus::ConnectionClosed);
        EXPECT_EQ(connection->stop_sending(1, 1).status,
                  TransportStatus::ConnectionClosed);
    }
}

TEST(QuicheConnectionMove, MovedFromObjectRejectsEveryOperationSafely) {
    FakeApiState state;
    auto original = make_connection(state);
    QuicheConnection moved = std::move(*original);

    EXPECT_EQ(original->open_bidi().status, TransportStatus::InvalidState);
    EXPECT_EQ(original->open_uni().status, TransportStatus::InvalidState);
    EXPECT_EQ(original->write(1, bytes({1}), false).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(original->reset(1, 2).status, TransportStatus::InvalidState);
    EXPECT_EQ(original->stop_sending(1, 2).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(original->send_datagram(bytes({1})).status,
              TransportStatus::InvalidState);
    EXPECT_TRUE(original->poll(2).empty());

    EXPECT_EQ(moved.open_bidi().stream_id, 1u);
}

TEST(QuicheConnectionMove, FreesOwnedConnectionExactlyOnceAfterMove) {
    FakeApiState state;
    {
        auto original = make_connection(state);
        QuicheConnection moved = std::move(*original);
        EXPECT_EQ(state.free_calls, 0);
        EXPECT_EQ(moved.open_uni().stream_id, 3u);
    }
    EXPECT_EQ(state.free_calls, 1);
}

}  // namespace
}  // namespace moq::interop::transport::detail
