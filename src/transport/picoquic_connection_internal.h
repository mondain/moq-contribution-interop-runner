#pragma once

#include "moq/interop/transport/native_quic_listener.h"

#include <picoquic.h>

#include <cstddef>
#include <cstdint>
#include <deque>
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
    void fail();

private:
    int on_event(picoquic_cnx_t* connection, std::uint64_t stream_id,
                 std::uint8_t* bytes, std::size_t length,
                 picoquic_call_back_event_t event);
    void enqueue(TransportEvent event);

    const NativeQuicListenerConfig& config_;
    picoquic_cnx_t* connection_ = nullptr;
    std::deque<TransportEvent> events_;
    bool overflowed_ = false;
    bool established_ = false;
};

}  // namespace moq::interop::transport::detail
