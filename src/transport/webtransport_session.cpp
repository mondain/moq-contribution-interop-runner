#include "transport/webtransport_session.h"
#include "picoquic_connection_internal.h"

#include <pico_webtransport.h>
#include <picoquic_internal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace moq::interop::transport {
namespace {

constexpr std::uint64_t kFirstWtApplicationError = 0x52e4a40fa8dbULL;
constexpr std::uint64_t kLastWtApplicationError = 0x52e5ac983162ULL;

std::array<std::uint8_t, 8> encode_varint(std::uint64_t value,
                                          std::size_t& length) {
    std::array<std::uint8_t, 8> bytes{};
    auto* end = picoquic_frames_varint_encode(bytes.data(), bytes.data() + bytes.size(),
                                               value);
    length = end == nullptr ? 0 : static_cast<std::size_t>(end - bytes.data());
    return bytes;
}

// Resets the sending half of a WebTransport stream. picowt_reset_stream sends RESET_STREAM_AT with a reliable
// size covering the stream header of a local stream, which picoquic refuses (PICOQUIC_ERROR_ILLEGAL_TRANSPORT_
// EXTENSION) unless both endpoints enabled RESET_STREAM_AT. Only the moq-lite profile admits a client without it
// (validate_connect requires it of every MoQ Transport client), and there the stream is reset with a plain
// RESET_STREAM instead. MoQ Transport sessions always negotiate RESET_STREAM_AT, so their path is unchanged.
// A plain RESET_STREAM does not guarantee delivery of the WebTransport stream header (the reliable size is what
// RESET_STREAM_AT protects), so a peer that never received it cannot attribute the reset to the session. That is
// fine for the moq-lite probes, which reset only established streams whose header and first bytes were sent.
int reset_stream(picoquic_cnx_t* connection, h3zero_stream_ctx_t* stream, std::uint64_t wire_error) {
    if (connection->is_reset_stream_at_enabled) return picowt_reset_stream(connection, stream, wire_error);
    const int result = picoquic_reset_stream(connection, stream->stream_id, wire_error);
    if (result == 0) stream->ps.stream_state.is_fin_sent = 1;
    return result;
}

OperationResult unavailable() { return {TransportStatus::InvalidState, 0, std::nullopt}; }
OperationResult failed() { return {TransportStatus::InternalError, 0, std::nullopt}; }

}  // namespace

std::uint64_t webtransport_to_http_error(std::uint32_t code) {
    return kFirstWtApplicationError + code + code / 0x1eU;
}

std::optional<std::uint32_t> webtransport_from_http_error(std::uint64_t code) {
    if (code < kFirstWtApplicationError || code > kLastWtApplicationError ||
        (code - 0x21U) % 0x1fU == 0) return std::nullopt;
    const auto shifted = code - kFirstWtApplicationError;
    return static_cast<std::uint32_t>(shifted - shifted / 0x1fU);
}

