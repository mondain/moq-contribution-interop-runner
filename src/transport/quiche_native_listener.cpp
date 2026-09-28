#include "quiche_native_listener_internal.h"

#include "quiche_connection_internal.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace moq::interop::transport {
namespace {

constexpr std::array<std::byte, 7> kDraft18Alpn{
    std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
    std::byte{'-'}, std::byte{'1'}, std::byte{'8'}};

OperationResult unavailable() {
    return {TransportStatus::InvalidState, 0, std::nullopt};
}

bool exact_draft18_alpn(std::span<const std::byte> alpn) {
    return std::ranges::equal(alpn, kDraft18Alpn);
}

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error) &&
           !error;
}

}  // namespace

struct NativeQuicListener::Impl {
    enum class State { Listening, Handshaking, Established, Closing, Closed };

    NativeQuicListenerConfig config;
    BoundEndpoint endpoint;
    int socket_fd = -1;
    quiche_config* quiche_configuration = nullptr;
    detail::NativeListenerDependencies dependencies;
    std::unique_ptr<detail::QuicheConnection> connection;
    std::unique_ptr<detail::RetryTokenCodec> token_codec;
    sockaddr_storage local_address{};
    socklen_t local_address_size = 0;
    sockaddr_storage peer_address{};
    socklen_t peer_address_size = 0;
    State state = State::Listening;
    bool establishment_checked = false;
    bool terminal_emitted = false;
    std::vector<std::uint8_t> accepted_local_cid;
    detail::ActiveCidRoutes active_local_cids;
    std::size_t additional_cids_registered = 0;
    std::vector<std::uint8_t> original_destination_cid;
    std::vector<std::byte> receive_buffer;
    std::vector<std::byte> send_buffer;
    struct PendingPacket {
        std::vector<std::byte> bytes;
        sockaddr_storage from{};
        socklen_t from_size = 0;
        sockaddr_storage to{};
        socklen_t to_size = 0;
        std::uint64_t deadline_nanoseconds = 0;
    };
    std::optional<PendingPacket> pending_packet;
    std::deque<TransportEvent> listener_events;

    TransportStatus application_status() const noexcept {
        if (state == State::Established) return TransportStatus::Success;
        if (state == State::Closing || state == State::Closed) {
            return TransportStatus::ConnectionClosed;
        }
        return TransportStatus::InvalidState;
    }

    ~Impl() {
        connection.reset();
        if (quiche_configuration != nullptr) {
            quiche_config_free(quiche_configuration);
        }
        if (socket_fd >= 0) {
            dependencies.socket.close(socket_fd);
        }
    }

    void terminal_transport_error() {
        if (state == State::Closed) return;
        pending_packet.reset();
        if (connection) {
            connection->notify_transport_error(TransportError::InternalFailure);
        } else {
            listener_events.emplace_back(
                TransportErrorEvent{TransportError::InternalFailure});
        }
        state = State::Closed;
    }

