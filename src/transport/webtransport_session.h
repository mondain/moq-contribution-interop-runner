#pragma once

#include "moq/interop/transport/session_transport.h"

#include <h3zero_common.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace moq::interop::transport {

struct WebTransportSessionLimits {
    std::size_t max_events = 256;
    std::size_t max_event_payload_bytes = 1u << 20;
    std::size_t max_datagram_payload = 1150;
    std::size_t max_queued_send_bytes = 1u << 20;
};

[[nodiscard]] std::uint64_t webtransport_to_http_error(std::uint32_t code);
[[nodiscard]] std::optional<std::uint32_t> webtransport_from_http_error(
    std::uint64_t code);
[[nodiscard]] bool valid_webtransport_close_reason(
    std::span<const std::byte> reason);

// H3zero has already decoded and removed WT stream and HTTP Datagram prefixes
// before invoking ingest_stream/ingest_datagram. This class never parses MOQT.
class WebTransportSession final : public SessionTransport {
public:
    WebTransportSession(StreamId connect_stream_id, WebTransportSessionLimits limits,
                        picoquic_cnx_t* connection = nullptr,
                        h3zero_callback_ctx_t* h3 = nullptr,
                        h3zero_stream_ctx_t* control = nullptr,
                        picohttp_post_data_cb_fn stream_callback = nullptr,
                        void* stream_callback_context = nullptr);

    [[nodiscard]] StreamId connect_stream_id() const noexcept;
    void establish(std::vector<std::byte> application_protocol,
                   std::vector<std::byte> local_connection_id,
                   std::vector<std::byte> peer_connection_id);
    [[nodiscard]] bool ingest_stream(StreamId stream_id, StreamId source_session_id,
                                     std::span<const std::byte> payload, bool fin);
    [[nodiscard]] bool ingest_datagram(StreamId source_session_id,
                                       std::span<const std::byte> payload);
    void ingest_reset(StreamId stream_id, std::uint64_t wire_error);
    void ingest_stop_sending(StreamId stream_id, std::uint64_t wire_error);
    void ingest_peer_close(std::uint64_t application_error,
                           std::span<const std::byte> reason);
    void ingest_connection_close(CloseErrorSpace space, std::uint64_t error,
                                 std::span<const std::byte> reason);
    void detach() noexcept;

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
    void enqueue(TransportEvent event);
    void terminate_streams();
    std::size_t queued_stream_bytes() const;
    OpenResult open(bool bidirectional);

    StreamId connect_stream_id_;
    WebTransportSessionLimits limits_;
    picoquic_cnx_t* connection_;
    h3zero_callback_ctx_t* h3_;
    h3zero_stream_ctx_t* control_;
    picohttp_post_data_cb_fn stream_callback_;
    void* stream_callback_context_;
    std::deque<TransportEvent> events_;
    std::unordered_set<StreamId> writable_streams_;
    std::unordered_set<StreamId> readable_streams_;
    std::unordered_set<StreamId> finished_streams_;
    std::unordered_set<StreamId> finished_read_streams_;
    bool detached_ = false;
    bool overflowed_ = false;
};

}  // namespace moq::interop::transport
