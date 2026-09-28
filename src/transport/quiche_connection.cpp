#include "quiche_connection_internal.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moq::interop::transport::detail {
namespace {

enum class LocalStreamState {
    Reserved,
    Materialized,
    AbandonedPending,
    Abandoned,
};

OperationResult operation(TransportStatus status, std::size_t accepted = 0) {
    return {status, accepted, std::nullopt};
}

std::size_t payload_size(const TransportEvent& event) {
    return std::visit(
        [](const auto& item) -> std::size_t {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, ConnectionEstablishedEvent>) {
                return item.alpn.size() + item.local_connection_id.size() +
                       item.peer_connection_id.size();
            } else if constexpr (std::is_same_v<T, StreamDataEvent> ||
                                 std::is_same_v<T, DatagramEvent>) {
                return item.data.size();
            } else if constexpr (std::is_same_v<T, PeerCloseEvent> ||
                                 std::is_same_v<T, LocalCloseEvent>) {
                return item.reason.size();
            } else {
                return 0;
            }
        },
        event);
}

bool is_overflow(const TransportEvent& event) {
    return std::holds_alternative<EventQueueOverflowEvent>(event);
}

TransportStatus map_stream_send_error(std::int64_t result) {
    switch (result) {
        case QUICHE_ERR_DONE:
            return TransportStatus::WouldBlock;
        case QUICHE_ERR_STREAM_STOPPED:
            return TransportStatus::PeerStopped;
        case QUICHE_ERR_STREAM_RESET:
            return TransportStatus::PeerReset;
        case QUICHE_ERR_STREAM_LIMIT:
            return TransportStatus::StreamLimit;
        case QUICHE_ERR_INVALID_STATE:
        case QUICHE_ERR_INVALID_STREAM_STATE:
            return TransportStatus::InvalidState;
        default:
            return TransportStatus::InternalError;
    }
}

TransportStatus map_stream_shutdown_error(int result) {
    switch (result) {
        case QUICHE_ERR_DONE:
        case QUICHE_ERR_INVALID_STATE:
        case QUICHE_ERR_INVALID_STREAM_STATE:
            return TransportStatus::InvalidState;
        case QUICHE_ERR_STREAM_STOPPED:
            return TransportStatus::PeerStopped;
        case QUICHE_ERR_STREAM_RESET:
            return TransportStatus::PeerReset;
        default:
            return TransportStatus::InternalError;
    }
}

TransportStatus map_datagram_capacity_error(std::int64_t result) {
    switch (result) {
        case QUICHE_ERR_DONE:
        case QUICHE_ERR_INVALID_STATE:
            return TransportStatus::InvalidState;
        default:
            return TransportStatus::InternalError;
    }
}

TransportStatus map_datagram_send_error(std::int64_t result) {
    switch (result) {
        case QUICHE_ERR_DONE:
            return TransportStatus::WouldBlock;
        case QUICHE_ERR_INVALID_STATE:
            return TransportStatus::InvalidState;
        default:
            return TransportStatus::InternalError;
    }
}

}  // namespace

struct QuicheConnection::Impl {
    Impl(quiche_conn* connection_value, QuicheApi api_value,
         EventLimits limits_value, StreamId next_bidi_value,
         StreamId next_uni_value)
        : connection(connection_value),
          api(api_value),
          limits(limits_value),
          next_bidi(next_bidi_value),
          next_uni(next_uni_value) {}

    quiche_conn* connection;
    QuicheApi api;
    EventLimits limits;
    StreamId next_bidi;
    StreamId next_uni;
    std::uint64_t reserved_bidi = 0;
    std::uint64_t reserved_uni = 0;
    bool bidi_ids_exhausted = false;
    bool uni_ids_exhausted = false;
    bool terminal = false;
    bool overflowed = false;
    std::unordered_map<StreamId, LocalStreamState> local_streams;
    std::unordered_set<StreamId> peer_stop_reported;
    std::deque<TransportEvent> events;
    std::size_t normal_event_count = 0;
    std::size_t owned_payload_bytes = 0;
    std::optional<TransportDiagnostic> last_diagnostic;

    ~Impl() { api.connection_free(connection); }

    OpenResult open(bool bidi) {
        if (terminal) return {TransportStatus::ConnectionClosed, 0};
        auto& reserved = bidi ? reserved_bidi : reserved_uni;
        auto& exhausted = bidi ? bidi_ids_exhausted : uni_ids_exhausted;
        auto& next = bidi ? next_bidi : next_uni;
        if (exhausted) return {TransportStatus::InvalidState, 0};
        const auto credit = bidi
                                ? api.peer_streams_left_bidi(connection)
                                : api.peer_streams_left_uni(connection);
        if (reserved >= credit) return {TransportStatus::StreamLimit, 0};

        const auto stream_id = next;
        ++reserved;
        local_streams.emplace(stream_id, LocalStreamState::Reserved);
        if (stream_id > std::numeric_limits<StreamId>::max() - 4) {
            exhausted = true;
        } else {
            next = stream_id + 4;
        }
        return {TransportStatus::Success, stream_id};
    }