    static std::uint64_t deadline(const timespec& value) {
        if (value.tv_sec < 0 || value.tv_nsec < 0 ||
            value.tv_nsec >= 1'000'000'000) {
            return 0;
        }
        constexpr auto billion = std::uint64_t{1'000'000'000};
        const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
        if (seconds >
            (std::numeric_limits<std::uint64_t>::max() -
             static_cast<std::uint64_t>(value.tv_nsec)) /
                billion) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return seconds * billion + static_cast<std::uint64_t>(value.tv_nsec);
    }

    bool queue_packet(std::span<const std::byte> packet,
                      const sockaddr* source, socklen_t source_size,
                      const sockaddr* destination, socklen_t destination_size,
                      std::uint64_t at) {
        if (pending_packet || state == State::Closed) return false;
        PendingPacket pending;
        pending.bytes.assign(packet.begin(), packet.end());
        if (source != nullptr && source_size <= sizeof(pending.from)) {
            std::memcpy(&pending.from, source, source_size);
            pending.from_size = source_size;
        }
        if (destination == nullptr || destination_size > sizeof(pending.to)) {
            terminal_transport_error();
            return false;
        }
        std::memcpy(&pending.to, destination, destination_size);
        pending.to_size = destination_size;
        pending.deadline_nanoseconds = at;
        pending_packet = std::move(pending);
        return true;
    }

    bool send_pending() {
        if (!pending_packet || state == State::Closed) return false;
        if (dependencies.monotonic_nanoseconds() <
            pending_packet->deadline_nanoseconds) {
            return false;
        }
        ssize_t sent = -1;
        do {
            sent = dependencies.socket.send_datagram(
                socket_fd, pending_packet->bytes.data(),
                pending_packet->bytes.size(), 0,
                reinterpret_cast<const sockaddr*>(&pending_packet->to),
                pending_packet->to_size);
        } while (sent < 0 && errno == EINTR);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
        if (sent < 0 ||
            static_cast<std::size_t>(sent) != pending_packet->bytes.size()) {
            terminal_transport_error();
            return false;
        }
        pending_packet.reset();
        return true;
    }

    detail::TokenAddress token_address(const sockaddr_storage& storage) const {
        detail::TokenAddress output;
        if (storage.ss_family == AF_INET) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(&storage);
            output.family = 4;
            output.length = 4;
            std::memcpy(output.bytes.data(), &address->sin_addr, 4);
            output.port = ntohs(address->sin_port);
        } else if (storage.ss_family == AF_INET6) {
            const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage);
            output.family = 6;
            output.length = 16;
            std::memcpy(output.bytes.data(), &address->sin6_addr, 16);
            output.port = ntohs(address->sin6_port);
        }
        return output;
    }

    bool route_matches(std::span<const std::byte> dcid) {
        if (!connection) return false;
        synchronize_connection_ids();
        return active_local_cids.contains(
            {reinterpret_cast<const std::uint8_t*>(dcid.data()), dcid.size()});
    }

    void synchronize_connection_ids() {
        if (!connection) return;
        // quiche 0.24.9's C iterators expose pointers to temporary cloned
        // ConnectionIds. With at most one additional SCID, the stable current
        // SCID plus the active count identifies a retired alias unambiguously.
        const std::uint8_t* current = nullptr;
        std::size_t current_size = 0;
        quiche_conn_source_id(connection->native_handle(), &current,
                              &current_size);
        const auto active_count =
            quiche_conn_active_scids(connection->native_handle());
        if (!active_local_cids.synchronize({current, current_size},
                                           active_count)) {
            terminal_transport_error();
        }
    }

    void register_additional_connection_ids() {
        if (!connection || state == State::Closed) return;
        std::size_t attempts = 0;
        const auto maximum_attempts =
            config.max_additional_connection_ids * 4u + 4u;
        while (additional_cids_registered <
                   config.max_additional_connection_ids &&
               quiche_conn_scids_left(connection->native_handle()) > 0) {
            if (++attempts > maximum_attempts) {
                terminal_transport_error();
                return;
            }
            std::array<std::byte, 16> cid{};
            std::array<std::byte, 16> reset_token{};
            if (!dependencies.random_bytes(cid) ||
                !dependencies.random_bytes(reset_token)) {
                terminal_transport_error();
                return;
            }
            const bool duplicate = active_local_cids.contains(
                {reinterpret_cast<const std::uint8_t*>(cid.data()), cid.size()});
            if (duplicate) continue;
            std::uint64_t sequence = 0;
            const auto result = quiche_conn_new_scid(
                connection->native_handle(),
                reinterpret_cast<const std::uint8_t*>(cid.data()), cid.size(),
                reinterpret_cast<const std::uint8_t*>(reset_token.data()),
                false, &sequence);
            if (result == QUICHE_ERR_DONE) break;
            if (result < 0) {
                terminal_transport_error();
                return;
            }
            ++additional_cids_registered;
            active_local_cids.add({
                reinterpret_cast<const std::uint8_t*>(cid.data()),
                cid.size()});
        }
    }

    void drain_application_events() {
        if (!connection) return;
        auto* native = connection->native_handle();
        auto* readable = quiche_conn_readable(native);
        if (readable != nullptr) {
            StreamId stream_id = 0;
            while (quiche_stream_iter_next(readable, &stream_id)) {
                while (true) {
                    bool fin = false;
                    std::uint64_t error = 0;
                    auto* data = reinterpret_cast<std::uint8_t*>(
                        receive_buffer.data());
                    const auto read = quiche_conn_stream_recv(
                        native, stream_id, data, receive_buffer.size(), &fin,
                        &error);
                    if (read == QUICHE_ERR_DONE) break;
                    if (read == QUICHE_ERR_STREAM_RESET) {
                        connection->notify_peer_reset(stream_id, error);
                        break;
                    }
                    if (read < 0) break;
                    connection->notify_stream_data(
                        stream_id,
                        std::span<const std::byte>(receive_buffer)
                            .first(static_cast<std::size_t>(read)),
                        fin);
                    if (fin || read == 0) break;
                }
            }
            quiche_stream_iter_free(readable);
        }

        while (true) {
            const auto front = quiche_conn_dgram_recv_front_len(native);
            if (front == QUICHE_ERR_DONE) break;
            if (front < 0 ||
                static_cast<std::uint64_t>(front) > receive_buffer.size()) {
                connection->notify_transport_error(
                    TransportError::InternalFailure);
                state = State::Closed;
                break;
            }
            const auto read = quiche_conn_dgram_recv(
                native, reinterpret_cast<std::uint8_t*>(receive_buffer.data()),
                receive_buffer.size());
            if (read < 0) break;
            connection->notify_datagram(
                std::span<const std::byte>(receive_buffer)
                    .first(static_cast<std::size_t>(read)));
        }
        close_on_event_overflow();
    }

    void close_on_event_overflow() {
        if (!connection || !connection->event_queue_overflowed() ||
            state == State::Closing || state == State::Closed) {
            return;
        }
        static constexpr std::string_view reason = "event queue overflow";
        const auto result = quiche_conn_close(
            connection->native_handle(), false, 1,
            reinterpret_cast<const std::uint8_t*>(reason.data()),
            reason.size());
        if (result == 0 || result == QUICHE_ERR_DONE) {
            state = State::Closing;
        } else {
            terminal_transport_error();
        }
    }

    void check_established() {
        if (!connection || establishment_checked ||
            !quiche_conn_is_established(connection->native_handle())) {
            return;
        }
        establishment_checked = true;
        const std::uint8_t* alpn = nullptr;
        std::size_t alpn_size = 0;
        quiche_conn_application_proto(connection->native_handle(), &alpn,
                                      &alpn_size);
        quiche_transport_params parameters{};
        const bool has_parameters = quiche_conn_peer_transport_params(
            connection->native_handle(), &parameters);
        const auto maximum = quiche_conn_dgram_max_writable_len(
            connection->native_handle());
        if (alpn_size != kDraft18Alpn.size() ||
            std::memcmp(alpn, kDraft18Alpn.data(), alpn_size) != 0 ||
            !has_parameters || parameters.peer_max_datagram_frame_size < 0 ||
            maximum < 0) {
            static constexpr std::array<std::byte, 28> reason{
                std::byte{'D'}, std::byte{'A'}, std::byte{'T'}, std::byte{'A'},
                std::byte{'G'}, std::byte{'R'}, std::byte{'A'}, std::byte{'M'},
                std::byte{' '}, std::byte{'n'}, std::byte{'o'}, std::byte{'t'},
                std::byte{' '}, std::byte{'n'}, std::byte{'e'}, std::byte{'g'},
                std::byte{'o'}, std::byte{'t'}, std::byte{'i'}, std::byte{'a'},
                std::byte{'t'}, std::byte{'e'}, std::byte{'d'}, std::byte{' '},
                std::byte{'f'}, std::byte{'o'}, std::byte{'r'}, std::byte{' '}};
            connection->close(config.missing_datagram_application_error,
                              reason);
            state = State::Closing;
            return;
        }
        synchronize_connection_ids();
        register_additional_connection_ids();
        if (state == State::Closed) return;
        const std::uint8_t* local_cid = nullptr;
        const std::uint8_t* peer_cid = nullptr;
        std::size_t local_size = 0;
        std::size_t peer_size = 0;
        quiche_conn_source_id(connection->native_handle(), &local_cid,
                              &local_size);
        quiche_conn_destination_id(connection->native_handle(), &peer_cid,
                                   &peer_size);
        const auto notified = connection->notify_established(
            {reinterpret_cast<const std::byte*>(alpn), alpn_size},
            {reinterpret_cast<const std::byte*>(local_cid), local_size},
            {reinterpret_cast<const std::byte*>(peer_cid), peer_size},
            static_cast<std::size_t>(maximum));
        close_on_event_overflow();
        if (!notified || state == State::Closing || state == State::Closed) {
            return;
        }
        state = State::Established;
    }

    void emit_terminal() {
        if (!connection) return;
        if (terminal_emitted) {
            if (quiche_conn_is_closed(connection->native_handle())) {
                state = State::Closed;
            }
            return;
        }
        if (quiche_conn_is_timed_out(connection->native_handle())) {
            terminal_emitted = true;
            connection->notify_idle_timeout();
            state = State::Closed;
            return;
        }
        bool application = false;
        std::uint64_t code = 0;
        const std::uint8_t* reason = nullptr;
        std::size_t reason_size = 0;
        if (quiche_conn_peer_error(connection->native_handle(), &application,
                                   &code, &reason, &reason_size)) {
            terminal_emitted = true;
            connection->notify_peer_close(
                application ? CloseErrorSpace::Application
                            : CloseErrorSpace::Transport,
                code, {reinterpret_cast<const std::byte*>(reason), reason_size});
            state = State::Closing;
        } else if (quiche_conn_local_error(connection->native_handle(),
                                           &application, &code, &reason,
                                           &reason_size)) {
            terminal_emitted = true;
            connection->notify_local_close(
                application ? CloseErrorSpace::Application
                            : CloseErrorSpace::Transport,
                code, {reinterpret_cast<const std::byte*>(reason), reason_size});
            state = State::Closing;
        } else if (quiche_conn_is_closed(connection->native_handle())) {
            terminal_emitted = true;
            connection->notify_transport_error(TransportError::InternalFailure);
        } else {
            return;
        }
        if (quiche_conn_is_closed(connection->native_handle())) {
            state = State::Closed;
        }
    }

    void flush_egress(std::size_t& remaining_budget) {
        if (state == State::Closed) return;
        while (remaining_budget > 0) {
            if (pending_packet) {
                if (!send_pending()) break;
                --remaining_budget;
                continue;
            }
            if (!connection) break;
            quiche_send_info info{};
            const auto written = quiche_conn_send(
                connection->native_handle(),
                reinterpret_cast<std::uint8_t*>(send_buffer.data()),
                send_buffer.size(), &info);
            if (written == QUICHE_ERR_DONE) break;
            if (written < 0) {
                terminal_transport_error();
                break;
            }
            if (!queue_packet(
                    std::span<const std::byte>(send_buffer).first(
                        static_cast<std::size_t>(written)),
                    reinterpret_cast<const sockaddr*>(&info.from), info.from_len,
                    reinterpret_cast<const sockaddr*>(&info.to), info.to_len,
                    deadline(info.at))) {
                break;
            }
        }
    }

    void handle_packet(std::span<std::byte> packet,
                       const sockaddr_storage& peer,
                       socklen_t peer_size) {
        std::array<std::uint8_t, QUICHE_MAX_CONN_ID_LEN> scid{};
        std::array<std::uint8_t, QUICHE_MAX_CONN_ID_LEN> dcid{};
        std::array<std::uint8_t, 256> token{};
        std::size_t scid_size = scid.size();
        std::size_t dcid_size = dcid.size();
        std::size_t token_size = token.size();
        std::uint32_t version = 0;
        std::uint8_t type = 0;
        const auto destination_id_length =
            accepted_local_cid.empty() ? QUICHE_MAX_CONN_ID_LEN
                                       : accepted_local_cid.size();
        if (quiche_header_info(
                reinterpret_cast<const std::uint8_t*>(packet.data()),
                packet.size(), destination_id_length, &version, &type,
                scid.data(), &scid_size, dcid.data(), &dcid_size,
                token.data(), &token_size) != 0) {
            return;
        }
        const auto dcid_bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(dcid.data()), dcid_size);
        if (connection && !route_matches(dcid_bytes)) return;
        if (!connection && pending_packet) return;

        if (!connection) {
            if (!quiche_version_is_supported(version)) {
                const auto written = quiche_negotiate_version(
                    scid.data(), scid_size, dcid.data(), dcid_size,
                    reinterpret_cast<std::uint8_t*>(send_buffer.data()),
                    send_buffer.size());
                if (written > 0) {
                    queue_packet(
                        std::span<const std::byte>(send_buffer)
                            .first(static_cast<std::size_t>(written)),
                        reinterpret_cast<const sockaddr*>(&local_address),
                        local_address_size,
                        reinterpret_cast<const sockaddr*>(&peer), peer_size,
                        dependencies.monotonic_nanoseconds());
                }
                return;
            }
            if (type != 1) return;
            if (token_size == 0) {
                std::array<std::byte, 16> retry_scid{};
                if (!dependencies.random_bytes(retry_scid)) return;
                const auto encoded = token_codec->encode(
                    dependencies.unix_seconds(), token_address(peer),
                    dcid_bytes, retry_scid);
                if (!encoded) return;
                const auto written = quiche_retry(
                    scid.data(), scid_size, dcid.data(), dcid_size,
                    reinterpret_cast<const std::uint8_t*>(retry_scid.data()),
                    retry_scid.size(),
                    reinterpret_cast<const std::uint8_t*>(encoded->data()),
                    encoded->size(), version,
                    reinterpret_cast<std::uint8_t*>(send_buffer.data()),
                    send_buffer.size());
                if (written > 0) {
                    queue_packet(
                        std::span<const std::byte>(send_buffer)
                            .first(static_cast<std::size_t>(written)),
                        reinterpret_cast<const sockaddr*>(&local_address),
                        local_address_size,
                        reinterpret_cast<const sockaddr*>(&peer), peer_size,
                        dependencies.monotonic_nanoseconds());
                }
                return;
            }
            const auto decoded = token_codec->validate(
                {reinterpret_cast<const std::byte*>(token.data()), token_size},
                dependencies.unix_seconds(),
                static_cast<std::uint64_t>(
                    config.retry_token_lifetime.count()),
                token_address(peer), dcid_bytes);
            if (!decoded) return;
            accepted_local_cid.assign(dcid.begin(), dcid.begin() + dcid_size);
            original_destination_cid.assign(
                reinterpret_cast<const std::uint8_t*>(
                    decoded->original_dcid.data()),
                reinterpret_cast<const std::uint8_t*>(
                    decoded->original_dcid.data()) +
                    decoded->original_dcid.size());
            auto* native = quiche_accept(
                accepted_local_cid.data(), accepted_local_cid.size(),
                original_destination_cid.data(),
                original_destination_cid.size(),
                reinterpret_cast<const sockaddr*>(&local_address),
                local_address_size, reinterpret_cast<const sockaddr*>(&peer),
                peer_size, quiche_configuration);
            if (native == nullptr) return;
            auto created = detail::QuicheConnection::create(
                native, detail::default_quiche_api(),
                detail::EventLimits{config.max_events,
                                    config.max_event_payload_bytes});
            if (!created.connection) {
                quiche_conn_free(native);
                return;
            }
            connection = std::move(created.connection);
            active_local_cids.add(accepted_local_cid);
            synchronize_connection_ids();
            peer_address = peer;
            peer_address_size = peer_size;
            state = State::Handshaking;
        }

        quiche_recv_info information{
            const_cast<sockaddr*>(reinterpret_cast<const sockaddr*>(&peer)),
            peer_size,
            reinterpret_cast<sockaddr*>(&local_address), local_address_size};
        const auto received = quiche_conn_recv(
            connection->native_handle(),
            reinterpret_cast<std::uint8_t*>(packet.data()), packet.size(),
            &information);
        if (received < 0 && received != QUICHE_ERR_DONE) return;
        check_established();
        drain_application_events();
        emit_terminal();
    }

    void receive_packets() {
        if (state == State::Closed) return;
        for (std::size_t count = 0; count < config.max_datagrams_per_poll;
             ++count) {
            sockaddr_storage peer{};
            iovec vector{receive_buffer.data(), receive_buffer.size()};
            msghdr message{};
            message.msg_name = &peer;
            message.msg_namelen = sizeof(peer);
            message.msg_iov = &vector;
            message.msg_iovlen = 1;
            ssize_t received = -1;
            do {
                received = dependencies.socket.receive_message(socket_fd,
                                                               &message, 0);
            } while (received < 0 && errno == EINTR);
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (received < 0) {
                terminal_transport_error();
                break;
            }
            if ((message.msg_flags & MSG_TRUNC) != 0) continue;
            handle_packet(
                std::span<std::byte>(receive_buffer)
                    .first(static_cast<std::size_t>(received)),
                peer, message.msg_namelen);
            if (state == State::Closed) break;
        }
    }
};