bool valid_webtransport_close_reason(std::span<const std::byte> reason) {
    if (reason.size() > 1024) return false;
    const auto value = [&](std::size_t index) {
        return static_cast<unsigned>(reason[index]);
    };
    const auto continuation = [&](std::size_t index) {
        return index < reason.size() && value(index) >= 0x80 && value(index) <= 0xbf;
    };
    for (std::size_t i = 0; i < reason.size();) {
        const auto first = value(i);
        if (first == 0) return false;
        if (first < 0x80) { ++i; continue; }
        if (first >= 0xc2 && first <= 0xdf && continuation(i + 1)) {
            i += 2;
            continue;
        }
        if (first >= 0xe0 && first <= 0xef && i + 2 < reason.size() &&
            continuation(i + 1) && continuation(i + 2) &&
            (first != 0xe0 || value(i + 1) >= 0xa0) &&
            (first != 0xed || value(i + 1) <= 0x9f)) {
            i += 3;
            continue;
        }
        if (first >= 0xf0 && first <= 0xf4 && i + 3 < reason.size() &&
            continuation(i + 1) && continuation(i + 2) && continuation(i + 3) &&
            (first != 0xf0 || value(i + 1) >= 0x90) &&
            (first != 0xf4 || value(i + 1) <= 0x8f)) {
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

WebTransportSession::WebTransportSession(StreamId connect_stream_id,
                                         WebTransportSessionLimits limits,
                                         picoquic_cnx_t* connection,
                                         h3zero_callback_ctx_t* h3,
                                         h3zero_stream_ctx_t* control,
                                         picohttp_post_data_cb_fn stream_callback,
                                         void* stream_callback_context)
    : connect_stream_id_(connect_stream_id), limits_(limits),
      connection_(connection), h3_(h3), control_(control),
      stream_callback_(stream_callback),
      stream_callback_context_(stream_callback_context) {}

StreamId WebTransportSession::connect_stream_id() const noexcept {
    return connect_stream_id_;
}

void WebTransportSession::establish(
    std::vector<std::byte> application_protocol,
    std::vector<std::byte> local_connection_id,
    std::vector<std::byte> peer_connection_id) {
    if (detached_) return;
    enqueue(ConnectionEstablishedEvent{std::move(application_protocol),
                                       std::move(local_connection_id),
                                       std::move(peer_connection_id),
                                       limits_.max_datagram_payload});
}

bool WebTransportSession::ingest_stream(StreamId stream_id,
                                        StreamId source_session_id,
                                        std::span<const std::byte> payload,
                                        bool fin) {
    if (detached_ || source_session_id != connect_stream_id_ ||
        finished_read_streams_.contains(stream_id) ||
        payload.size() > limits_.max_event_payload_bytes) return false;
    if (stream_id == connect_stream_id_) return false;
    // Stream id bits: 0 client bidi, 1 server bidi, 2 client uni, 3 server uni. The peer opens bidi and uni streams of
    // the other side's kind; a bidi stream of ours is readable only once we opened it.
    const auto direction = stream_id & 3U;
    const auto own_uni = limits_.client_role ? 2U : 3U;
    const auto own_bidi = limits_.client_role ? 0U : 1U;
    if (direction == own_uni || (direction == own_bidi && !readable_streams_.contains(stream_id)))
        return false;
    readable_streams_.insert(stream_id);
    if (direction == (limits_.client_role ? 1U : 0U)) writable_streams_.insert(stream_id);
    enqueue(StreamDataEvent{stream_id, {payload.begin(), payload.end()}, fin});
    if (fin) finished_read_streams_.insert(stream_id);
    return true;
}

bool WebTransportSession::ingest_datagram(StreamId source_session_id,
                                          std::span<const std::byte> payload) {
    if (detached_ || source_session_id != connect_stream_id_ ||
        payload.size() > (limits_.max_received_datagram_payload != 0 ? limits_.max_received_datagram_payload
                                                                      : limits_.max_datagram_payload) ||
        payload.size() > limits_.max_event_payload_bytes) return false;
    enqueue(DatagramEvent{{payload.begin(), payload.end()}});
    return true;
}

void WebTransportSession::ingest_reset(StreamId stream_id,
                                       std::uint64_t wire_error) {
    if (detached_) return;
    const auto app_error = webtransport_from_http_error(wire_error);
    enqueue(PeerResetEvent{stream_id, app_error});
    finished_read_streams_.insert(stream_id);
}

void WebTransportSession::ingest_stop_sending(StreamId stream_id,
                                              std::uint64_t wire_error) {
    if (detached_) return;
    const auto app_error = webtransport_from_http_error(wire_error);
    enqueue(PeerStopSendingEvent{stream_id, app_error});
    finished_streams_.insert(stream_id);
}

void WebTransportSession::ingest_peer_close(
    std::uint64_t application_error, std::span<const std::byte> reason) {
    ingest_connection_close(CloseErrorSpace::Application, application_error, reason);
}

void WebTransportSession::ingest_connection_close(
    CloseErrorSpace space, std::uint64_t error, std::span<const std::byte> reason) {
    if (detached_) return;
    if (reason.size() > limits_.max_event_payload_bytes)
        enqueue(EventQueueOverflowEvent{});
    else
        enqueue(PeerCloseEvent{space, error, {reason.begin(), reason.end()}});
    detach();
}

void WebTransportSession::detach() noexcept {
    terminate_streams();
    detached_ = true;
    connection_ = nullptr;
    h3_ = nullptr;
    control_ = nullptr;
}

void WebTransportSession::terminate_streams() {
    if (connection_ == nullptr || h3_ == nullptr) return;
    constexpr std::uint64_t session_gone = 0x170d7b68ULL;
    for (const auto stream_id : writable_streams_) {
        if (finished_streams_.contains(stream_id)) continue;
        auto* stream = h3zero_find_stream(h3_, stream_id);
        if (stream != nullptr) (void)reset_stream(connection_, stream, session_gone);
    }
    for (const auto stream_id : readable_streams_)
        (void)picoquic_stop_sending(connection_, stream_id, session_gone);
}

void WebTransportSession::enqueue(TransportEvent event) {
    if (overflowed_ || detached_) return;
    if (events_.size() >= limits_.max_events) {
        events_.clear();
        events_.emplace_back(EventQueueOverflowEvent{});
        overflowed_ = true;
        return;
    }
    events_.push_back(std::move(event));
}

std::size_t WebTransportSession::queued_stream_bytes() const {
    std::size_t queued = 0;
    for (const auto stream_id : writable_streams_) {
        const auto* stream = picoquic_find_stream(connection_, stream_id);
        if (stream == nullptr) continue;
        for (auto* node = stream->send_queue; node != nullptr;
             node = node->next_stream_data) {
            if (node->length > limits_.max_queued_send_bytes - queued)
                return limits_.max_queued_send_bytes;
            queued += node->length;
        }
    }
    return queued;
}

OpenResult WebTransportSession::open(bool bidirectional) {
    if (detached_ || connection_ == nullptr || h3_ == nullptr)
        return {TransportStatus::InvalidState, 0};
    auto* stream = picowt_create_local_stream(connection_, bidirectional ? 1 : 0,
                                               h3_, connect_stream_id_);
    if (stream == nullptr) return {TransportStatus::StreamLimit, 0};
    stream->path_callback = stream_callback_;
    stream->path_callback_ctx = stream_callback_context_;
    writable_streams_.insert(stream->stream_id);
    if (bidirectional) readable_streams_.insert(stream->stream_id);
    return {TransportStatus::Success, stream->stream_id};
}

OpenResult WebTransportSession::open_bidi() { return open(true); }
OpenResult WebTransportSession::open_uni() { return open(false); }

OperationResult WebTransportSession::write(StreamId stream_id,
                                           std::span<const std::byte> data,
                                           bool fin) {
    if (detached_ || connection_ == nullptr || !writable_streams_.contains(stream_id) ||
        finished_streams_.contains(stream_id)) return unavailable();
    if (data.size() > limits_.max_queued_send_bytes - queued_stream_bytes())
        return {TransportStatus::WouldBlock, 0, std::nullopt};
    auto* stream = h3zero_find_stream(h3_, stream_id);
    if (stream == nullptr) return unavailable();
    static constexpr std::uint8_t empty = 0;
    const auto* pointer = data.empty() ? &empty :
        reinterpret_cast<const std::uint8_t*>(data.data());
    if (picoquic_add_to_stream_with_ctx(connection_, stream_id, pointer, data.size(),
                                        fin ? 1 : 0, stream) != 0) return failed();
    if (fin) finished_streams_.insert(stream_id);
    return {TransportStatus::Success, data.size(), std::nullopt};
}

OperationResult WebTransportSession::reset(StreamId stream_id,
                                           std::uint64_t application_error) {
    if (detached_ || connection_ == nullptr || !writable_streams_.contains(stream_id) ||
        finished_streams_.contains(stream_id) ||
        application_error > std::numeric_limits<std::uint32_t>::max()) return unavailable();
    auto* stream = h3zero_find_stream(h3_, stream_id);
    if (stream == nullptr) return unavailable();
    if (reset_stream(connection_, stream,
                     webtransport_to_http_error(static_cast<std::uint32_t>(application_error))) != 0)
        return failed();
    finished_streams_.insert(stream_id);
    return {TransportStatus::Success, 0, std::nullopt};
}

OperationResult WebTransportSession::stop_sending(StreamId stream_id,
                                                  std::uint64_t application_error) {
    if (detached_ || connection_ == nullptr || !readable_streams_.contains(stream_id) ||
        application_error > std::numeric_limits<std::uint32_t>::max()) return unavailable();
    if (picoquic_stop_sending(connection_, stream_id,
                              webtransport_to_http_error(
                                  static_cast<std::uint32_t>(application_error))) != 0)
        return failed();
    return {TransportStatus::Success, 0, std::nullopt};
}

OperationResult WebTransportSession::grant_peer_streams(bool bidirectional,
                                                        std::uint64_t additional) {
    if (detached_ || connection_ == nullptr) return unavailable();
    return detail::grant_peer_streams(connection_, bidirectional, additional);
}

OperationResult WebTransportSession::send_datagram(
    std::span<const std::byte> data) {
    if (detached_ || connection_ == nullptr) return unavailable();
    if (data.size() > limits_.max_datagram_payload)
        return {TransportStatus::DatagramTooLarge, 0, std::nullopt};
    std::size_t prefix_length = 0;
    const auto prefix = encode_varint(connect_stream_id_ / 4, prefix_length);
    if (prefix_length == 0) return failed();
    std::vector<std::uint8_t> payload;
    payload.reserve(prefix_length + data.size());
    payload.insert(payload.end(), prefix.begin(), prefix.begin() +
                   static_cast<std::ptrdiff_t>(prefix_length));
    for (const auto byte : data) payload.push_back(static_cast<std::uint8_t>(byte));
    if (picoquic_queue_datagram_frame(connection_, payload.size(),
                                      payload.data()) != 0) return failed();
    return {TransportStatus::Success, data.size(), std::nullopt};
}

OperationResult WebTransportSession::close(std::uint64_t application_error,
                                           std::span<const std::byte> reason) {
    if (detached_ || connection_ == nullptr || control_ == nullptr ||
        application_error > std::numeric_limits<std::uint32_t>::max() ||
        !valid_webtransport_close_reason(reason))
        return unavailable();
    std::array<std::uint8_t, 1028> payload{};
    const auto code = static_cast<std::uint32_t>(application_error);
    payload[0] = static_cast<std::uint8_t>(code >> 24);
    payload[1] = static_cast<std::uint8_t>(code >> 16);
    payload[2] = static_cast<std::uint8_t>(code >> 8);
    payload[3] = static_cast<std::uint8_t>(code);
    for (std::size_t i = 0; i < reason.size(); ++i)
        payload[4 + i] = static_cast<std::uint8_t>(reason[i]);
    if (h3zero_send_capsule(connection_, control_,
                            picowt_capsule_close_webtransport_session,
                            4 + reason.size(), payload.data(), 1) != 0) return failed();
    enqueue(LocalCloseEvent{CloseErrorSpace::Application, application_error,
                            {reason.begin(), reason.end()}});
    detach();
    return {TransportStatus::Success, 0, std::nullopt};
}

std::vector<TransportEvent> WebTransportSession::poll(std::size_t max_events) {
    std::vector<TransportEvent> result;
    while (!events_.empty() && result.size() < max_events) {
        result.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return result;
}

}  // namespace moq::interop::transport
