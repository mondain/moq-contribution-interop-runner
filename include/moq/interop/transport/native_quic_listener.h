#pragma once

#include "moq/interop/transport/session_transport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace moq::interop::transport {

struct NativeQuicListenerConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t bind_port = 0;
    std::filesystem::path certificate_path;
    std::filesystem::path private_key_path;
    std::vector<std::byte> expected_alpn;
    std::chrono::milliseconds idle_timeout{30'000};
    std::chrono::seconds retry_token_lifetime{120};
    std::size_t max_udp_payload = 1350;
    std::size_t max_datagrams_per_poll = 32;
    std::size_t max_egress_datagrams_per_call = 32;
    std::size_t max_events = 256;
    std::size_t max_event_payload_bytes = 1u << 20;
    std::size_t max_queued_send_bytes = 1u << 20;
    std::uint64_t initial_max_data = 1u << 20;
    std::uint64_t initial_max_stream_data_bidi_local = 1u << 18;
    std::uint64_t initial_max_stream_data_bidi_remote = 1u << 18;
    std::uint64_t initial_max_stream_data_uni = 1u << 18;
    std::uint64_t initial_max_streams_bidi = 64;
    std::uint64_t initial_max_streams_uni = 64;
    std::uint64_t missing_datagram_application_error = 3;
};

struct BoundEndpoint {
    std::string address;
    std::uint16_t port = 0;
};

enum class NativeQuicListenerError {
    InvalidConfiguration,
    UnsupportedBindAddress,
    CertificateLoadFailed,
    PrivateKeyLoadFailed,
    CryptoInitializationFailed,
    SocketOpenFailed,
    SocketConfigurationFailed,
    BindFailed,
    BoundEndpointFailed,
    AfterBindFailed,
    TransportConfigurationFailed,
};

class NativeQuicListener;

struct NativeQuicListenerCreateResult {
    std::unique_ptr<NativeQuicListener> listener;
    std::optional<NativeQuicListenerError> error;
};

class NativeQuicListener final : public SessionTransport {
public:
    struct Impl;

    static NativeQuicListenerCreateResult create(
        NativeQuicListenerConfig config);

    ~NativeQuicListener() override;
    NativeQuicListener(NativeQuicListener&&) noexcept;
    NativeQuicListener& operator=(NativeQuicListener&&) noexcept;
    NativeQuicListener(const NativeQuicListener&) = delete;
    NativeQuicListener& operator=(const NativeQuicListener&) = delete;

    const BoundEndpoint& bound_endpoint() const noexcept;

    OpenResult open_bidi() override;
    OpenResult open_uni() override;
    OperationResult write(StreamId stream_id,
                          std::span<const std::byte> data,
                          bool fin) override;
    OperationResult reset(StreamId stream_id,
                          std::uint64_t application_error) override;
    OperationResult stop_sending(StreamId stream_id,
                                 std::uint64_t application_error) override;
    OperationResult send_datagram(
        std::span<const std::byte> data) override;
    OperationResult close(std::uint64_t application_error,
                          std::span<const std::byte> reason) override;
    std::vector<TransportEvent> poll(std::size_t max_events) override;
    OperationResult grant_peer_streams(bool bidirectional,
                                       std::uint64_t additional) override;
    OperationResult set_inbound_drop(bool enabled) override;

private:
    explicit NativeQuicListener(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend struct NativeQuicListenerFactory;
};

}  // namespace moq::interop::transport
