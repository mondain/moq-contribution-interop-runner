#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace moq::interop::transport {

using StreamId = std::uint64_t;

enum class TransportStatus {
    Success,
    Partial,
    WouldBlock,
    PeerStopped,
    PeerReset,
    StreamLimit,
    DatagramTooLarge,
    InvalidState,
    ConnectionClosed,
    InternalError,
};

struct OpenResult {
    TransportStatus status = TransportStatus::InternalError;
    StreamId stream_id = 0;
};

struct OperationResult {
    TransportStatus status = TransportStatus::InternalError;
    std::size_t accepted = 0;
    std::optional<std::uint64_t> application_error;
};

struct ConnectionEstablishedEvent {
    std::vector<std::byte> alpn;
    std::vector<std::byte> local_connection_id;
    std::vector<std::byte> peer_connection_id;
    std::size_t max_datagram_payload = 0;
};

struct StreamDataEvent {
    StreamId stream_id = 0;
    std::vector<std::byte> data;
    bool fin = false;
};

struct PeerResetEvent {
    StreamId stream_id = 0;
    std::optional<std::uint64_t> application_error;
};

struct PeerStopSendingEvent {
    StreamId stream_id = 0;
    std::optional<std::uint64_t> application_error;
};

struct DatagramEvent {
    std::vector<std::byte> data;
};

enum class CloseErrorSpace { Transport, Application };

struct PeerCloseEvent {
    CloseErrorSpace error_space = CloseErrorSpace::Transport;
    std::uint64_t error_code = 0;
    std::vector<std::byte> reason;
};

struct LocalCloseEvent {
    CloseErrorSpace error_space = CloseErrorSpace::Transport;
    std::uint64_t error_code = 0;
    std::vector<std::byte> reason;
};

struct IdleTimeoutEvent {};

enum class TransportError {
    ProtocolFailure,
    InternalFailure,
};

struct TransportErrorEvent {
    TransportError error = TransportError::InternalFailure;
};

struct EventQueueOverflowEvent {};

using TransportEvent =
    std::variant<ConnectionEstablishedEvent, StreamDataEvent, PeerResetEvent,
                 PeerStopSendingEvent, DatagramEvent, PeerCloseEvent,
                 LocalCloseEvent, IdleTimeoutEvent, TransportErrorEvent,
                 EventQueueOverflowEvent>;

class SessionTransport {
public:
    virtual ~SessionTransport() = default;

    virtual OpenResult open_bidi() = 0;
    virtual OpenResult open_uni() = 0;
    virtual OperationResult write(StreamId stream_id,
                                  std::span<const std::byte> data,
                                  bool fin) = 0;
    virtual OperationResult reset(StreamId stream_id,
                                  std::uint64_t application_error) = 0;
    virtual OperationResult stop_sending(
        StreamId stream_id, std::uint64_t application_error) = 0;
    virtual OperationResult send_datagram(
        std::span<const std::byte> data) = 0;
    virtual OperationResult close(std::uint64_t application_error,
                                  std::span<const std::byte> reason) = 0;
    virtual std::vector<TransportEvent> poll(std::size_t max_events) = 0;

    // Raises the number of additional peer-initiated streams (bidirectional or
    // unidirectional) the peer may open, by sending a QUIC MAX_STREAMS frame.
    // A harness primitive for stream-credit scenarios; transports that cannot
    // do it keep this default.
    virtual OperationResult grant_peer_streams(bool bidirectional,
                                               std::uint64_t additional) {
        (void)bidirectional;
        (void)additional;
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
};

}  // namespace moq::interop::transport
