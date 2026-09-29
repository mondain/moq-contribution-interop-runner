#include "picoquic_connection_internal.h"

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

}  // namespace

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
        if (connection_ != nullptr && connection_ != connection) {
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
            enqueue(TransportErrorEvent{TransportError::ProtocolFailure});
            return picoquic_close(connection,
                                  config_.missing_datagram_application_error);
        }
        established_ = true;
        ConnectionEstablishedEvent ready;
        ready.alpn = config_.expected_alpn;
        ready.local_connection_id =
            cid_bytes(picoquic_get_local_cnxid(connection));
        ready.peer_connection_id =
            cid_bytes(picoquic_get_remote_cnxid(connection));
        ready.max_datagram_payload = std::min<std::size_t>(
            remote->max_datagram_frame_size, config_.max_udp_payload);
        enqueue(std::move(ready));
    } else if (event == picoquic_callback_stream_data ||
               event == picoquic_callback_stream_fin) {
        if (connection_ != connection) return -1;
        if (length > config_.max_event_payload_bytes) {
            enqueue(TransportErrorEvent{TransportError::ProtocolFailure});
            return -1;
        }
        StreamDataEvent received;
        received.stream_id = stream_id;
        received.data.resize(length);
        if (length > 0) std::memcpy(received.data.data(), bytes, length);
        received.fin = event == picoquic_callback_stream_fin;
        enqueue(std::move(received));
    } else if (event == picoquic_callback_close ||
               event == picoquic_callback_application_close ||
               event == picoquic_callback_stateless_reset) {
        if (connection_ == connection) {
            enqueue(PeerCloseEvent{
                event == picoquic_callback_application_close
                    ? CloseErrorSpace::Application
                    : CloseErrorSpace::Transport,
                picoquic_get_remote_error(connection), {}});
            connection_ = nullptr;
            established_ = false;
        }
    }
    return 0;
}

void PicoquicConnectionState::enqueue(TransportEvent event) {
    if (overflowed_) return;
    if (events_.size() >= config_.max_events) {
        events_.clear();
        events_.emplace_back(EventQueueOverflowEvent{});
        overflowed_ = true;
        return;
    }
    events_.push_back(std::move(event));
}

std::vector<TransportEvent> PicoquicConnectionState::drain(
    std::size_t max_events) {
    std::vector<TransportEvent> result;
    while (!events_.empty() && result.size() < max_events) {
        result.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return result;
}

void PicoquicConnectionState::fail() {
    enqueue(TransportErrorEvent{TransportError::InternalFailure});
}

}  // namespace moq::interop::transport::detail
