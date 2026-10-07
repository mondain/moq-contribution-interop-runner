#include "moq/interop/transport/native_quic_listener.h"

#include "moq/interop/app/draft_traits.h"
#include "picoquic_connection_internal.h"

#include <picoquic_internal.h>

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
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
#include <vector>

namespace moq::interop::transport {
namespace {

bool valid_alpn(std::span<const std::byte> offered) {
    const std::string_view value{reinterpret_cast<const char*>(offered.data()),
                                 offered.size()};
    // moq-lite-06 is known but has no native session handler yet, so it is never negotiated.
    return app::known_alpn(value) && value != app::alpn(app::DraftVersion::MoqLite06);
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
    sockaddr_storage pending_destination{};
    std::size_t pending_send_length = 0;
    StreamId next_bidi = 1;
    StreamId next_uni = 3;
    std::unordered_set<StreamId> reserved_streams;
    std::unordered_set<StreamId> peer_bidi_streams;
    std::unordered_set<StreamId> finished_streams;
    std::string close_reason;
    bool closing = false;
    bool drop_inbound = false;

    std::size_t queued_stream_bytes() const {
        auto* active = connection.connection();
        if (active == nullptr) return 0;
        std::size_t total = 0;
        const auto count = [&](const auto& streams) {
            for (const auto stream_id : streams) {
                const auto* stream = picoquic_find_stream(active, stream_id);
                if (stream == nullptr) continue;
                for (auto* node = stream->send_queue; node != nullptr;
                     node = node->next_stream_data) {
                    if (node->length > config.max_queued_send_bytes - total) {
                        return false;
                    }
                    total += node->length;
                }
            }
            return true;
        };
        if (!count(reserved_streams) || !count(peer_bidi_streams)) {
            return config.max_queued_send_bytes;
        }
        return total;
    }

    OpenResult open_stream(bool unidirectional) {
        const auto status = connection.application_status();
        if (status != TransportStatus::Success) return {status, 0};
        auto* active = connection.connection();
        const auto* peer_limits = picoquic_get_transport_parameters(active, 0);
        if (peer_limits == nullptr) {
            return {TransportStatus::InternalError, 0};
        }
        auto& next = unidirectional ? next_uni : next_bidi;
        const auto maximum = unidirectional ? active->max_streams_unidir_remote
                                            : active->max_streams_bidir_remote;
        if ((next >> 2u) >= maximum) return {TransportStatus::StreamLimit, 0};
        const auto result = next;
        if (next > std::numeric_limits<StreamId>::max() - 4) {
            next = std::numeric_limits<StreamId>::max();
        } else {
            next += 4;
        }
        reserved_streams.insert(result);
        return {TransportStatus::Success, result};
    }

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
            if (drop_inbound) continue;
            auto local = local_address;
            picoquic_incoming_packet(
                quic, receive.data(), static_cast<std::size_t>(length),
                reinterpret_cast<sockaddr*>(&peer),
                reinterpret_cast<sockaddr*>(&local), 0, 0,
                picoquic_current_time());
            connection.observe_transport_state();
        }
    }

