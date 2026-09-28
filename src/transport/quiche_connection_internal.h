#pragma once

#include "moq/interop/transport/session_transport.h"

#include <quiche.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace moq::interop::transport::detail {

struct QuicheApi {
    decltype(&quiche_conn_stream_send) stream_send = nullptr;
    decltype(&quiche_conn_stream_shutdown) stream_shutdown = nullptr;
    decltype(&quiche_conn_peer_streams_left_bidi) peer_streams_left_bidi =
        nullptr;
    decltype(&quiche_conn_peer_streams_left_uni) peer_streams_left_uni =
        nullptr;
    decltype(&quiche_conn_dgram_max_writable_len) datagram_max_writable_len =
        nullptr;
    decltype(&quiche_conn_dgram_send) datagram_send = nullptr;
    decltype(&quiche_conn_close) connection_close = nullptr;
    decltype(&quiche_conn_free) connection_free = nullptr;
};

QuicheApi default_quiche_api() noexcept;

struct EventLimits {
    std::size_t max_count = 0;
    std::size_t max_payload_bytes = 0;
};

struct InitialStreamIds {
    StreamId bidi = 1;
    StreamId uni = 3;
};

enum class ConstructionError {
    InvalidEventLimits,
    InvalidConnection,
    InvalidApi,
    InvalidStreamIds,
};

enum class TransportOperation {
    StreamWrite,
    StreamShutdown,
    DatagramCapacity,
    DatagramSend,
};

struct TransportDiagnostic {
    TransportOperation operation;
    std::int64_t implementation_code;

    bool operator==(const TransportDiagnostic&) const = default;
};

class QuicheConnection;

struct QuicheConnectionCreateResult {
    std::unique_ptr<QuicheConnection> connection;
    std::optional<ConstructionError> error;
};

class QuicheConnection final : public SessionTransport {
public:
    static QuicheConnectionCreateResult create(
        quiche_conn* connection, QuicheApi api, EventLimits event_limits,
        InitialStreamIds initial_stream_ids = {});

    ~QuicheConnection() override;
    QuicheConnection(QuicheConnection&&) noexcept;
    QuicheConnection& operator=(QuicheConnection&&) noexcept;
    QuicheConnection(const QuicheConnection&) = delete;
    QuicheConnection& operator=(const QuicheConnection&) = delete;

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

    bool notify_established(std::span<const std::byte> alpn,
                            std::span<const std::byte> local_connection_id,
                            std::span<const std::byte> peer_connection_id,
                            std::size_t max_datagram_payload);
    bool notify_stream_data(StreamId stream_id,
                            std::span<const std::byte> data, bool fin);
    bool notify_peer_reset(StreamId stream_id,
                           std::uint64_t application_error);
    bool notify_peer_stop_sending(StreamId stream_id,
                                  std::uint64_t application_error);
    bool notify_datagram(std::span<const std::byte> data);
    bool notify_peer_close(CloseErrorSpace error_space,
                           std::uint64_t error_code,
                           std::span<const std::byte> reason);
    bool notify_local_close(CloseErrorSpace error_space,
                            std::uint64_t error_code,
                            std::span<const std::byte> reason);
    bool notify_idle_timeout();
    bool notify_transport_error(TransportError error);
    std::optional<TransportDiagnostic> last_transport_diagnostic() const;
    quiche_conn* native_handle() noexcept;

private:
    struct Impl;
    explicit QuicheConnection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::transport::detail
