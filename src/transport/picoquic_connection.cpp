#include "picoquic_connection_internal.h"

#include <picoquic_internal.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace moq::interop::transport::detail {
namespace {

std::vector<std::byte> cid_bytes(picoquic_connection_id_t id) {
    std::vector<std::byte> result(id.id_len);
    if (id.id_len != 0) std::memcpy(result.data(), id.id, id.id_len);
    return result;
}

std::size_t payload_size(const TransportEvent& event) {
    if (const auto* ready = std::get_if<ConnectionEstablishedEvent>(&event)) {
        return ready->alpn.size() + ready->local_connection_id.size() +
               ready->peer_connection_id.size();
    }
    if (const auto* stream = std::get_if<StreamDataEvent>(&event)) return stream->data.size();
    if (const auto* datagram = std::get_if<DatagramEvent>(&event)) return datagram->data.size();
    if (const auto* close = std::get_if<PeerCloseEvent>(&event)) return close->reason.size();
    if (const auto* close = std::get_if<LocalCloseEvent>(&event)) return close->reason.size();
    return 0;
}

}  // namespace

OperationResult grant_peer_streams(picoquic_cnx_t* connection, bool bidirectional,
                                   std::uint64_t additional) {
    if (connection == nullptr || additional == 0 || additional > (std::uint64_t{1} << 40))
        return {TransportStatus::InvalidState, 0, std::nullopt};
    auto& limit = bidirectional ? connection->max_streams_bidir_local
                                : connection->max_streams_unidir_local;
    const auto raised = limit + additional;
    if (raised > (std::uint64_t{1} << 60)) return {TransportStatus::InvalidState, 0, std::nullopt};
    std::uint8_t frame[16];
    std::uint8_t* end = picoquic_frames_uint8_encode(
        frame, frame + sizeof(frame),
        bidirectional ? picoquic_frame_type_max_streams_bidir
                      : picoquic_frame_type_max_streams_unidir);
    if (end != nullptr) end = picoquic_frames_varint_encode(end, frame + sizeof(frame), raised);
    if (end == nullptr ||
        picoquic_queue_misc_frame(connection, frame, static_cast<std::size_t>(end - frame), 0,
                                  picoquic_packet_context_application) != 0)
        return {TransportStatus::InternalError, 0, std::nullopt};
    limit = raised;
    return {TransportStatus::Success, 0, std::nullopt};
}

int PicoquicConnectionState::callback(picoquic_cnx_t* connection,
                                      std::uint64_t stream_id,
                                      std::uint8_t* bytes, std::size_t length,
                                      picoquic_call_back_event_t event,
                                      void* context, void*) {
    if (context == nullptr) return -1;
    return static_cast<PicoquicConnectionState*>(context)->on_event(
        connection, stream_id, bytes, length, event);
}

