#pragma once

#include "moq/interop/transport/native_quic_listener.h"

#include <picoquic.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace moq::interop::transport::detail {

class PicoquicConnectionState {
public:
    explicit PicoquicConnectionState(const NativeQuicListenerConfig& config)
        : config_(config) {}

    static int callback(picoquic_cnx_t* connection, std::uint64_t stream_id,
                        std::uint8_t* bytes, std::size_t length,
                        picoquic_call_back_event_t event, void* context,
                        void* stream_context);

    std::vector<TransportEvent> drain(std::size_t max_events);
    picoquic_cnx_t* connection() const noexcept { return connection_; }
    std::size_t max_datagram_payload() const noexcept {
        return max_datagram_payload_;
    }
    TransportStatus application_status() const noexcept {
        return terminal_ ? TransportStatus::ConnectionClosed
                         : established_ && connection_ != nullptr
                               ? TransportStatus::Success
                               : TransportStatus::InvalidState;
    }
    void observe_transport_state();
    void fail();
    void note_local_close(std::uint64_t application_error,
                          std::span<const std::byte> reason);

private:
    int on_event(picoquic_cnx_t* connection, std::uint64_t stream_id,
                 std::uint8_t* bytes, std::size_t length,
                 picoquic_call_back_event_t event);
    void enqueue(TransportEvent event);
    bool reserve_event(std::size_t payload);
    void overflow();

    const NativeQuicListenerConfig& config_;
    picoquic_cnx_t* connection_ = nullptr;
    std::deque<TransportEvent> events_;
    bool terminal_ = false;
    std::size_t event_payload_bytes_ = 0;
    bool established_ = false;
    bool local_close_ = false;
    bool peer_transport_close_ = false;
    std::size_t max_datagram_payload_ = 0;
};

}  // namespace moq::interop::transport::detail
