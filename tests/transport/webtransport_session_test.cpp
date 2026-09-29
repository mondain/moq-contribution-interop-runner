#include "transport/webtransport_session.h"

#include <gtest/gtest.h>

#include <h3zero.h>

#include <limits>

using namespace moq::interop::transport;

namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

int ignore_payload(picoquic_cnx_t*, std::uint8_t*, std::size_t,
                   picohttp_call_back_event_t, h3zero_stream_ctx_t*, void*) {
    return 0;
}

h3zero_stream_ctx_t stream_context(std::uint64_t stream_id) {
    h3zero_stream_ctx_t stream{};
    stream.stream_id = stream_id;
    stream.ps.stream_state.stream_type = std::numeric_limits<std::uint64_t>::max();
    stream.ps.stream_state.control_stream_id = std::numeric_limits<std::uint64_t>::max();
    return stream;
}

TEST(WebTransportSession, DeliversOnlyMatchingSessionPayload) {
    WebTransportSession session(4, {.max_events = 4, .max_event_payload_bytes = 8,
                                    .max_datagram_payload = 4});
    EXPECT_FALSE(session.ingest_stream(8, 12, bytes({1}), false));
    EXPECT_FALSE(session.ingest_stream(4, 4, bytes({1}), false));
    EXPECT_FALSE(session.ingest_stream(11, 4, bytes({1}), false));
    EXPECT_TRUE(session.ingest_stream(10, 4, bytes({7}), false));
    const auto uni = session.poll(1);
    ASSERT_EQ(uni.size(), 1u);
    EXPECT_EQ(std::get<StreamDataEvent>(uni[0]).stream_id, 10u);
    EXPECT_TRUE(session.poll(1).empty());
    EXPECT_TRUE(session.ingest_stream(8, 4, bytes({1, 2}), false));
    const auto first = session.poll(1);
    ASSERT_EQ(first.size(), 1u);
    const auto* data = std::get_if<StreamDataEvent>(&first[0]);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->stream_id, 8u);
    EXPECT_EQ(data->data, bytes({1, 2}));
    EXPECT_FALSE(data->fin);
    EXPECT_TRUE(session.ingest_stream(8, 4, {}, true));
    const auto second = session.poll(1);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_TRUE(std::get<StreamDataEvent>(second[0]).fin);
    EXPECT_FALSE(session.ingest_stream(8, 4, bytes({3}), false));
}