struct NativeQuicListenerFactory {
    static NativeQuicListenerCreateResult make(
        std::unique_ptr<NativeQuicListener::Impl> impl) {
        return {std::unique_ptr<NativeQuicListener>(
                    new NativeQuicListener(std::move(impl))),
                std::nullopt};
    }
};

NativeQuicListener::NativeQuicListener(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
NativeQuicListener::~NativeQuicListener() = default;
NativeQuicListener::NativeQuicListener(NativeQuicListener&&) noexcept =
    default;
NativeQuicListener& NativeQuicListener::operator=(
    NativeQuicListener&&) noexcept = default;

NativeQuicListenerCreateResult NativeQuicListener::create(
    NativeQuicListenerConfig config) {
    return detail::create_native_quic_listener(
        std::move(config), detail::default_native_listener_dependencies());
}

const BoundEndpoint& NativeQuicListener::bound_endpoint() const noexcept {
    static const BoundEndpoint empty;
    return impl_ ? impl_->endpoint : empty;
}

OpenResult NativeQuicListener::open_bidi() {
    if (!impl_) return {TransportStatus::InvalidState, 0};
    const auto status = impl_->application_status();
    return status == TransportStatus::Success
               ? impl_->connection->open_bidi()
               : OpenResult{status, 0};
}

OpenResult NativeQuicListener::open_uni() {
    if (!impl_) return {TransportStatus::InvalidState, 0};
    const auto status = impl_->application_status();
    return status == TransportStatus::Success
               ? impl_->connection->open_uni()
               : OpenResult{status, 0};
}

OperationResult NativeQuicListener::write(StreamId stream_id,
                                          std::span<const std::byte> data,
                                          bool fin) {
    if (!impl_) return unavailable();
    const auto status = impl_->application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    auto result = impl_->connection->write(stream_id, data, fin);
    auto budget = impl_->config.max_egress_datagrams_per_call;
    impl_->flush_egress(budget);
    return result;
}

OperationResult NativeQuicListener::reset(
    StreamId stream_id, std::uint64_t application_error) {
    if (!impl_) return unavailable();
    const auto status = impl_->application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    auto result = impl_->connection->reset(stream_id, application_error);
    auto budget = impl_->config.max_egress_datagrams_per_call;
    impl_->flush_egress(budget);
    return result;
}

OperationResult NativeQuicListener::stop_sending(
    StreamId stream_id, std::uint64_t application_error) {
    if (!impl_) return unavailable();
    const auto status = impl_->application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    auto result = impl_->connection->stop_sending(stream_id,
                                                  application_error);
    auto budget = impl_->config.max_egress_datagrams_per_call;
    impl_->flush_egress(budget);
    return result;
}

OperationResult NativeQuicListener::send_datagram(
    std::span<const std::byte> data) {
    if (!impl_) return unavailable();
    const auto status = impl_->application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    auto result = impl_->connection->send_datagram(data);
    auto budget = impl_->config.max_egress_datagrams_per_call;
    impl_->flush_egress(budget);
    return result;
}

OperationResult NativeQuicListener::close(
    std::uint64_t application_error, std::span<const std::byte> reason) {
    if (!impl_) return unavailable();
    const auto status = impl_->application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    auto result = impl_->connection->close(application_error, reason);
    if (result.status == TransportStatus::Success) {
        impl_->state = Impl::State::Closing;
    }
    auto budget = impl_->config.max_egress_datagrams_per_call;
    impl_->flush_egress(budget);
    return result;
}

std::vector<TransportEvent> NativeQuicListener::poll(std::size_t max_events) {
    if (!impl_) return {};
    auto budget = impl_->config.max_egress_datagrams_per_call;
    impl_->flush_egress(budget);
    impl_->receive_packets();
    impl_->flush_egress(budget);
    if (impl_->connection) {
        const auto timeout =
            quiche_conn_timeout_as_nanos(impl_->connection->native_handle());
        if (timeout == 0) {
            quiche_conn_on_timeout(impl_->connection->native_handle());
            impl_->drain_application_events();
            impl_->flush_egress(budget);
        }
        impl_->check_established();
        impl_->emit_terminal();
        impl_->flush_egress(budget);
        return impl_->connection->poll(max_events);
    }
    std::vector<TransportEvent> output;
    const auto count = std::min(max_events, impl_->listener_events.size());
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        output.push_back(std::move(impl_->listener_events.front()));
        impl_->listener_events.pop_front();
    }
    return output;
}

}  // namespace moq::interop::transport

