#include "moq/interop/transport/webtransport_listener.h"

#include "moq/interop/app/draft_traits.h"
#include "transport/webtransport_connect.h"
#include "transport/webtransport_session.h"

#include <pico_webtransport.h>
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
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

extern "C" int picowt_set_wt_protocol(h3zero_stream_ctx_t* stream_ctx,
                                        const char* selected_protocol);

namespace moq::interop::transport {
namespace {

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

socklen_t address_size(const sockaddr_storage& address) {
    return address.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

std::string header_string(const std::uint8_t* data, std::size_t length) {
    return data == nullptr ? std::string{} :
           std::string{reinterpret_cast<const char*>(data), length};
}

std::vector<std::byte> cid_bytes(picoquic_connection_id_t id) {
    std::vector<std::byte> value(id.id_len);
    if (id.id_len != 0) std::memcpy(value.data(), id.id, id.id_len);
    return value;
}

std::vector<std::byte> application_protocol_bytes(const std::string& value) {
    std::vector<std::byte> result;
    result.reserve(value.size());
    for (const char byte : value)
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    return result;
}

std::string authority_host(std::string host) {
    if (host.find(':') != std::string::npos &&
        (host.empty() || host.front() != '['))
        return "[" + host + "]";
    return host;
}

}  // namespace

struct WebTransportListener::Impl {
    explicit Impl(WebTransportListenerConfig settings)
        : config(std::move(settings)), receive(config.quic.max_udp_payload),
          send(config.quic.max_udp_payload) {}

    ~Impl() {
        if (session) session->detach();
        picowt_release_capsule(&capsule);
        if (quic != nullptr) picoquic_free(quic);
        if (socket_fd >= 0) ::close(socket_fd);
    }

    WebTransportListenerConfig config;
    BoundEndpoint endpoint;
    RunEndpoint run_endpoint;
    WebTransportProfile profile = WebTransportProfile::Draft18Wt15;
    picohttp_server_path_item_t route{};
    picohttp_server_parameters_t parameters{};
    picoquic_quic_t* quic = nullptr;
    picoquic_cnx_t* session_connection = nullptr;
    bool drop_inbound = false;
    std::unique_ptr<WebTransportSession> session;
    picowt_capsule_t capsule{};
    int socket_fd = -1;
    sockaddr_storage local_address{};
    std::vector<std::uint8_t> receive;
    std::vector<std::uint8_t> send;

    static int callback(picoquic_cnx_t* cnx, std::uint64_t stream_id,
                        std::uint8_t* bytes, std::size_t length,
                        picoquic_call_back_event_t event, void* callback_ctx,
                        void* stream_ctx) {
        if (callback_ctx == nullptr) return -1;
        const auto* defaults = picoquic_get_default_callback_context(cnx->quic);
        Impl* self = nullptr;
        if (callback_ctx == defaults) {
            auto* parameters = static_cast<picohttp_server_parameters_t*>(callback_ctx);
            if (parameters->path_table_nb != 1) return -1;
            self = static_cast<Impl*>(parameters->path_table[0].path_app_ctx);
        } else {
            auto* h3 = static_cast<h3zero_callback_ctx_t*>(callback_ctx);
            if (h3->path_table_nb != 1) return -1;
            self = static_cast<Impl*>(h3->path_table[0].path_app_ctx);
        }
        if (self == nullptr) return -1;
        const bool connection_closing = event == picoquic_callback_close ||
            event == picoquic_callback_application_close ||
            event == picoquic_callback_stateless_reset;
        if (connection_closing && self->session_connection == cnx && self->session) {
            const auto space = event == picoquic_callback_application_close
                ? CloseErrorSpace::Application : CloseErrorSpace::Transport;
            const auto error = space == CloseErrorSpace::Application
                ? picoquic_get_application_error(cnx) : picoquic_get_remote_error(cnx);
            std::span<const std::byte> reason;
            if (cnx->remote_error_reason != nullptr) {
                const auto length = ::strnlen(cnx->remote_error_reason,
                    self->config.quic.max_event_payload_bytes + 1);
                reason = {reinterpret_cast<const std::byte*>(cnx->remote_error_reason),
                          length};
            }
            self->session->ingest_connection_close(space, error, reason);
            self->session_connection = nullptr;
        }
        const auto result = h3zero_callback(cnx, stream_id, bytes, length, event,
                                            callback_ctx, stream_ctx);
        if (!connection_closing) {
            auto* h3 = picoquic_get_callback_context(cnx);
            if (h3 != nullptr) picoquic_set_callback(cnx, callback, h3);
        }
        return result;
    }

