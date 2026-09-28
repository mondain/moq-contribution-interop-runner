#include "support/quiche_client.h"

#include <quiche.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace moq::interop::transport::test {
namespace {

constexpr std::size_t kPacketSize = 65'535;

bool set_nonblocking(int fd) {
    const auto flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

struct QuicheTestClient::Impl {
    int socket_fd = -1;
    quiche_config* config = nullptr;
    quiche_conn* connection = nullptr;
    sockaddr_storage local{};
    socklen_t local_size = 0;
    sockaddr_storage peer{};
    socklen_t peer_size = 0;
    std::vector<std::uint8_t> receive_buffer =
        std::vector<std::uint8_t>(kPacketSize);
    std::vector<std::uint8_t> send_buffer =
        std::vector<std::uint8_t>(kPacketSize);
    std::unordered_map<std::uint64_t, ClientStreamObservation> streams;
    std::vector<std::vector<std::byte>> datagrams;
    std::optional<ClientCloseObservation> peer_close;

    ~Impl() {
        if (connection != nullptr) quiche_conn_free(connection);
        if (config != nullptr) quiche_config_free(config);
        if (socket_fd >= 0) ::close(socket_fd);
    }

    bool flush() {
        while (true) {
            quiche_send_info info{};
            const auto written = quiche_conn_send(
                connection, send_buffer.data(), send_buffer.size(), &info);
            if (written == QUICHE_ERR_DONE) return true;
            if (written < 0) return false;
            ssize_t sent = -1;
            do {
                sent = ::sendto(socket_fd, send_buffer.data(),
                                static_cast<std::size_t>(written), 0,
                                reinterpret_cast<const sockaddr*>(&info.to),
                                info.to_len);
            } while (sent < 0 && errno == EINTR);
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }
            if (sent != written) return false;
        }
    }

    bool drain_application() {
        auto* readable = quiche_conn_readable(connection);
        if (readable != nullptr) {
            std::uint64_t stream_id = 0;
            while (quiche_stream_iter_next(readable, &stream_id)) {
                auto& observation = streams[stream_id];
                while (true) {
                    bool fin = false;
                    std::uint64_t error = 0;
                    const auto read = quiche_conn_stream_recv(
                        connection, stream_id, receive_buffer.data(),
                        receive_buffer.size(), &fin, &error);
                    if (read == QUICHE_ERR_DONE) break;
                    if (read == QUICHE_ERR_STREAM_RESET) {
                        observation.reset_error = error;
                        break;
                    }
                    if (read < 0) {
                        quiche_stream_iter_free(readable);
                        return false;
                    }
                    const auto size = static_cast<std::size_t>(read);
                    if (size > 0) {
                        observation.data.insert(
                            observation.data.end(),
                            reinterpret_cast<const std::byte*>(
                                receive_buffer.data()),
                            reinterpret_cast<const std::byte*>(
                                receive_buffer.data()) + size);
                        observation.chunk_sizes.push_back(size);
                    }
                    if (fin) {
                        observation.fin = true;
                        ++observation.fin_count;
                        break;
                    }
                    if (read == 0) break;
                }
            }
            quiche_stream_iter_free(readable);
        }
        while (true) {
            const auto front = quiche_conn_dgram_recv_front_len(connection);
            if (front == QUICHE_ERR_DONE) break;
            if (front < 0 || static_cast<std::size_t>(front) >
                                 receive_buffer.size()) {
                return false;
            }
            const auto read = quiche_conn_dgram_recv(
                connection, receive_buffer.data(), receive_buffer.size());
            if (read < 0) return false;
            datagrams.emplace_back(
                reinterpret_cast<const std::byte*>(receive_buffer.data()),
                reinterpret_cast<const std::byte*>(receive_buffer.data()) +
                    static_cast<std::size_t>(read));
        }
        if (!peer_close) {
            bool application = false;
            std::uint64_t code = 0;
            const std::uint8_t* reason = nullptr;
            std::size_t reason_size = 0;
            if (quiche_conn_peer_error(connection, &application, &code,
                                       &reason, &reason_size)) {
                std::vector<std::byte> owned_reason;
                if (reason_size > 0) {
                    owned_reason.assign(
                        reinterpret_cast<const std::byte*>(reason),
                        reinterpret_cast<const std::byte*>(reason) +
                            reason_size);
                }
                peer_close = ClientCloseObservation{
                    application, code, std::move(owned_reason)};
            }
        }
        return true;
    }
};

QuicheTestClient::QuicheTestClient(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

QuicheTestClient::~QuicheTestClient() = default;

std::unique_ptr<QuicheTestClient> QuicheTestClient::create(
    const Config& input) {
    if (input.port == 0 || input.alpn.empty() || input.alpn.size() > 255 ||
        input.datagram_queue == 0) {
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (impl->config == nullptr) return nullptr;
    std::vector<std::uint8_t> wire(input.alpn.size() + 1);
    wire[0] = static_cast<std::uint8_t>(input.alpn.size());
    std::memcpy(wire.data() + 1, input.alpn.data(), input.alpn.size());
    if (quiche_config_set_application_protos(impl->config, wire.data(),
                                              wire.size()) != 0) {
        return nullptr;
    }
    quiche_config_verify_peer(impl->config, false);
    quiche_config_set_max_idle_timeout(impl->config, 5'000);
    quiche_config_set_max_recv_udp_payload_size(impl->config, 1350);
    quiche_config_set_max_send_udp_payload_size(impl->config, 1350);
    quiche_config_set_initial_max_data(impl->config, 1u << 20);
    quiche_config_set_initial_max_stream_data_bidi_local(impl->config,
                                                         1u << 18);
    quiche_config_set_initial_max_stream_data_bidi_remote(impl->config,
                                                          1u << 18);
    quiche_config_set_initial_max_stream_data_uni(impl->config, 1u << 18);
    quiche_config_set_initial_max_streams_bidi(impl->config, 64);
    quiche_config_set_initial_max_streams_uni(impl->config, 64);
    if (input.enable_datagrams) {
        quiche_config_enable_dgram(impl->config, true, input.datagram_queue,
                                   input.datagram_queue);
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(input.port);
    if (inet_pton(AF_INET, input.host.c_str(), &peer.sin_addr) != 1) {
        return nullptr;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    impl->socket_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket_fd < 0 || !set_nonblocking(impl->socket_fd) ||
        ::bind(impl->socket_fd, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0) {
        return nullptr;
    }
    socklen_t local_size = sizeof(local);
    if (::getsockname(impl->socket_fd, reinterpret_cast<sockaddr*>(&local),
                      &local_size) != 0) {
        return nullptr;
    }
    std::memcpy(&impl->local, &local, local_size);
    impl->local_size = local_size;
    std::memcpy(&impl->peer, &peer, sizeof(peer));
    impl->peer_size = sizeof(peer);
    std::array<std::uint8_t, 16> scid{
        0, 1, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    impl->connection = quiche_connect(
        "localhost", scid.data(), scid.size(),
        reinterpret_cast<const sockaddr*>(&impl->local), impl->local_size,
        reinterpret_cast<const sockaddr*>(&impl->peer), impl->peer_size,
        impl->config);
    if (impl->connection == nullptr || !impl->flush()) return nullptr;
    return std::unique_ptr<QuicheTestClient>(
        new QuicheTestClient(std::move(impl)));
}

bool QuicheTestClient::pump() {
    if (!impl_ || !impl_->connection) return false;
    while (true) {
        sockaddr_storage from{};
        socklen_t from_size = sizeof(from);
        ssize_t received = -1;
        do {
            received = ::recvfrom(impl_->socket_fd, impl_->receive_buffer.data(),
                                  impl_->receive_buffer.size(), 0,
                                  reinterpret_cast<sockaddr*>(&from),
                                  &from_size);
        } while (received < 0 && errno == EINTR);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (received < 0) return false;
        quiche_recv_info info{
            reinterpret_cast<sockaddr*>(&from), from_size,
            reinterpret_cast<sockaddr*>(&impl_->local), impl_->local_size};
        if (quiche_conn_recv(impl_->connection, impl_->receive_buffer.data(),
                             static_cast<std::size_t>(received), &info) < 0) {
            return false;
        }
    }
    if (quiche_conn_timeout_as_nanos(impl_->connection) == 0) {
        quiche_conn_on_timeout(impl_->connection);
    }
    return impl_->drain_application() && impl_->flush();
}

bool QuicheTestClient::established() const {
    return impl_ && quiche_conn_is_established(impl_->connection);
}

bool QuicheTestClient::send_stream(std::uint64_t stream_id,
                                   std::span<const std::byte> data, bool fin) {
    const auto result = try_send_stream(stream_id, data, fin);
    return result.status == ClientStreamSendStatus::Success;
}

ClientStreamSendResult QuicheTestClient::try_send_stream(
    std::uint64_t stream_id, std::span<const std::byte> data, bool fin) {
    static constexpr std::uint8_t kEmpty = 0;
    const auto* pointer = data.empty()
                              ? &kEmpty
                              : reinterpret_cast<const std::uint8_t*>(data.data());
    std::uint64_t error = 0;
    const auto sent = quiche_conn_stream_send(impl_->connection, stream_id,
                                               pointer, data.size(), fin,
                                               &error);
    if (sent >= 0) {
        if (!impl_->flush()) return {};
        return {static_cast<std::size_t>(sent) == data.size()
                    ? ClientStreamSendStatus::Success
                    : ClientStreamSendStatus::Partial,
                static_cast<std::size_t>(sent), 0};
    }
    if (sent == QUICHE_ERR_DONE) {
        return {ClientStreamSendStatus::WouldBlock, 0, 0};
    }
    if (sent == QUICHE_ERR_STREAM_STOPPED) {
        return {ClientStreamSendStatus::PeerStopped, 0, error};
    }
    if (sent == QUICHE_ERR_STREAM_RESET) {
        return {ClientStreamSendStatus::PeerReset, 0, error};
    }
    return {};
}

bool QuicheTestClient::send_datagram(std::span<const std::byte> data) {
    static constexpr std::uint8_t kEmpty = 0;
    const auto* pointer = data.empty()
                              ? &kEmpty
                              : reinterpret_cast<const std::uint8_t*>(data.data());
    return quiche_conn_dgram_send(impl_->connection, pointer, data.size()) >= 0 &&
           impl_->flush();
}

bool QuicheTestClient::reset_stream(std::uint64_t stream_id,
                                    std::uint64_t application_error) {
    return quiche_conn_stream_shutdown(impl_->connection, stream_id,
                                       QUICHE_SHUTDOWN_WRITE,
                                       application_error) == 0 &&
           impl_->flush();
}

bool QuicheTestClient::stop_stream(std::uint64_t stream_id,
                                   std::uint64_t application_error) {
    return quiche_conn_stream_shutdown(impl_->connection, stream_id,
                                       QUICHE_SHUTDOWN_READ,
                                       application_error) == 0 &&
           impl_->flush();
}

std::size_t QuicheTestClient::available_destination_ids() const {
    return impl_ ? quiche_conn_available_dcids(impl_->connection) : 0;
}

std::optional<std::uint64_t> QuicheTestClient::migrate_source() {
    if (!impl_ || quiche_conn_scids_left(impl_->connection) == 0) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> scid{
        31, 0, 32, 0, 33, 0, 34, 0, 35, 0, 36, 0, 37, 0, 38, 0};
    std::array<std::uint8_t, 16> reset_token{
        41, 0, 42, 0, 43, 0, 44, 0, 45, 0, 46, 0, 47, 0, 48, 0};
    std::uint64_t scid_sequence = 0;
    if (quiche_conn_new_scid(impl_->connection, scid.data(), scid.size(),
                             reset_token.data(), false,
                             &scid_sequence) != 0) {
        return std::nullopt;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto replacement = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (replacement < 0 || !set_nonblocking(replacement) ||
        ::bind(replacement, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0) {
        if (replacement >= 0) ::close(replacement);
        return std::nullopt;
    }
    socklen_t local_size = sizeof(local);
    if (::getsockname(replacement, reinterpret_cast<sockaddr*>(&local),
                      &local_size) != 0) {
        ::close(replacement);
        return std::nullopt;
    }
    std::uint64_t destination_sequence = 0;
    if (quiche_conn_migrate_source(
            impl_->connection, reinterpret_cast<const sockaddr*>(&local),
            local_size, &destination_sequence) != 0) {
        ::close(replacement);
        return std::nullopt;
    }
    ::close(impl_->socket_fd);
    impl_->socket_fd = replacement;
    std::memcpy(&impl_->local, &local, local_size);
    impl_->local_size = local_size;
    if (!impl_->flush()) return std::nullopt;
    return destination_sequence;
}

bool QuicheTestClient::retire_destination_id(std::uint64_t sequence) {
    return impl_ && quiche_conn_retire_dcid(impl_->connection, sequence) == 0 &&
           impl_->flush();
}

std::optional<ClientStreamObservation> QuicheTestClient::stream(
    std::uint64_t stream_id) const {
    if (!impl_) return std::nullopt;
    const auto found = impl_->streams.find(stream_id);
    return found == impl_->streams.end()
               ? std::nullopt
               : std::optional<ClientStreamObservation>{found->second};
}

std::vector<std::vector<std::byte>> QuicheTestClient::take_datagrams() {
    if (!impl_) return {};
    auto output = std::move(impl_->datagrams);
    impl_->datagrams.clear();
    return output;
}

std::optional<ClientCloseObservation> QuicheTestClient::peer_close() const {
    return impl_ ? impl_->peer_close : std::nullopt;
}

bool QuicheTestClient::close(std::uint64_t application_error,
                             std::span<const std::byte> reason) {
    static constexpr std::uint8_t kEmpty = 0;
    const auto* pointer = reason.empty()
                              ? &kEmpty
                              : reinterpret_cast<const std::uint8_t*>(reason.data());
    return quiche_conn_close(impl_->connection, true, application_error,
                             pointer, reason.size()) == 0 &&
           impl_->flush();
}

}  // namespace moq::interop::transport::test