namespace moq::interop::transport::detail {

bool ActiveCidRoutes::add(std::span<const std::uint8_t> cid) {
    if (cid.empty() || contains(cid)) return false;
    routes_.emplace_back(cid.begin(), cid.end());
    return true;
}

bool ActiveCidRoutes::contains(std::span<const std::uint8_t> cid) const {
    return std::ranges::any_of(routes_, [&](const auto& route) {
        return std::ranges::equal(route, cid);
    });
}

bool ActiveCidRoutes::synchronize(std::span<const std::uint8_t> current,
                                  std::size_t active_count) {
    if (active_count == 0) {
        routes_.clear();
        return current.empty();
    }
    if (current.empty() || !contains(current) ||
        active_count > routes_.size()) {
        return false;
    }
    if (active_count == routes_.size()) return true;
    if (active_count != 1) return false;
    std::erase_if(routes_, [&](const auto& route) {
        return !std::ranges::equal(route, current);
    });
    return routes_.size() == 1;
}

namespace {

constexpr std::uint8_t kTokenVersion = 1;
constexpr std::size_t kTagLength = 32;
constexpr std::size_t kMaxCidLength = QUICHE_MAX_CONN_ID_LEN;

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value >> 8u));
    output.push_back(static_cast<std::byte>(value));
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned shift = 56; shift <= 56; shift -= 8) {
        output.push_back(static_cast<std::byte>(value >> shift));
        if (shift == 0) break;
    }
}