    static int route_callback(picoquic_cnx_t* cnx, std::uint8_t* bytes,
                              std::size_t length, picohttp_call_back_event_t event,
                              h3zero_stream_ctx_t* stream_ctx, void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr || stream_ctx == nullptr) return -1;
        if (event == picohttp_callback_connect)
            return self->on_connect(cnx, stream_ctx);
        if (self->session == nullptr || self->session_connection != cnx)
            return 0;
        if (event == picohttp_callback_post_data ||
            event == picohttp_callback_post_fin) {
            if (stream_ctx->stream_id == self->session->connect_stream_id())
                return self->on_control(cnx, bytes, length,
                                        event == picohttp_callback_post_fin);
            if (self->config.quic.hold_uni_stream_credit && (stream_ctx->stream_id & 3u) == 2u)
                (void)picoquic_set_app_flow_control(cnx, stream_ctx->stream_id, 1);
            const auto* payload = reinterpret_cast<const std::byte*>(bytes);
            if (!self->session->ingest_stream(
                    stream_ctx->stream_id,
                    stream_ctx->ps.stream_state.control_stream_id,
                    {payload, length}, event == picohttp_callback_post_fin))
                return -1;
            return 0;
        }
        if (event == picohttp_callback_post_datagram) {
            const auto* payload = reinterpret_cast<const std::byte*>(bytes);
            (void)self->session->ingest_datagram(stream_ctx->stream_id,
                                                  {payload, length});
            return 0;
        }
        if (event == picohttp_callback_reset) {
            self->session->ingest_reset(stream_ctx->stream_id,
                picoquic_get_remote_stream_error(cnx, stream_ctx->stream_id));
        } else if (event == picohttp_callback_stop_sending) {
            const auto* stream = picoquic_find_stream(cnx, stream_ctx->stream_id);
            if (stream != nullptr)
                self->session->ingest_stop_sending(stream_ctx->stream_id,
                                                    stream->remote_stop_error);
        } else if (event == picohttp_callback_deregister) {
            self->session->detach();
            self->session_connection = nullptr;
        }
        return 0;
    }

    int on_connect(picoquic_cnx_t* cnx, h3zero_stream_ctx_t* control) {
        if (session != nullptr) return -1;
        auto* h3 = static_cast<h3zero_callback_ctx_t*>(picoquic_get_callback_context(cnx));
        const auto* peer = picoquic_get_transport_parameters(cnx, 0);
        if (h3 == nullptr || peer == nullptr) return -1;
        const auto& parts = control->ps.stream_state.header;
        H3Request request;
        request.method = parts.method == h3zero_method_connect ? "CONNECT" : "invalid";
        request.protocol = header_string(parts.protocol, parts.protocol_length);
        request.scheme = parts.scheme_present && parts.scheme_https ? "https" : "invalid";
        request.authority = header_string(parts.authority, parts.authority_length);
        request.path = header_string(parts.path, parts.path_length);
        if (parts.origin != nullptr)
            request.headers.emplace_back("origin",
                header_string(parts.origin, parts.origin_length));
        if (parts.wt_available_protocols != nullptr)
            request.headers.emplace_back("wt-available-protocols",
                header_string(parts.wt_available_protocols,
                              parts.wt_available_protocols_length));
        const PeerCapabilities caps{
            h3->settings.settings_received != 0,
            h3->settings.webtransport_enabled,
            h3->settings.h3_datagram != 0,
            peer->max_datagram_frame_size > 0,
            peer->is_reset_stream_at_enabled != 0};
        const auto decision = validate_connect(request, caps, run_endpoint, profile);
        if (!decision.accepted()) return -1;
        if (picowt_set_wt_protocol(control, decision.selected_protocol.c_str()) != 0)
            return -1;
        if (h3zero_declare_stream_prefix(h3, control->stream_id,
                                         route_callback, this) != 0)
            return -1;
        const auto max_datagram = std::min<std::size_t>(
            {peer->max_datagram_frame_size, config.quic.max_udp_payload - 50,
             config.quic.max_event_payload_bytes});
        const auto usable = max_datagram > 8 ? max_datagram - 8 : 0;
        session = std::make_unique<WebTransportSession>(
            control->stream_id,
            WebTransportSessionLimits{config.quic.max_events,
                                      config.quic.max_event_payload_bytes,
                                      usable,
                                      config.quic.max_queued_send_bytes},
            cnx, h3, control, route_callback, this);
        session_connection = cnx;
        session->establish(application_protocol_bytes(config.application_protocol),
                           cid_bytes(picoquic_get_local_cnxid(cnx)),
                           cid_bytes(picoquic_get_remote_cnxid(cnx)));
        return 0;
    }