TEST(WebTransportSession, BoundsIncomingPayloadAndDatagrams) {
    WebTransportSession session(4, {.max_events = 2, .max_event_payload_bytes = 4,
                                    .max_datagram_payload = 3});
    EXPECT_FALSE(session.ingest_stream(8, 4, bytes({1, 2, 3, 4, 5}), false));
    EXPECT_FALSE(session.ingest_datagram(8, bytes({1})));
    EXPECT_FALSE(session.ingest_datagram(4, bytes({1, 2, 3, 4})));
    EXPECT_TRUE(session.poll(4).empty());
    EXPECT_TRUE(session.ingest_datagram(4, bytes({1, 2, 3})));
    const auto events = session.poll(1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(std::get<DatagramEvent>(events[0]).data, bytes({1, 2, 3}));
}

TEST(WebTransportSession, OverflowAndCloseAreIsolated) {
    WebTransportSession first(4, {.max_events = 1, .max_event_payload_bytes = 4,
                                  .max_datagram_payload = 3});
    WebTransportSession second(8, {.max_events = 1, .max_event_payload_bytes = 4,
                                   .max_datagram_payload = 3});
    EXPECT_TRUE(first.ingest_stream(12, 4, bytes({1}), false));
    EXPECT_TRUE(first.ingest_stream(12, 4, bytes({2}), false));
    const auto overflow = first.poll(1);
    ASSERT_EQ(overflow.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<EventQueueOverflowEvent>(overflow[0]));
    EXPECT_TRUE(second.ingest_stream(16, 8, bytes({3}), false));
    const auto delivered = second.poll(1);
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_EQ(std::get<StreamDataEvent>(delivered[0]).data, bytes({3}));
    second.detach();
    EXPECT_FALSE(second.ingest_stream(16, 8, bytes({4}), false));
}

TEST(WebTransportSession, EstablishAndPeerCloseHaveBoundedEvidence) {
    WebTransportSession session(4, {.max_events = 3, .max_event_payload_bytes = 8,
                                    .max_datagram_payload = 3});
    session.establish(bytes({'m', 'o', 'q', 't', '-', '2', '1'}), bytes({1}), bytes({2}));
    session.ingest_peer_close(7, bytes({'b', 'y', 'e'}));
    const auto events = session.poll(2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(std::get<ConnectionEstablishedEvent>(events[0]).max_datagram_payload, 3u);
    EXPECT_EQ(std::get<PeerCloseEvent>(events[1]).error_code, 7u);
    EXPECT_FALSE(session.ingest_datagram(4, bytes({1})));
}

TEST(WebTransportSession, TransportCloseRetainsErrorSpaceAndCode) {
    WebTransportSession session(4, {.max_events = 3});
    session.ingest_connection_close(CloseErrorSpace::Transport, 0x42,
                                    bytes({'q', 'u', 'i', 'c'}));
    const auto events = session.poll(2);
    ASSERT_EQ(events.size(), 1u);
    const auto& close = std::get<PeerCloseEvent>(events[0]);
    EXPECT_EQ(close.error_space, CloseErrorSpace::Transport);
    EXPECT_EQ(close.error_code, 0x42u);
    EXPECT_EQ(close.reason, bytes({'q', 'u', 'i', 'c'}));
    EXPECT_FALSE(session.ingest_datagram(4, bytes({1})));
}

TEST(WebTransportSession, MapsApplicationStreamErrors) {
    EXPECT_EQ(webtransport_to_http_error(0), 0x52e4a40fa8dbULL);
    EXPECT_EQ(webtransport_to_http_error(0x1e), 0x52e4a40fa8faULL);
    EXPECT_EQ(webtransport_from_http_error(0x52e4a40fa8dbULL), 0u);
    EXPECT_FALSE(webtransport_from_http_error(0x52e4a40fa8dbULL - 1).has_value());
    WebTransportSession session(4, {.max_events = 4});
    session.ingest_reset(8, webtransport_to_http_error(17));
    session.ingest_stop_sending(12, 0x10);
    const auto events = session.poll(2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(std::get<PeerResetEvent>(events[0]).application_error, 17u);
    EXPECT_FALSE(std::get<PeerStopSendingEvent>(events[1]).application_error.has_value());
}

TEST(WebTransportSession, CloseReasonRequiresBoundedUtf8) {
    EXPECT_TRUE(valid_webtransport_close_reason(bytes({'o', 'k'})));
    EXPECT_TRUE(valid_webtransport_close_reason(bytes({0xf0, 0x9f, 0x8c, 0x8d})));
    EXPECT_FALSE(valid_webtransport_close_reason(bytes({0})));
    EXPECT_FALSE(valid_webtransport_close_reason(bytes({0xc0, 0xaf})));
    EXPECT_FALSE(valid_webtransport_close_reason(bytes({0xed, 0xa0, 0x80})));
    EXPECT_FALSE(valid_webtransport_close_reason(bytes({0xf4, 0x90, 0x80, 0x80})));
    EXPECT_FALSE(valid_webtransport_close_reason(bytes({0xe2, 0x82})));
    EXPECT_TRUE(valid_webtransport_close_reason(std::vector<std::byte>(1024, std::byte{'a'})));
    EXPECT_FALSE(valid_webtransport_close_reason(std::vector<std::byte>(1025, std::byte{'a'})));
}

TEST(WebTransportSession, H3zeroStripsFragmentedBidiAndUniPrefixes) {
    auto* h3 = h3zero_callback_create_context(nullptr);
    ASSERT_NE(h3, nullptr);
    ASSERT_EQ(h3zero_declare_stream_prefix(h3, 0, ignore_payload, nullptr), 0);

    auto bidi = stream_context(4);
    const std::uint8_t bidi_first[] = {0x40};
    EXPECT_EQ(h3zero_parse_incoming_remote_stream(bidi_first,
        bidi_first + sizeof(bidi_first), &bidi, h3, nullptr),
        bidi_first + sizeof(bidi_first));
    const std::uint8_t bidi_rest[] = {0x41, 0x00, 'm'};
    EXPECT_EQ(h3zero_parse_incoming_remote_stream(bidi_rest,
        bidi_rest + sizeof(bidi_rest), &bidi, h3, nullptr), bidi_rest + 2);
    EXPECT_EQ(bidi.ps.stream_state.control_stream_id, 0u);
    EXPECT_EQ(bidi.path_callback, ignore_payload);

    auto uni = stream_context(2);
    const std::uint8_t uni_first[] = {0x40};
    EXPECT_EQ(h3zero_parse_incoming_remote_stream(uni_first,
        uni_first + sizeof(uni_first), &uni, h3, nullptr),
        uni_first + sizeof(uni_first));
    const std::uint8_t uni_rest[] = {0x54, 0x00, 'q'};
    EXPECT_EQ(h3zero_parse_incoming_remote_stream(uni_rest,
        uni_rest + sizeof(uni_rest), &uni, h3, nullptr), uni_rest + 2);
    EXPECT_EQ(uni.ps.stream_state.control_stream_id, 0u);

    auto wrong_session = stream_context(8);
    const std::uint8_t wrong[] = {0x40, 0x41, 0x04, 'x'};
    EXPECT_EQ(h3zero_parse_incoming_remote_stream(wrong,
        wrong + sizeof(wrong), &wrong_session, h3, nullptr), nullptr);

    auto nonminimal = stream_context(12);
    const std::uint8_t nonminimal_id[] = {0x40, 0x41, 0x40, 0x00, 'y'};
    EXPECT_EQ(h3zero_parse_incoming_remote_stream(nonminimal_id,
        nonminimal_id + sizeof(nonminimal_id), &nonminimal, h3, nullptr),
        nonminimal_id + 4);
    EXPECT_EQ(nonminimal.ps.stream_state.control_stream_id, 0u);
    // H3zero's deregistration callback requires a live connection. This
    // parser-only fixture has none, so suppress deregistration on teardown.
    h3zero_find_stream_prefix(h3, 0)->function_call = nullptr;
    h3zero_callback_delete_context(nullptr, h3);
}

}  // namespace