std::uint16_t read_u16(std::span<const std::byte> input,
                       std::size_t offset) {
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[offset]) << 8u) |
        std::to_integer<std::uint16_t>(input[offset + 1]));
}

std::uint64_t read_u64(std::span<const std::byte> input,
                       std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8u) |
                std::to_integer<std::uint8_t>(input[offset + index]);
    }
    return value;
}

std::optional<std::array<std::byte, kTagLength>> token_tag(
    std::span<const std::byte> secret, std::span<const std::byte> body) {
    std::array<std::byte, EVP_MAX_MD_SIZE> output{};
    unsigned output_size = 0;
    const auto* result = HMAC(
        EVP_sha256(), secret.data(), secret.size(),
        reinterpret_cast<const unsigned char*>(body.data()), body.size(),
        reinterpret_cast<unsigned char*>(output.data()), &output_size);
    if (result == nullptr || output_size != kTagLength) return std::nullopt;
    std::array<std::byte, kTagLength> tag{};
    std::copy_n(output.begin(), tag.size(), tag.begin());
    return tag;
}

bool valid_address(const TokenAddress& address) {
    return (address.family == 4 && address.length == 4) ||
           (address.family == 6 && address.length == 16);
}

bool constant_equal(std::span<const std::byte> left,
                    std::span<const std::byte> right) {
    return left.size() == right.size() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

NativeQuicListenerError validate_config(
    const NativeQuicListenerConfig& config) {
    constexpr auto max_varint = (std::uint64_t{1} << 62u) - 1u;
    constexpr auto max_stream_count = std::uint64_t{1} << 60u;
    if (config.bind_address == "0.0.0.0" || config.bind_address == "::") {
        return NativeQuicListenerError::UnsupportedBindAddress;
    }
    if (config.bind_address.empty() ||
        !exact_draft18_alpn(config.expected_alpn) ||
        config.idle_timeout.count() <= 0 ||
        config.retry_token_lifetime.count() <= 0 ||
        config.max_udp_payload < QUICHE_MIN_CLIENT_INITIAL_LEN ||
        config.max_udp_payload > 65'535 ||
        config.max_datagrams_per_poll == 0 ||
        config.max_egress_datagrams_per_call == 0 || config.max_events == 0 ||
        config.max_event_payload_bytes == 0 ||
        config.initial_max_data == 0 ||
        config.initial_max_stream_data_bidi_local == 0 ||
        config.initial_max_stream_data_bidi_remote == 0 ||
        config.initial_max_stream_data_uni == 0 ||
        config.initial_max_streams_bidi == 0 ||
        config.initial_max_streams_uni == 0 ||
        config.datagram_receive_queue == 0 ||
        config.datagram_send_queue == 0 ||
        static_cast<std::uint64_t>(config.idle_timeout.count()) > max_varint ||
        config.initial_max_data > max_varint ||
        config.initial_max_stream_data_bidi_local > max_varint ||
        config.initial_max_stream_data_bidi_remote > max_varint ||
        config.initial_max_stream_data_uni > max_varint ||
        config.initial_max_streams_bidi > max_stream_count ||
        config.initial_max_streams_uni > max_stream_count ||
        config.missing_datagram_application_error > max_varint ||
        config.max_additional_connection_ids > 1) {
        return NativeQuicListenerError::InvalidConfiguration;
    }
    return static_cast<NativeQuicListenerError>(-1);
}

}  // namespace