    void abandon_if_reserved(StreamId stream_id) {
        const auto found = local_streams.find(stream_id);
        if (found != local_streams.end() &&
            found->second == LocalStreamState::Reserved) {
            found->second = LocalStreamState::AbandonedPending;
        }
    }

    void materialize_if_reserved(StreamId stream_id) {
        const bool bidi = (stream_id & 2u) == 0u;
        auto& reserved = bidi ? reserved_bidi : reserved_uni;
        for (auto& [candidate_id, state] : local_streams) {
            if (((candidate_id & 2u) == 0u) != bidi ||
                candidate_id > stream_id) {
                continue;
            }
            if (state == LocalStreamState::Reserved) {
                state = LocalStreamState::Materialized;
                --reserved;
            } else if (state == LocalStreamState::AbandonedPending) {
                state = LocalStreamState::Abandoned;
                --reserved;
            }
        }
    }

    bool enqueue(TransportEvent event, bool terminal_event) {
        if (terminal) return false;
        const auto bytes = payload_size(event);
        const bool count_exceeded = normal_event_count >= limits.max_count;
        const bool bytes_exceeded =
            bytes > limits.max_payload_bytes - owned_payload_bytes;
        if (count_exceeded || bytes_exceeded) {
            events.emplace_back(EventQueueOverflowEvent{});
            overflowed = true;
            terminal = true;
            return false;
        }

        events.push_back(std::move(event));
        owned_payload_bytes += bytes;
        ++normal_event_count;
        if (terminal_event) terminal = true;
        return true;
    }
};

QuicheApi default_quiche_api() noexcept {
    return {quiche_conn_stream_send, quiche_conn_stream_shutdown,
            quiche_conn_peer_streams_left_bidi,
            quiche_conn_peer_streams_left_uni,
            quiche_conn_dgram_max_writable_len, quiche_conn_dgram_send,
            quiche_conn_close, quiche_conn_free};
}

QuicheConnectionCreateResult QuicheConnection::create(
    quiche_conn* connection, QuicheApi api, EventLimits event_limits,
    InitialStreamIds initial_stream_ids) {
    if (event_limits.max_count == 0 || event_limits.max_payload_bytes == 0) {
        return {nullptr, ConstructionError::InvalidEventLimits};
    }
    if (connection == nullptr) {
        return {nullptr, ConstructionError::InvalidConnection};
    }
    if (api.stream_send == nullptr || api.stream_shutdown == nullptr ||
        api.peer_streams_left_bidi == nullptr ||
        api.peer_streams_left_uni == nullptr ||
        api.datagram_max_writable_len == nullptr ||
        api.datagram_send == nullptr || api.connection_close == nullptr ||
        api.connection_free == nullptr) {
        return {nullptr, ConstructionError::InvalidApi};
    }
    if ((initial_stream_ids.bidi & 3u) != 1u ||
        (initial_stream_ids.uni & 3u) != 3u) {
        return {nullptr, ConstructionError::InvalidStreamIds};
    }
    auto impl = std::make_unique<Impl>(
        connection, api, event_limits, initial_stream_ids.bidi,
        initial_stream_ids.uni);
    return {std::unique_ptr<QuicheConnection>(
                new QuicheConnection(std::move(impl))),
            std::nullopt};
}