    void pump_send() {
        for (std::size_t index = 0;
             index < config.max_egress_datagrams_per_call; ++index) {
            if (pending_send_length == 0) {
                sockaddr_storage source{};
                picoquic_connection_id_t log_id{};
                picoquic_cnx_t* last_connection = nullptr;
                int interface_index = 0;
                const int result = picoquic_prepare_next_packet(
                    quic, picoquic_current_time(), send.data(), send.size(),
                    &pending_send_length, &pending_destination, &source,
                    &interface_index, &log_id, &last_connection);
                if (result != 0) {
                    connection.fail();
                    break;
                }
                if (pending_send_length == 0) break;
            }
            const auto length = pending_send_length;
            const auto sent = ::sendto(
                socket_fd, send.data(), length, 0,
                reinterpret_cast<const sockaddr*>(&pending_destination),
                address_size(pending_destination));
            // The QUIC engine has already consumed this packet. Keep it until
            // the socket accepts it instead of preparing a replacement.
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (sent < 0 && errno == EINTR) continue;
            pending_send_length = 0;
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
        config.max_queued_send_bytes == 0 ||
        config.idle_timeout <= std::chrono::milliseconds{0} ||
        config.retry_token_lifetime != std::chrono::seconds{120} ||
        config.initial_max_data >= (std::uint64_t{1} << 62u) ||
        config.initial_max_stream_data_bidi_local >= (std::uint64_t{1} << 62u) ||
        config.initial_max_stream_data_bidi_remote >= (std::uint64_t{1} << 62u) ||
        config.initial_max_stream_data_uni >= (std::uint64_t{1} << 62u) ||
        config.initial_max_streams_bidi > (std::uint64_t{1} << 60u) ||
        config.initial_max_streams_uni > (std::uint64_t{1} << 60u) ||
        config.missing_datagram_application_error >= (std::uint64_t{1} << 62u)) {
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

    impl->socket_fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
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
    static const BoundEndpoint empty{};
    return impl_ ? impl_->endpoint : empty;
}

OpenResult NativeQuicListener::open_bidi() {
    return impl_ ? impl_->open_stream(false) : OpenResult{TransportStatus::InvalidState, 0};
}
OpenResult NativeQuicListener::open_uni() {
    return impl_ ? impl_->open_stream(true) : OpenResult{TransportStatus::InvalidState, 0};
}
OperationResult NativeQuicListener::write(StreamId stream_id,
                                          std::span<const std::byte> data,
                                          bool fin) {
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    const auto status = impl_->connection.application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    auto* active = impl_->connection.connection();
    if ((!impl_->reserved_streams.contains(stream_id) &&
         !impl_->peer_bidi_streams.contains(stream_id)) ||
        impl_->finished_streams.contains(stream_id)) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    if (const auto* stream = picoquic_find_stream(active, stream_id);
        stream != nullptr && stream->stop_sending_received) {
        return {TransportStatus::PeerStopped, 0, stream->remote_stop_error};
    }
    const auto queued = impl_->queued_stream_bytes();
    const auto accepted = std::min(data.size(), impl_->config.max_queued_send_bytes - queued);
    if (!data.empty() && accepted == 0) {
        return {TransportStatus::WouldBlock, 0, std::nullopt};
    }
    const bool accepted_fin = fin && accepted == data.size();
    static constexpr std::uint8_t kEmpty = 0;
    const auto* pointer = data.empty()
                              ? &kEmpty
                              : reinterpret_cast<const std::uint8_t*>(
                                    data.data());
    const int result = picoquic_add_to_stream(active, stream_id, pointer,
                                              accepted, accepted_fin ? 1 : 0);
    if (result == PICOQUIC_ERROR_INVALID_STREAM_ID) {
        return {TransportStatus::StreamLimit, 0, std::nullopt};
    }
    if (result != 0) {
        return {TransportStatus::InternalError, 0, std::nullopt};
    }
    if (accepted_fin) impl_->finished_streams.insert(stream_id);
    impl_->pump_send();
    return {accepted == data.size() ? TransportStatus::Success : TransportStatus::Partial,
            accepted, std::nullopt};
}
OperationResult NativeQuicListener::reset(StreamId stream_id,
                                          std::uint64_t application_error) {
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    const auto status = impl_->connection.application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    if (application_error >= (std::uint64_t{1} << 62u)) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    auto* active = impl_->connection.connection();
    if ((!impl_->reserved_streams.contains(stream_id) &&
         !impl_->peer_bidi_streams.contains(stream_id)) ||
        impl_->finished_streams.contains(stream_id)) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    const int result = picoquic_reset_stream(active, stream_id,
                                             application_error);
    if (result == PICOQUIC_ERROR_INVALID_STREAM_ID ||
        result == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    if (result != 0) {
        return {TransportStatus::InternalError, 0, std::nullopt};
    }
    impl_->finished_streams.insert(stream_id);
    return {TransportStatus::Success, 0, std::nullopt};
}
OperationResult NativeQuicListener::stop_sending(
    StreamId stream_id, std::uint64_t application_error) {
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    const auto status = impl_->connection.application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    if (application_error >= (std::uint64_t{1} << 62u)) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    auto* active = impl_->connection.connection();
    if ((stream_id & 3u) == 3u) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    const int result = picoquic_stop_sending(active, stream_id,
                                             application_error);
    if (result == PICOQUIC_ERROR_INVALID_STREAM_ID ||
        result == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    if (result != 0) {
        return {TransportStatus::InternalError, 0, std::nullopt};
    }
    return {TransportStatus::Success, 0, std::nullopt};
}
OperationResult NativeQuicListener::send_datagram(
    std::span<const std::byte> data) {
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    const auto status = impl_->connection.application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    auto* active = impl_->connection.connection();
    if (data.size() > impl_->connection.max_datagram_payload()) {
        return {TransportStatus::DatagramTooLarge, 0, std::nullopt};
    }
    static constexpr std::uint8_t kEmpty = 0;
    const auto* pointer = data.empty()
                              ? &kEmpty
                              : reinterpret_cast<const std::uint8_t*>(
                                    data.data());
    const int result = picoquic_queue_datagram_frame(active, data.size(),
                                                     pointer);
    if (result == PICOQUIC_ERROR_DATAGRAM_TOO_LONG) {
        return {TransportStatus::DatagramTooLarge, 0, std::nullopt};
    }
    if (result != 0) {
        return {TransportStatus::InternalError, 0, std::nullopt};
    }
    return {TransportStatus::Success, data.size(), std::nullopt};
}
OperationResult NativeQuicListener::grant_peer_streams(bool bidirectional,
                                                      std::uint64_t additional) {
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    const auto status = impl_->connection.application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    return detail::grant_peer_streams(impl_->connection.connection(), bidirectional, additional);
}
OperationResult NativeQuicListener::set_inbound_drop(bool enabled) {
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    impl_->drop_inbound = enabled;
    return {TransportStatus::Success, 0, std::nullopt};
}
OperationResult NativeQuicListener::close(
    std::uint64_t application_error, std::span<const std::byte> reason) {
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    if (impl_->closing) {
        return {TransportStatus::ConnectionClosed, 0, std::nullopt};
    }
    if (!impl_) return {TransportStatus::InvalidState, 0, std::nullopt};
    const auto status = impl_->connection.application_status();
    if (status != TransportStatus::Success) return {status, 0, std::nullopt};
    if (application_error >= (std::uint64_t{1} << 62u)) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    auto* active = impl_->connection.connection();
    if (reason.size() > impl_->config.max_udp_payload - 50 ||
        std::find(reason.begin(), reason.end(), std::byte{0}) !=
            reason.end()) {
        return {TransportStatus::InvalidState, 0, std::nullopt};
    }
    impl_->close_reason.clear();
    for (const auto value : reason) {
        impl_->close_reason.push_back(static_cast<char>(value));
    }
    const int result = picoquic_close_ex(active, application_error,
                                         impl_->close_reason.c_str());
    if (result != 0) {
        return {TransportStatus::InternalError, 0, std::nullopt};
    }
    impl_->closing = true;
    impl_->connection.note_local_close(application_error, reason);
    impl_->pump_send();
    return {TransportStatus::Success, 0, std::nullopt};
}
std::vector<TransportEvent> NativeQuicListener::poll(std::size_t max_events) {
    if (!impl_) return {};
    impl_->pump_receive();
    impl_->pump_send();
    auto events = impl_->connection.drain(max_events);
    for (const auto& event : events) {
        if (const auto* stream = std::get_if<StreamDataEvent>(&event)) {
            if ((stream->stream_id & 3) == 0) {
                impl_->peer_bidi_streams.insert(stream->stream_id);
            }
        }
    }
    return events;
}

}  // namespace moq::interop::transport