RetryTokenCodec::RetryTokenCodec(std::array<std::byte, 32> secret)
    : secret_(secret) {}

std::optional<std::vector<std::byte>> RetryTokenCodec::encode(
    std::uint64_t issue_time_seconds, const TokenAddress& address,
    std::span<const std::byte> original_dcid,
    std::span<const std::byte> retry_scid) const {
    if (!valid_address(address) || original_dcid.empty() ||
        retry_scid.empty() || original_dcid.size() > kMaxCidLength ||
        retry_scid.size() > kMaxCidLength) {
        return std::nullopt;
    }

    std::vector<std::byte> token;
    token.reserve(1 + 8 + 1 + 1 + static_cast<std::size_t>(address.length) +
                  2 + 1 +
                  original_dcid.size() + 1 + retry_scid.size() + kTagLength);
    token.push_back(static_cast<std::byte>(kTokenVersion));
    append_u64(token, issue_time_seconds);
    token.push_back(static_cast<std::byte>(address.family));
    token.push_back(static_cast<std::byte>(address.length));
    token.insert(token.end(), address.bytes.begin(),
                 address.bytes.begin() + address.length);
    append_u16(token, address.port);
    token.push_back(static_cast<std::byte>(original_dcid.size()));
    token.insert(token.end(), original_dcid.begin(), original_dcid.end());
    token.push_back(static_cast<std::byte>(retry_scid.size()));
    token.insert(token.end(), retry_scid.begin(), retry_scid.end());
    const auto tag = token_tag(secret_, token);
    if (!tag) return std::nullopt;
    token.insert(token.end(), tag->begin(), tag->end());
    return token;
}