    int on_control(picoquic_cnx_t* cnx, std::uint8_t* bytes,
                   std::size_t length, bool fin) {
        if (bytes != nullptr && length != 0 &&
            picowt_receive_capsule(cnx, bytes, bytes + length, &capsule) != 0)
            return -1;
        if (capsule.h3_capsule.is_stored &&
            capsule.h3_capsule.capsule_type == picowt_capsule_close_webtransport_session) {
            const auto* reason = reinterpret_cast<const std::byte*>(capsule.error_msg);
            const std::span<const std::byte> message{reason, capsule.error_msg_len};
            if (!valid_webtransport_close_reason(message)) return -1;
            session->ingest_peer_close(capsule.error_code, message);
            session_connection = nullptr;
            picowt_release_capsule(&capsule);
            return 0;
        }
        if (fin) {
            session->ingest_peer_close(0, {});
            session_connection = nullptr;
        }
        return 0;
    }

    void pump_receive() {
        for (std::size_t index = 0; index < config.quic.max_datagrams_per_poll;
             ++index) {
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            const auto length = ::recvfrom(socket_fd, receive.data(), receive.size(), 0,
                                           reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (length < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                break;
            }
            if (length == 0) continue;
            if (drop_inbound) continue;
            auto local = local_address;
            (void)picoquic_incoming_packet(quic, receive.data(),
                    static_cast<std::size_t>(length),
                    reinterpret_cast<sockaddr*>(&peer),
                    reinterpret_cast<sockaddr*>(&local), 0, 0,
                    picoquic_current_time());
        }
    }

    void pump_send() {
        for (std::size_t index = 0;
             index < config.quic.max_egress_datagrams_per_call; ++index) {
            sockaddr_storage destination{};
            sockaddr_storage source{};
            picoquic_connection_id_t log_id{};
            picoquic_cnx_t* last_connection = nullptr;
            std::size_t length = 0;
            int interface_index = 0;
            if (picoquic_prepare_next_packet(
                    quic, picoquic_current_time(), send.data(), send.size(),
                    &length, &destination, &source, &interface_index, &log_id,
                    &last_connection) != 0) break;
            if (length == 0) break;
            const auto sent = ::sendto(socket_fd, send.data(), length, 0,
                          reinterpret_cast<const sockaddr*>(&destination),
                          address_size(destination));
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (sent < 0 || static_cast<std::size_t>(sent) != length) break;
        }
    }
};

WebTransportListener::WebTransportListener(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
WebTransportListener::~WebTransportListener() = default;
WebTransportListener::WebTransportListener(WebTransportListener&&) noexcept = default;
WebTransportListener& WebTransportListener::operator=(WebTransportListener&&) noexcept = default;

WebTransportListenerCreateResult WebTransportListener::create(
    WebTransportListenerConfig config) {
    // moq-lite-06 is a known protocol identifier but has no WebTransport session handler yet.
    if (!app::known_alpn(config.application_protocol) ||
        config.application_protocol == app::alpn(app::DraftVersion::MoqLite06) ||
        config.path.empty() || config.path.front() != '/' ||
        (config.require_origin && config.allowed_origins.empty()) ||
        config.quic.max_udp_payload < 1200 ||
        config.quic.max_datagrams_per_poll == 0 ||
        config.quic.max_egress_datagrams_per_call == 0 ||
        config.quic.max_events == 0 || config.quic.max_event_payload_bytes == 0 ||
        config.quic.max_queued_send_bytes == 0 ||
        config.quic.idle_timeout <= std::chrono::milliseconds{0} ||
        config.quic.retry_token_lifetime != std::chrono::seconds{120})
        return {nullptr, NativeQuicListenerError::InvalidConfiguration};
    if (!regular_file(config.quic.certificate_path))
        return {nullptr, NativeQuicListenerError::CertificateLoadFailed};
    if (!regular_file(config.quic.private_key_path))
        return {nullptr, NativeQuicListenerError::PrivateKeyLoadFailed};

    auto impl = std::make_unique<Impl>(std::move(config));
    sockaddr_storage address{};
    socklen_t length = 0;
    int family = AF_INET;
    auto* ipv4 = reinterpret_cast<sockaddr_in*>(&address);
    if (::inet_pton(AF_INET, impl->config.quic.bind_address.c_str(),
                    &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(impl->config.quic.bind_port);
        length = sizeof(sockaddr_in);
    } else {
        auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&address);
        if (::inet_pton(AF_INET6, impl->config.quic.bind_address.c_str(),
                        &ipv6->sin6_addr) != 1)
            return {nullptr, NativeQuicListenerError::UnsupportedBindAddress};
        family = AF_INET6;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(impl->config.quic.bind_port);
        length = sizeof(sockaddr_in6);
    }
    impl->socket_fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (impl->socket_fd < 0)
        return {nullptr, NativeQuicListenerError::SocketOpenFailed};
    const int flags = ::fcntl(impl->socket_fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(impl->socket_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return {nullptr, NativeQuicListenerError::SocketConfigurationFailed};
    if (::bind(impl->socket_fd, reinterpret_cast<sockaddr*>(&address), length) < 0)
        return {nullptr, NativeQuicListenerError::BindFailed};
    socklen_t bound_size = sizeof(impl->local_address);
    if (::getsockname(impl->socket_fd,
                      reinterpret_cast<sockaddr*>(&impl->local_address),
                      &bound_size) < 0)
        return {nullptr, NativeQuicListenerError::BoundEndpointFailed};
    impl->endpoint.address = impl->config.quic.bind_address;
    impl->endpoint.port = family == AF_INET
        ? ntohs(reinterpret_cast<const sockaddr_in*>(&impl->local_address)->sin_port)
        : ntohs(reinterpret_cast<const sockaddr_in6*>(&impl->local_address)->sin6_port);

    if (impl->config.authority.empty())
        impl->config.authority = authority_host(impl->config.advertised_host.empty()
                                     ? impl->endpoint.address
                                     : impl->config.advertised_host) + ":" +
                                 std::to_string(impl->endpoint.port);
    impl->profile = profile_for_application_protocol(impl->config.application_protocol);
    impl->run_endpoint = {impl->config.authority, impl->config.path,
                          impl->config.allowed_origins,
                          impl->config.application_protocol,
                          impl->config.require_origin};
    impl->route.path = impl->config.path.c_str();
    impl->route.path_length = impl->config.path.size();
    impl->route.path_callback = Impl::route_callback;
    impl->route.path_app_ctx = impl.get();
    impl->route.connect_protocol = "webtransport-h3";
    impl->route.connect_protocol_length = std::strlen("webtransport-h3");
    impl->route.connect_error_status = 400;
    impl->parameters.path_table = &impl->route;
    impl->parameters.path_table_nb = 1;
    impl->quic = picoquic_create(
        1, impl->config.quic.certificate_path.c_str(),
        impl->config.quic.private_key_path.c_str(), nullptr, "h3",
        Impl::callback, &impl->parameters, nullptr, nullptr, nullptr,
        picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    if (impl->quic == nullptr)
        return {nullptr, NativeQuicListenerError::CryptoInitializationFailed};
    picoquic_set_cookie_mode(impl->quic, 1);
    const std::array parameters{
        std::pair{picoquic_tp_idle_timeout,
                  static_cast<std::uint64_t>(impl->config.quic.idle_timeout.count())},
        std::pair{picoquic_tp_initial_max_data,
                  impl->config.quic.initial_max_data},
        std::pair{picoquic_tp_initial_max_stream_data_bidi_local,
                  impl->config.quic.initial_max_stream_data_bidi_local},
        std::pair{picoquic_tp_initial_max_stream_data_bidi_remote,
                  impl->config.quic.initial_max_stream_data_bidi_remote},
        std::pair{picoquic_tp_initial_max_stream_data_uni,
                  impl->config.quic.initial_max_stream_data_uni},
        std::pair{picoquic_tp_initial_max_streams_bidi,
                  impl->config.quic.initial_max_streams_bidi},
        std::pair{picoquic_tp_initial_max_streams_uni,
                  impl->config.quic.initial_max_streams_uni},
        std::pair{picoquic_tp_max_datagram_frame_size,
                  static_cast<std::uint64_t>(impl->config.quic.max_udp_payload)}};
    for (const auto& [type, value] : parameters)
        if (picoquic_set_default_tp_value(impl->quic,
                static_cast<std::uint64_t>(type), value) != 0)
            return {nullptr, NativeQuicListenerError::TransportConfigurationFailed};
    picowt_set_default_transport_parameters(impl->quic);
    return {std::unique_ptr<WebTransportListener>(
                new WebTransportListener(std::move(impl))), std::nullopt};
}

const BoundEndpoint& WebTransportListener::bound_endpoint() const noexcept {
    return impl_->endpoint;
}
OpenResult WebTransportListener::open_bidi() {
    return impl_->session ? impl_->session->open_bidi() : OpenResult{TransportStatus::InvalidState, 0};
}
OpenResult WebTransportListener::open_uni() {
    return impl_->session ? impl_->session->open_uni() : OpenResult{TransportStatus::InvalidState, 0};
}
OperationResult WebTransportListener::write(StreamId id, std::span<const std::byte> data,
                                             bool fin) {
    if (!impl_->session)
        return {TransportStatus::InvalidState, 0, std::nullopt};
    const auto result = impl_->session->write(id, data, fin);
    if (result.status == TransportStatus::Success ||
        result.status == TransportStatus::Partial)
        impl_->pump_send();
    return result;
}
OperationResult WebTransportListener::reset(StreamId id, std::uint64_t error) {
    return impl_->session ? impl_->session->reset(id, error) :
                            OperationResult{TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult WebTransportListener::stop_sending(StreamId id, std::uint64_t error) {
    return impl_->session ? impl_->session->stop_sending(id, error) :
                            OperationResult{TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult WebTransportListener::send_datagram(std::span<const std::byte> data) {
    return impl_->session ? impl_->session->send_datagram(data) :
                            OperationResult{TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult WebTransportListener::grant_peer_streams(bool bidirectional,
                                                         std::uint64_t additional) {
    return impl_->session ? impl_->session->grant_peer_streams(bidirectional, additional) :
                            OperationResult{TransportStatus::InvalidState, 0, std::nullopt};
}
OperationResult WebTransportListener::set_inbound_drop(bool enabled) {
    impl_->drop_inbound = enabled;
    return {TransportStatus::Success, 0, std::nullopt};
}
OperationResult WebTransportListener::close(std::uint64_t error,
                                             std::span<const std::byte> reason) {
    return impl_->session ? impl_->session->close(error, reason) :
                            OperationResult{TransportStatus::InvalidState, 0, std::nullopt};
}
std::vector<TransportEvent> WebTransportListener::poll(std::size_t max_events) {
    impl_->pump_receive();
    impl_->pump_send();
    return impl_->session ? impl_->session->poll(max_events) :
                            std::vector<TransportEvent>{};
}

}  // namespace moq::interop::transport