QuicheConnection::QuicheConnection(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

QuicheConnection::~QuicheConnection() = default;
QuicheConnection::QuicheConnection(QuicheConnection&&) noexcept = default;
QuicheConnection& QuicheConnection::operator=(QuicheConnection&&) noexcept =
    default;

OpenResult QuicheConnection::open_bidi() {
    if (!impl_) return {TransportStatus::InvalidState, 0};
    return impl_->open(true);
}

OpenResult QuicheConnection::open_uni() {
    if (!impl_) return {TransportStatus::InvalidState, 0};
    return impl_->open(false);
}

OperationResult QuicheConnection::write(StreamId stream_id,
                                        std::span<const std::byte> data,
                                        bool fin) {
    if (!impl_) return operation(TransportStatus::InvalidState);
    if (impl_->terminal) return operation(TransportStatus::ConnectionClosed);
    const auto known = impl_->local_streams.find(stream_id);
    if ((stream_id & 1u) != 0u && known == impl_->local_streams.end()) {
        return operation(TransportStatus::InvalidState);
    }
    if (known != impl_->local_streams.end() &&
        (known->second == LocalStreamState::AbandonedPending ||
         known->second == LocalStreamState::Abandoned)) {
        return operation(TransportStatus::InvalidState);
    }
    const bool pending_local =
        known != impl_->local_streams.end() &&
        known->second == LocalStreamState::Reserved;
    const bool bidi = (stream_id & 2u) == 0u;
    const auto credit_before =
        pending_local
            ? (bidi ? impl_->api.peer_streams_left_bidi(impl_->connection)
                    : impl_->api.peer_streams_left_uni(impl_->connection))
            : 0;

    std::uint64_t application_error = 0;
    const auto result = impl_->api.stream_send(
        impl_->connection, stream_id,
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), fin,
        &application_error);
    if (pending_local) {
        const auto credit_after =
            bidi ? impl_->api.peer_streams_left_bidi(impl_->connection)
                 : impl_->api.peer_streams_left_uni(impl_->connection);
        if (credit_after < credit_before) {
            impl_->materialize_if_reserved(stream_id);
        }
    }
    if (result >= 0) {
        const auto accepted = static_cast<std::uint64_t>(result);
        if (accepted > data.size()) {
            impl_->last_diagnostic =
                TransportDiagnostic{TransportOperation::StreamWrite, result};
            impl_->abandon_if_reserved(stream_id);
            return operation(TransportStatus::InternalError);
        }
        if (accepted == 0 && !data.empty()) {
            return operation(TransportStatus::WouldBlock);
        }
        impl_->materialize_if_reserved(stream_id);
        if (accepted < data.size()) {
            return operation(TransportStatus::Partial,
                             static_cast<std::size_t>(accepted));
        }
        return operation(TransportStatus::Success,
                         static_cast<std::size_t>(accepted));
    }

    const auto status = map_stream_send_error(result);
    if (status != TransportStatus::WouldBlock) {
        impl_->abandon_if_reserved(stream_id);
    }
    if (status == TransportStatus::InternalError) {
        impl_->last_diagnostic =
            TransportDiagnostic{TransportOperation::StreamWrite, result};
    }
    auto mapped = operation(status);
    if (status == TransportStatus::PeerStopped ||
        status == TransportStatus::PeerReset) {
        mapped.application_error = application_error;
    }
    if (status == TransportStatus::PeerStopped &&
        impl_->peer_stop_reported.insert(stream_id).second) {
        impl_->enqueue(PeerStopSendingEvent{stream_id, application_error},
                       false);
    }
    return mapped;
}

OperationResult QuicheConnection::reset(StreamId stream_id,
                                        std::uint64_t application_error) {
    if (!impl_) return operation(TransportStatus::InvalidState);
    if (impl_->terminal) return operation(TransportStatus::ConnectionClosed);
    const auto result = impl_->api.stream_shutdown(
        impl_->connection, stream_id, QUICHE_SHUTDOWN_WRITE,
        application_error);
    const auto status =
        result == 0 ? TransportStatus::Success
                    : map_stream_shutdown_error(result);
    if (status == TransportStatus::InternalError) {
        impl_->last_diagnostic = TransportDiagnostic{
            TransportOperation::StreamShutdown, result};
    }
    return operation(status);
}

OperationResult QuicheConnection::stop_sending(
    StreamId stream_id, std::uint64_t application_error) {
    if (!impl_) return operation(TransportStatus::InvalidState);
    if (impl_->terminal) return operation(TransportStatus::ConnectionClosed);
    const auto result = impl_->api.stream_shutdown(
        impl_->connection, stream_id, QUICHE_SHUTDOWN_READ,
        application_error);
    const auto status =
        result == 0 ? TransportStatus::Success
                    : map_stream_shutdown_error(result);
    if (status == TransportStatus::InternalError) {
        impl_->last_diagnostic = TransportDiagnostic{
            TransportOperation::StreamShutdown, result};
    }
    return operation(status);
}

OperationResult QuicheConnection::send_datagram(
    std::span<const std::byte> data) {
    if (!impl_) return operation(TransportStatus::InvalidState);
    if (impl_->terminal) return operation(TransportStatus::ConnectionClosed);
    const auto maximum =
        impl_->api.datagram_max_writable_len(impl_->connection);
    if (maximum < 0) {
        const auto status = map_datagram_capacity_error(maximum);
        if (status == TransportStatus::InternalError) {
            impl_->last_diagnostic = TransportDiagnostic{
                TransportOperation::DatagramCapacity, maximum};
        }
        return operation(status);
    }
    if (data.size() > static_cast<std::size_t>(maximum)) {
        return operation(TransportStatus::DatagramTooLarge);
    }

    static constexpr std::uint8_t kEmptyDatagramByte = 0;
    const auto* data_pointer =
        data.empty() ? &kEmptyDatagramByte
                     : reinterpret_cast<const std::uint8_t*>(data.data());
    const auto result = impl_->api.datagram_send(
        impl_->connection, data_pointer, data.size());
    if (result >= 0) {
        const auto accepted = static_cast<std::uint64_t>(result);
        if (accepted > data.size()) {
            impl_->last_diagnostic = TransportDiagnostic{
                TransportOperation::DatagramSend, result};
            return operation(TransportStatus::InternalError);
        }
        if (accepted == 0 && !data.empty()) {
            return operation(TransportStatus::WouldBlock);
        }
        return operation(accepted < data.size() ? TransportStatus::Partial
                                                : TransportStatus::Success,
                         static_cast<std::size_t>(accepted));
    }
    if (result == QUICHE_ERR_BUFFER_TOO_SHORT) {
        return operation(TransportStatus::DatagramTooLarge);
    }
    const auto status = map_datagram_send_error(result);
    if (status == TransportStatus::InternalError) {
        impl_->last_diagnostic =
            TransportDiagnostic{TransportOperation::DatagramSend, result};
    }
    return operation(status);
}