std::optional<RetryTokenValue> RetryTokenCodec::validate(
    std::span<const std::byte> token, std::uint64_t now_seconds,
    std::uint64_t lifetime_seconds, const TokenAddress& address,
    std::span<const std::byte> current_dcid) const {
    constexpr std::size_t kFixedBeforeAddress = 1 + 8 + 1 + 1;
    constexpr std::size_t kFixedAfterAddress = 2 + 1 + 1 + kTagLength;
    if (!valid_address(address) || lifetime_seconds == 0 ||
        token.size() < kFixedBeforeAddress + 4 + kFixedAfterAddress ||
        std::to_integer<std::uint8_t>(token[0]) != kTokenVersion) {
        return std::nullopt;
    }
    const auto encoded_family = std::to_integer<std::uint8_t>(token[9]);
    const auto address_length = std::to_integer<std::uint8_t>(token[10]);
    if (!((encoded_family == 4 && address_length == 4) ||
          (encoded_family == 6 && address_length == 16))) {
        return std::nullopt;
    }
    std::size_t cursor = kFixedBeforeAddress;
    if (token.size() < cursor + address_length + kFixedAfterAddress) {
        return std::nullopt;
    }
    const auto encoded_address = token.subspan(cursor, address_length);
    cursor += address_length;
    const auto encoded_port = read_u16(token, cursor);
    cursor += 2;
    const auto odcid_length = std::to_integer<std::uint8_t>(token[cursor++]);
    if (odcid_length == 0 || odcid_length > kMaxCidLength ||
        token.size() < cursor + odcid_length + 1 + kTagLength) {
        return std::nullopt;
    }
    const auto odcid = token.subspan(cursor, odcid_length);
    cursor += odcid_length;
    const auto retry_length = std::to_integer<std::uint8_t>(token[cursor++]);
    if (retry_length == 0 || retry_length > kMaxCidLength ||
        token.size() != cursor + retry_length + kTagLength) {
        return std::nullopt;
    }
    const auto retry = token.subspan(cursor, retry_length);
    cursor += retry_length;
    const auto body = token.first(cursor);
    const auto supplied_tag = token.subspan(cursor, kTagLength);
    const auto expected_tag = token_tag(secret_, body);
    if (!expected_tag || !constant_equal(supplied_tag, *expected_tag)) {
        return std::nullopt;
    }

    const auto issue_time = read_u64(token, 1);
    if (issue_time > now_seconds || now_seconds - issue_time > lifetime_seconds ||
        encoded_family != address.family ||
        encoded_port != address.port ||
        !constant_equal(encoded_address,
                        std::span<const std::byte>(address.bytes)
                            .first(address.length)) ||
        !constant_equal(retry, current_dcid)) {
        return std::nullopt;
    }
    return RetryTokenValue{{odcid.begin(), odcid.end()},
                           {retry.begin(), retry.end()}};
}

NativeListenerDependencies default_native_listener_dependencies() {
    NativeListenerDependencies dependencies;
    dependencies.socket.open = [](int domain, int type, int protocol) {
        return ::socket(domain, type, protocol);
    };
    dependencies.socket.close = [](int fd) { return ::close(fd); };
    dependencies.socket.bind = [](int fd, const sockaddr* address,
                                  socklen_t size) {
        return ::bind(fd, address, size);
    };
    dependencies.socket.local_address = [](int fd, sockaddr* address,
                                           socklen_t* size) {
        return ::getsockname(fd, address, size);
    };
    dependencies.socket.receive_message = [](int fd, msghdr* message,
                                             int flags) {
        return ::recvmsg(fd, message, flags);
    };
    dependencies.socket.send_datagram =
        [](int fd, const void* data, std::size_t size, int flags,
           const sockaddr* address, socklen_t address_size) {
        return ::sendto(fd, data, size, flags, address, address_size);
    };
    dependencies.random_bytes = [](std::span<std::byte> output) {
        return RAND_bytes(reinterpret_cast<unsigned char*>(output.data()),
                          output.size()) == 1;
    };
    dependencies.unix_seconds = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    dependencies.monotonic_nanoseconds = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    dependencies.after_bind = [](const BoundEndpoint&) { return true; };
    return dependencies;
}

