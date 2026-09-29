#include "moq/interop/transport/native_quic_listener.h"

#include "picoquic_connection_internal.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moq::interop::transport {
namespace {

bool valid_alpn(std::span<const std::byte> offered) {
    const std::string_view value{reinterpret_cast<const char*>(offered.data()),
                                 offered.size()};
    return value == "moqt-18" || value == "moqt-21";
}

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

socklen_t address_size(const sockaddr_storage& address) {
    return address.ss_family == AF_INET6 ? sizeof(sockaddr_in6)
                                          : sizeof(sockaddr_in);
}

}  // namespace

struct NativeQuicListener::Impl {
    explicit Impl(NativeQuicListenerConfig settings)
        : config(std::move(settings)), connection(config),
          alpn(reinterpret_cast<const char*>(config.expected_alpn.data()),
               config.expected_alpn.size()), receive(config.max_udp_payload),
          send(config.max_udp_payload) {}

    ~Impl() {
        if (quic != nullptr) picoquic_free(quic);
        if (socket_fd >= 0) ::close(socket_fd);
    }

    NativeQuicListenerConfig config;
    detail::PicoquicConnectionState connection;
    std::string alpn;
    BoundEndpoint endpoint;
    int socket_fd = -1;
    picoquic_quic_t* quic = nullptr;
    sockaddr_storage local_address{};
    std::vector<std::uint8_t> receive;
    std::vector<std::uint8_t> send;