int PicoquicConnectionState::on_event(picoquic_cnx_t* connection,
                                      std::uint64_t stream_id,
                                      std::uint8_t* bytes, std::size_t length,
                                      picoquic_call_back_event_t event) {
    if (event == picoquic_callback_ready) {
        if (terminal_ || (connection_ != nullptr && connection_ != connection)) {
            return picoquic_close(connection, 0x10);
        }
        connection_ = connection;
        if (established_) return 0;

        const char* negotiated = picoquic_tls_get_negotiated_alpn(connection);
        const std::string_view expected{
            reinterpret_cast<const char*>(config_.expected_alpn.data()),
            config_.expected_alpn.size()};
        const auto* remote = picoquic_get_transport_parameters(connection, 0);
        if (negotiated == nullptr || std::string_view{negotiated} != expected ||
            remote == nullptr || remote->max_datagram_frame_size == 0) {
            static constexpr std::string_view reason =
                "QUIC DATAGRAM not negotiated";
            const auto result = picoquic_close_ex(
                connection, config_.missing_datagram_application_error,
                reason.data());
            if (result == 0) {
                note_local_close(
                    config_.missing_datagram_application_error,
                    std::span{reinterpret_cast<const std::byte*>(reason.data()),
                              reason.size()});
            }
            return result;
        }
        established_ = true;
        ConnectionEstablishedEvent ready;
        ready.alpn = config_.expected_alpn;
        ready.local_connection_id =
            cid_bytes(picoquic_get_local_cnxid(connection));
        ready.peer_connection_id =
            cid_bytes(picoquic_get_remote_cnxid(connection));
        ready.max_datagram_payload = std::min<std::size_t>(
            {remote->max_datagram_frame_size,
             config_.max_udp_payload - 50, 1150});
        max_datagram_payload_ = ready.max_datagram_payload;
        enqueue(std::move(ready));
    } else if (terminal_ || connection_ != connection) {
        return 0;
    } else if (event == picoquic_callback_stream_data ||
               event == picoquic_callback_stream_fin) {
        if (connection_ != connection) return -1;
        if (!reserve_event(length)) return 0;
        StreamDataEvent received;
        received.stream_id = stream_id;
        received.data.resize(length);
        if (length > 0) std::memcpy(received.data.data(), bytes, length);
        received.fin = event == picoquic_callback_stream_fin;
        enqueue(std::move(received));
    } else if (event == picoquic_callback_stream_reset) {
        if (connection_ != connection) return -1;
        enqueue(PeerResetEvent{
            stream_id, picoquic_get_remote_stream_error(connection, stream_id)});
    } else if (event == picoquic_callback_datagram) {
        if (connection_ != connection) return -1;
        if (!reserve_event(length)) return 0;
        DatagramEvent received;
        received.data.resize(length);
        if (length > 0) std::memcpy(received.data.data(), bytes, length);
        enqueue(std::move(received));
    } else if (event == picoquic_callback_stop_sending) {
        const auto* stream = picoquic_find_stream(connection, stream_id);
        if (stream == nullptr) return -1;
        enqueue(PeerStopSendingEvent{stream_id, stream->remote_stop_error});
    } else if (event == picoquic_callback_close ||
               event == picoquic_callback_application_close ||
               event == picoquic_callback_stateless_reset) {
        if (connection_ == connection) {
            if (!local_close_) {
                if (event == picoquic_callback_close &&
                    connection->local_error == PICOQUIC_ERROR_IDLE_TIMEOUT) {
                    enqueue(IdleTimeoutEvent{});
                    terminal_ = true;
                    connection_ = nullptr;
                    established_ = false;
                    return 0;
                }
                if (event == picoquic_callback_close && !peer_transport_close_) {
                    enqueue(TransportErrorEvent{TransportError::ProtocolFailure});
                    terminal_ = true;
                    connection_ = nullptr;
                    established_ = false;
                    max_datagram_payload_ = 0;
                    return 0;
                }
                std::vector<std::byte> reason;
                if (connection->remote_error_reason != nullptr) {
                    const auto length =
                        std::strlen(connection->remote_error_reason);
                    if (!reserve_event(length)) return 0;
                    reason.resize(length);
                    if (length != 0) {
                        std::memcpy(reason.data(),
                                    connection->remote_error_reason, length);
                    }
                }
                enqueue(PeerCloseEvent{
                    event == picoquic_callback_application_close
                        ? CloseErrorSpace::Application
                        : CloseErrorSpace::Transport,
                    event == picoquic_callback_application_close
                        ? picoquic_get_application_error(connection)
                        : picoquic_get_remote_error(connection),
                    std::move(reason)});
            }
            terminal_ = true;
            connection_ = nullptr;
            established_ = false;
            max_datagram_payload_ = 0;
        }
    }
    return 0;
}

void PicoquicConnectionState::observe_transport_state() {
    if (terminal_ || connection_ == nullptr || !established_) return;
    // A received transport CLOSE has no dedicated callback until disconnect.
    // Record the actual receive transition, including code zero/empty reason.
    if (connection_->cnx_state == picoquic_state_closing_received) {
        peer_transport_close_ = true;
        on_event(connection_, 0, nullptr, 0, picoquic_callback_close);
    } else if (connection_->local_error != 0 &&
               connection_->local_error != PICOQUIC_ERROR_IDLE_TIMEOUT) {
        enqueue(TransportErrorEvent{TransportError::ProtocolFailure});
        terminal_ = true;
    }
}

void PicoquicConnectionState::overflow() {
    if (terminal_) return;
    terminal_ = true;
    events_.emplace_back(EventQueueOverflowEvent{});
    if (connection_ != nullptr) {
        picoquic_connection_error_ex(connection_, 1, 0, "event queue overflow");
    }
}

bool PicoquicConnectionState::reserve_event(std::size_t payload) {
    if (terminal_) return false;
    if (events_.size() >= config_.max_events ||
        payload > config_.max_event_payload_bytes - event_payload_bytes_) {
        overflow();
        return false;
    }
    return true;
}

void PicoquicConnectionState::enqueue(TransportEvent event) {
    const auto payload = payload_size(event);
    if (!reserve_event(payload)) return;
    event_payload_bytes_ += payload;
    events_.push_back(std::move(event));
}

std::vector<TransportEvent> PicoquicConnectionState::drain(
    std::size_t max_events) {
    std::vector<TransportEvent> result;
    while (!events_.empty() && result.size() < max_events) {
        event_payload_bytes_ -= payload_size(events_.front());
        result.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return result;
}

void PicoquicConnectionState::fail() {
    if (terminal_) return;
    enqueue(TransportErrorEvent{TransportError::InternalFailure});
    terminal_ = true;
    if (connection_ != nullptr) picoquic_connection_error(connection_, 1, 0);
}

void PicoquicConnectionState::note_local_close(
    std::uint64_t application_error, std::span<const std::byte> reason) {
    if (terminal_) return;
    local_close_ = true;
    enqueue(LocalCloseEvent{CloseErrorSpace::Application, application_error,
                            {reason.begin(), reason.end()}});
    terminal_ = true;
}

}  // namespace moq::interop::transport::detail