OperationResult QuicheConnection::close(
    std::uint64_t application_error, std::span<const std::byte> reason) {
    if (!impl_) return operation(TransportStatus::InvalidState);
    if (impl_->terminal) return operation(TransportStatus::ConnectionClosed);
    static constexpr std::uint8_t kEmptyReasonByte = 0;
    const auto* reason_pointer =
        reason.empty() ? &kEmptyReasonByte
                       : reinterpret_cast<const std::uint8_t*>(reason.data());
    const auto result = impl_->api.connection_close(
        impl_->connection, true, application_error, reason_pointer,
        reason.size());
    if (result == 0 || result == QUICHE_ERR_DONE) {
        return operation(TransportStatus::Success);
    }
    if (result == QUICHE_ERR_INVALID_STATE) {
        return operation(TransportStatus::InvalidState);
    }
    return operation(TransportStatus::InternalError);
}

std::vector<TransportEvent> QuicheConnection::poll(std::size_t max_events) {
    std::vector<TransportEvent> output;
    if (!impl_) return output;
    const auto count = std::min(max_events, impl_->events.size());
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto event = std::move(impl_->events.front());
        impl_->events.pop_front();
        if (!is_overflow(event)) {
            --impl_->normal_event_count;
            impl_->owned_payload_bytes -= payload_size(event);
        }
        output.push_back(std::move(event));
    }
    return output;
}

bool QuicheConnection::notify_established(
    std::span<const std::byte> alpn,
    std::span<const std::byte> local_connection_id,
    std::span<const std::byte> peer_connection_id,
    std::size_t max_datagram_payload) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(
        ConnectionEstablishedEvent{{alpn.begin(), alpn.end()},
                                   {local_connection_id.begin(),
                                    local_connection_id.end()},
                                   {peer_connection_id.begin(),
                                    peer_connection_id.end()},
                                   max_datagram_payload},
        false);
}

bool QuicheConnection::notify_stream_data(StreamId stream_id,
                                          std::span<const std::byte> data,
                                          bool fin) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(
        StreamDataEvent{stream_id, {data.begin(), data.end()}, fin}, false);
}

bool QuicheConnection::notify_peer_reset(
    StreamId stream_id, std::uint64_t application_error) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(PeerResetEvent{stream_id, application_error}, false);
}

bool QuicheConnection::notify_peer_stop_sending(
    StreamId stream_id, std::uint64_t application_error) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(
        PeerStopSendingEvent{stream_id, application_error}, false);
}

bool QuicheConnection::notify_datagram(std::span<const std::byte> data) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(DatagramEvent{{data.begin(), data.end()}}, false);
}

bool QuicheConnection::notify_peer_close(
    CloseErrorSpace error_space, std::uint64_t error_code,
    std::span<const std::byte> reason) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(
        PeerCloseEvent{error_space, error_code,
                       {reason.begin(), reason.end()}},
        true);
}

bool QuicheConnection::notify_local_close(
    CloseErrorSpace error_space, std::uint64_t error_code,
    std::span<const std::byte> reason) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(
        LocalCloseEvent{error_space, error_code,
                        {reason.begin(), reason.end()}},
        true);
}

bool QuicheConnection::notify_idle_timeout() {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(IdleTimeoutEvent{}, true);
}

bool QuicheConnection::notify_transport_error(TransportError error) {
    if (!impl_ || impl_->terminal) return false;
    return impl_->enqueue(TransportErrorEvent{error}, true);
}

std::optional<TransportDiagnostic>
QuicheConnection::last_transport_diagnostic() const {
    if (!impl_) return std::nullopt;
    return impl_->last_diagnostic;
}

bool QuicheConnection::event_queue_overflowed() const noexcept {
    return impl_ && impl_->overflowed;
}

quiche_conn* QuicheConnection::native_handle() noexcept {
    return impl_ ? impl_->connection : nullptr;
}

}  // namespace moq::interop::transport::detail