    void pump_receive() {
        for (std::size_t index = 0; index < config.max_datagrams_per_poll;
             ++index) {
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            const auto length = ::recvfrom(
                socket_fd, receive.data(), receive.size(), 0,
                reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (length < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                connection.fail();
                break;
            }
            if (length == 0) continue;
            auto local = local_address;
            picoquic_incoming_packet(
                quic, receive.data(), static_cast<std::size_t>(length),
                reinterpret_cast<sockaddr*>(&peer),
                reinterpret_cast<sockaddr*>(&local), 0, 0,
                picoquic_current_time());
        }
    }

    void pump_send() {
        for (std::size_t index = 0;
             index < config.max_egress_datagrams_per_call; ++index) {
            sockaddr_storage destination{};
            sockaddr_storage source{};
            picoquic_connection_id_t log_id{};
            picoquic_cnx_t* last_connection = nullptr;
            std::size_t length = 0;
            int interface_index = 0;
            const int result = picoquic_prepare_next_packet(
                quic, picoquic_current_time(), send.data(), send.size(),
                &length, &destination, &source, &interface_index, &log_id,
                &last_connection);
            if (result != 0) {
                connection.fail();
                break;
            }
            if (length == 0) break;
            const auto sent = ::sendto(
                socket_fd, send.data(), length, 0,
                reinterpret_cast<const sockaddr*>(&destination),
                address_size(destination));
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (sent < 0 || static_cast<std::size_t>(sent) != length) {
                connection.fail();
                break;
            }
        }
    }
};

NativeQuicListener::NativeQuicListener(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

NativeQuicListener::~NativeQuicListener() = default;
NativeQuicListener::NativeQuicListener(NativeQuicListener&&) noexcept = default;
NativeQuicListener& NativeQuicListener::operator=(NativeQuicListener&&) noexcept =
    default;

NativeQuicListenerCreateResult NativeQuicListener::create(
    NativeQuicListenerConfig config) {
    if (!valid_alpn(config.expected_alpn) || config.max_udp_payload < 1200 ||
        config.max_datagrams_per_poll == 0 ||
        config.max_egress_datagrams_per_call == 0 || config.max_events == 0 ||
        config.max_event_payload_bytes == 0 ||
        config.idle_timeout <= std::chrono::milliseconds{0} ||
        config.retry_token_lifetime <= std::chrono::seconds{0}) {
        return {nullptr, NativeQuicListenerError::InvalidConfiguration};
    }
    if (!regular_file(config.certificate_path)) {
        return {nullptr, NativeQuicListenerError::CertificateLoadFailed};
    }
    if (!regular_file(config.private_key_path)) {
        return {nullptr, NativeQuicListenerError::PrivateKeyLoadFailed};
    }

    auto impl = std::make_unique<Impl>(std::move(config));
    sockaddr_storage address{};
    socklen_t length = 0;
    int family = AF_INET;
    auto* ipv4 = reinterpret_cast<sockaddr_in*>(&address);
    if (::inet_pton(AF_INET, impl->config.bind_address.c_str(),
                    &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(impl->config.bind_port);
        length = sizeof(sockaddr_in);
    } else {
        auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&address);
        if (::inet_pton(AF_INET6, impl->config.bind_address.c_str(),
                        &ipv6->sin6_addr) != 1) {
            return {nullptr, NativeQuicListenerError::UnsupportedBindAddress};
        }
        family = AF_INET6;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(impl->config.bind_port);
        length = sizeof(sockaddr_in6);
    }

    impl->socket_fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket_fd < 0) {
        return {nullptr, NativeQuicListenerError::SocketOpenFailed};
    }
    const int flags = ::fcntl(impl->socket_fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(impl->socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return {nullptr, NativeQuicListenerError::SocketConfigurationFailed};
    }
    if (::bind(impl->socket_fd, reinterpret_cast<sockaddr*>(&address), length) <
        0) {
        return {nullptr, NativeQuicListenerError::BindFailed};
    }
    socklen_t bound_size = sizeof(impl->local_address);
    if (::getsockname(impl->socket_fd,
                      reinterpret_cast<sockaddr*>(&impl->local_address),
                      &bound_size) < 0) {
        return {nullptr, NativeQuicListenerError::BoundEndpointFailed};
    }
    impl->endpoint.address = impl->config.bind_address;
    if (family == AF_INET) {
        impl->endpoint.port = ntohs(
            reinterpret_cast<const sockaddr_in*>(&impl->local_address)
                ->sin_port);
    } else {
        impl->endpoint.port = ntohs(
            reinterpret_cast<const sockaddr_in6*>(&impl->local_address)
                ->sin6_port);
    }

    impl->quic = picoquic_create(
        1, impl->config.certificate_path.c_str(),
        impl->config.private_key_path.c_str(), nullptr, impl->alpn.c_str(),
        detail::PicoquicConnectionState::callback, &impl->connection, nullptr,
        nullptr, nullptr, picoquic_current_time(), nullptr, nullptr, nullptr,
        0);
    if (impl->quic == nullptr) {
        return {nullptr, NativeQuicListenerError::CryptoInitializationFailed};
    }
    picoquic_set_cookie_mode(impl->quic, 1);
    const std::array parameters{
        std::pair{picoquic_tp_idle_timeout,
                  static_cast<std::uint64_t>(impl->config.idle_timeout.count())},
        std::pair{picoquic_tp_initial_max_data,
                  impl->config.initial_max_data},
        std::pair{picoquic_tp_initial_max_stream_data_bidi_local,
                  impl->config.initial_max_stream_data_bidi_local},
        std::pair{picoquic_tp_initial_max_stream_data_bidi_remote,
                  impl->config.initial_max_stream_data_bidi_remote},
        std::pair{picoquic_tp_initial_max_stream_data_uni,
                  impl->config.initial_max_stream_data_uni},
        std::pair{picoquic_tp_initial_max_streams_bidi,
                  impl->config.initial_max_streams_bidi},
        std::pair{picoquic_tp_initial_max_streams_uni,
                  impl->config.initial_max_streams_uni},
        std::pair{picoquic_tp_max_datagram_frame_size,
                  static_cast<std::uint64_t>(impl->config.max_udp_payload)},
    };
    for (const auto& [type, value] : parameters) {
        if (picoquic_set_default_tp_value(
                impl->quic, static_cast<std::uint64_t>(type), value) != 0) {
            return {nullptr,
                    NativeQuicListenerError::TransportConfigurationFailed};
        }
    }
    return {std::unique_ptr<NativeQuicListener>(
                new NativeQuicListener(std::move(impl))),
            std::nullopt};
}

const BoundEndpoint& NativeQuicListener::bound_endpoint() const noexcept {
    return impl_->endpoint;
}

OpenResult NativeQuicListener::open_bidi() {
    return {TransportStatus::InvalidState, 0};
}
OpenResult NativeQuicListener::open_uni() {
    return {TransportStatus::InvalidState, 0};
}
OperationResult NativeQuicListener::write(StreamId,
                                          std::span<const std::byte>, bool) {
    return {TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult NativeQuicListener::reset(StreamId, std::uint64_t) {
    return {TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult NativeQuicListener::stop_sending(StreamId, std::uint64_t) {
    return {TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult NativeQuicListener::send_datagram(
    std::span<const std::byte>) {
    return {TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult NativeQuicListener::close(
    std::uint64_t, std::span<const std::byte>) {
    return {TransportStatus::InvalidState, 0, std::nullopt};
}
std::vector<TransportEvent> NativeQuicListener::poll(std::size_t max_events) {
    impl_->pump_receive();
    impl_->pump_send();
    return impl_->connection.drain(max_events);
}

}  // namespace moq::interop::transport
