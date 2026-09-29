#pragma once

#include "moq/interop/transport/native_quic_listener.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace moq::interop::transport {

struct WebTransportListenerConfig {
    NativeQuicListenerConfig quic;
    std::string authority;
    std::string path = "/moq";
    std::vector<std::string> allowed_origins;
    std::string application_protocol;
    bool require_origin = false;
};

class WebTransportListener;

struct WebTransportListenerCreateResult {
    std::unique_ptr<WebTransportListener> listener;
    std::optional<NativeQuicListenerError> error;
};

// One accepted publisher WebTransport session per run. The H3zero HTTP/3
// connection remains separate from MOQT: rejected CONNECTs emit no MOQT event.
class WebTransportListener final : public SessionTransport {
public:
    struct Impl;
    static WebTransportListenerCreateResult create(WebTransportListenerConfig config);

    ~WebTransportListener() override;
    WebTransportListener(WebTransportListener&&) noexcept;
    WebTransportListener& operator=(WebTransportListener&&) noexcept;
    WebTransportListener(const WebTransportListener&) = delete;
    WebTransportListener& operator=(const WebTransportListener&) = delete;

    const BoundEndpoint& bound_endpoint() const noexcept;

    OpenResult open_bidi() override;
    OpenResult open_uni() override;
    OperationResult write(StreamId stream_id, std::span<const std::byte> data,
                          bool fin) override;
    OperationResult reset(StreamId stream_id, std::uint64_t application_error) override;
    OperationResult stop_sending(StreamId stream_id,
                                 std::uint64_t application_error) override;
    OperationResult send_datagram(std::span<const std::byte> data) override;
    OperationResult close(std::uint64_t application_error,
                          std::span<const std::byte> reason) override;
    std::vector<TransportEvent> poll(std::size_t max_events) override;

private:
    explicit WebTransportListener(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::transport