NativeQuicListenerCreateResult create_native_quic_listener(
    NativeQuicListenerConfig config, NativeListenerDependencies dependencies) {
    const auto validation = validate_config(config);
    if (validation == NativeQuicListenerError::UnsupportedBindAddress ||
        validation == NativeQuicListenerError::InvalidConfiguration) {
        return {nullptr, validation};
    }
    if (!regular_file(config.certificate_path)) {
        return {nullptr, NativeQuicListenerError::CertificateLoadFailed};
    }
    if (!regular_file(config.private_key_path)) {
        return {nullptr, NativeQuicListenerError::PrivateKeyLoadFailed};
    }
    if (!dependencies.socket.open || !dependencies.socket.close ||
        !dependencies.socket.bind || !dependencies.socket.local_address ||
        !dependencies.socket.receive_message ||
        !dependencies.socket.send_datagram || !dependencies.random_bytes ||
        !dependencies.unix_seconds ||
        !dependencies.monotonic_nanoseconds || !dependencies.after_bind) {
        return {nullptr, NativeQuicListenerError::InvalidConfiguration};
    }

    auto impl = std::make_unique<NativeQuicListener::Impl>();
    impl->config = std::move(config);
    impl->dependencies = std::move(dependencies);
    impl->quiche_configuration = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (impl->quiche_configuration == nullptr) {
        return {nullptr, NativeQuicListenerError::TransportConfigurationFailed};
    }

    std::array<std::byte, 8> wire_alpn{};
    wire_alpn[0] = std::byte{7};
    std::copy(kDraft18Alpn.begin(), kDraft18Alpn.end(),
              wire_alpn.begin() + 1);
    if (quiche_config_set_application_protos(
            impl->quiche_configuration,
            reinterpret_cast<const std::uint8_t*>(wire_alpn.data()),
            wire_alpn.size()) != 0) {
        return {nullptr, NativeQuicListenerError::TransportConfigurationFailed};
    }
    if (quiche_config_load_cert_chain_from_pem_file(
            impl->quiche_configuration,
            impl->config.certificate_path.c_str()) != 0) {
        return {nullptr, NativeQuicListenerError::CertificateLoadFailed};
    }
    if (quiche_config_load_priv_key_from_pem_file(
            impl->quiche_configuration,
            impl->config.private_key_path.c_str()) != 0) {
        return {nullptr, NativeQuicListenerError::PrivateKeyLoadFailed};
    }
    quiche_config_verify_peer(impl->quiche_configuration, false);
    quiche_config_set_max_idle_timeout(
        impl->quiche_configuration,
        static_cast<std::uint64_t>(impl->config.idle_timeout.count()));
    quiche_config_set_max_recv_udp_payload_size(
        impl->quiche_configuration, impl->config.max_udp_payload);
    quiche_config_set_max_send_udp_payload_size(
        impl->quiche_configuration, impl->config.max_udp_payload);
    quiche_config_set_initial_max_data(impl->quiche_configuration,
                                       impl->config.initial_max_data);
    quiche_config_set_initial_max_stream_data_bidi_local(
        impl->quiche_configuration,
        impl->config.initial_max_stream_data_bidi_local);
    quiche_config_set_initial_max_stream_data_bidi_remote(
        impl->quiche_configuration,
        impl->config.initial_max_stream_data_bidi_remote);
    quiche_config_set_initial_max_stream_data_uni(
        impl->quiche_configuration,
        impl->config.initial_max_stream_data_uni);
    quiche_config_set_initial_max_streams_bidi(
        impl->quiche_configuration, impl->config.initial_max_streams_bidi);
    quiche_config_set_initial_max_streams_uni(
        impl->quiche_configuration, impl->config.initial_max_streams_uni);
    quiche_config_enable_dgram(
        impl->quiche_configuration, true,
        impl->config.datagram_receive_queue,
        impl->config.datagram_send_queue);

    std::array<std::byte, 32> secret{};
    if (!impl->dependencies.random_bytes(secret)) {
        return {nullptr, NativeQuicListenerError::CryptoInitializationFailed};
    }
    impl->token_codec = std::make_unique<RetryTokenCodec>(secret);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(impl->config.bind_port);
    if (inet_pton(AF_INET, impl->config.bind_address.c_str(),
                  &address.sin_addr) != 1 || address.sin_addr.s_addr == 0) {
        return {nullptr, NativeQuicListenerError::UnsupportedBindAddress};
    }
    impl->socket_fd = impl->dependencies.socket.open(
        AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (impl->socket_fd < 0) {
        return {nullptr, NativeQuicListenerError::SocketOpenFailed};
    }
    if (impl->dependencies.socket.bind(
            impl->socket_fd, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        return {nullptr, NativeQuicListenerError::BindFailed};
    }
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (impl->dependencies.socket.local_address(
            impl->socket_fd, reinterpret_cast<sockaddr*>(&bound),
            &bound_size) != 0 ||
        bound_size != sizeof(bound)) {
        return {nullptr, NativeQuicListenerError::BoundEndpointFailed};
    }
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &bound.sin_addr, text.data(), text.size()) ==
        nullptr) {
        return {nullptr, NativeQuicListenerError::BoundEndpointFailed};
    }
    impl->endpoint = {text.data(), ntohs(bound.sin_port)};
    impl->local_address_size = bound_size;
    std::memcpy(&impl->local_address, &bound, bound_size);
    impl->receive_buffer.resize(impl->config.max_udp_payload);
    impl->send_buffer.resize(impl->config.max_udp_payload);
    if (!impl->dependencies.after_bind(impl->endpoint)) {
        return {nullptr, NativeQuicListenerError::AfterBindFailed};
    }
    return NativeQuicListenerFactory::make(std::move(impl));
}

}  // namespace moq::interop::transport::detail
