#include "lite_ref_session.h"

#include "transport/webtransport_session.h"

#include <h3zero.h>
#include <h3zero_common.h>
#include <pico_webtransport.h>
#include <picoquic.h>
#include <picoquic_internal.h>
#include <picoquic_utils.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <variant>

namespace moq::interop::lite_ref {
namespace {

using test::lite::ConformingLitePublisher;
using test::lite::ConformingLitePublisherConfig;
using test::lite::ScriptedLitePeer;

constexpr std::uint64_t kScannedStreams = 64;

// A Group stream starts with the stream type 0x0 (draft 7.x); the publisher's outbound event for a new uni stream
// carrying it is one group sent.
bool starts_group(const transport::StreamDataEvent& event) {
    return !event.data.empty() && event.data.front() == std::byte{0x0};
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Native QUIC
// ---------------------------------------------------------------------------------------------------------------

QuicDriver::QuicDriver(std::string host, std::uint16_t port, ConformingLitePublisherConfig config,
                       std::string_view alpn)
    : publisher_(std::move(config)), mirror_(publisher_.reaction()) {
    defect_ = std::string(defect_name(publisher_.config().defect));
    Client::Config client;
    client.host = std::move(host);
    client.port = port;
    client.alpn.assign(reinterpret_cast<const std::byte*>(alpn.data()),
                       reinterpret_cast<const std::byte*>(alpn.data()) + alpn.size());
    // moq-lite needs no QUIC DATAGRAM (decision (c)): the lite listener accepts a client without it. A publisher that
    // sends datagrams (config.datagrams) offers the extension.
    client.enable_datagrams = publisher_.config().datagrams;
    client.keep_alive = true;
    client_ = Client::create(client);
}

bool QuicDriver::step() {
    if (!client_ || !client_->pump()) return false;
    if (!client_->established()) return true;
    mirror_runner();
    for (auto& event : mirror_.poll(1024)) outbound_.push_back(std::move(event));
    flush();
    if (const auto close = client_->peer_close()) {
        close_reason_ = std::string(reinterpret_cast<const char*>(close->reason.data()), close->reason.size());
        return false;
    }
    return true;
}

void QuicDriver::mirror_runner() {
    // Runner-opened streams: bidi 1, 5, 9, ... and uni 3, 7, 11, ...
    for (std::uint64_t index = 0; index < kScannedStreams; ++index) {
        for (const std::uint64_t base : {1u, 3u}) {
            const auto id = base + 4 * index;
            const auto observed = client_->stream(id);
            if (!observed) continue;
            auto& done = forwarded_[id];
            const bool fin = observed->fin && !fin_forwarded_.contains(id);
            if (observed->data.size() > done || fin) {
                const std::span<const std::byte> fresh(observed->data.data() + done, observed->data.size() - done);
                (void)mirror_.write(id, fresh, observed->fin);
                done = observed->data.size();
                if (observed->fin) fin_forwarded_.insert(id);
            }
            if (observed->reset_error && !reset_forwarded_.contains(id)) {
                reset_forwarded_.insert(id);
                (void)mirror_.reset(id, *observed->reset_error);
            }
        }
    }
    // STOP_SENDING from the runner: on the publisher's streams and on the runner's bidi streams.
    for (std::uint64_t index = 0; index < kScannedStreams; ++index) {
        for (const std::uint64_t base : {0u, 1u, 2u}) {
            const auto id = base + 4 * index;
            if (stop_forwarded_.contains(id)) continue;
            if (const auto code = client_->stop_sending_error(id)) {
                stop_forwarded_.insert(id);
                (void)mirror_.stop_sending(id, *code);
            }
        }
    }
}

void QuicDriver::flush() {
    while (!outbound_.empty()) {
        const auto& event = outbound_.front();
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            const auto sent = client_->try_send_stream(data->stream_id, data->data, data->fin);
            if (sent.status == transport::test::ClientStreamSendStatus::WouldBlock) return;
            if (starts_group(*data) && counted_groups_.insert(data->stream_id).second) ++groups_sent_;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            (void)client_->reset_stream(reset->stream_id, reset->application_error.value_or(0));
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) {
            (void)client_->stop_stream(stop->stream_id, stop->application_error.value_or(0));
        } else if (const auto* datagram = std::get_if<transport::DatagramEvent>(&event)) {
            if (client_->send_datagram(datagram->data)) ++datagrams_sent_;
        } else if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
            (void)client_->close(close->error_code, close->reason);
        }
        outbound_.pop_front();
    }
}

RefSession::Summary QuicDriver::summary() const {
    return {"native_quic", defect_, groups_sent_, datagrams_sent_, close_reason_};
}

// ---------------------------------------------------------------------------------------------------------------
// WebTransport
// ---------------------------------------------------------------------------------------------------------------

namespace {

class WtDriver final : public RefSession {
public:
    WtDriver(const std::string& host, std::uint16_t port, std::string target, ConformingLitePublisherConfig config)
        : port_(port), target_(std::move(target)), publisher_(std::move(config)), mirror_(publisher_.reaction()) {
        defect_ = std::string(defect_name(publisher_.config().defect));
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) return;
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
            return;
        socklen_t size = sizeof(local);
        (void)::getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &size);
        local_ = local;
        remote_.sin_family = AF_INET;
        remote_.sin_port = htons(port);
        authority_ = host + ":" + std::to_string(port);
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) return;
        remote_.sin_addr = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr;
        ::freeaddrinfo(found);
        quic_ = picoquic_create(1, nullptr, nullptr, nullptr, "h3", nullptr, nullptr, nullptr, nullptr, nullptr,
                                picoquic_current_time(), nullptr, nullptr, nullptr, 0);
        if (!quic_) return;
        picoquic_set_null_verifier(quic_);
        // QUIC datagrams carry the HTTP Datagram; picowt sets the WebTransport transport parameters itself.
        ok_ = picowt_prepare_client_cnx(quic_, reinterpret_cast<sockaddr*>(&remote_), &cnx_, &h3_, &control_,
                                        picoquic_current_time(), host.c_str()) == 0 &&
              h3zero_declare_stream_prefix(h3_, control_->stream_id, callback, this) == 0 &&
              picoquic_start_client_cnx(cnx_) == 0;
        if (ok_) picoquic_enable_keep_alive(cnx_, 0);
    }

    ~WtDriver() override {
        session_.reset();
        if (h3_ != nullptr) h3zero_callback_delete_context(cnx_, h3_);
        if (quic_ != nullptr) picoquic_free(quic_);
        if (fd_ >= 0) ::close(fd_);
    }
    WtDriver(const WtDriver&) = delete;
    WtDriver& operator=(const WtDriver&) = delete;

    [[nodiscard]] bool valid() const { return ok_; }
    [[nodiscard]] bool established() const override { return session_ != nullptr; }
    [[nodiscard]] Summary summary() const override {
        return {"webtransport", defect_, groups_sent_, datagrams_sent_, close_reason_};
    }

    bool step() override {
        if (!ok_ || ended_) return false;
        egress();
        ingress();
        if (!connect_sent_ && h3_->settings.settings_received) {
            if (!send_connect()) return false;
        }
        if (session_) {
            drain_runner();
            for (auto& event : mirror_.poll(1024)) outbound_.push_back(std::move(event));
            flush();
        }
        egress();
        return !ended_;
    }

private:
    static constexpr std::size_t kPacket = 2048;

    void egress() {
        std::array<std::uint8_t, kPacket> buffer{};
        for (int packet = 0; packet < 16; ++packet) {
            sockaddr_storage destination{};
            sockaddr_storage source{};
            picoquic_connection_id_t log_id{};
            picoquic_cnx_t* last = nullptr;
            std::size_t length = 0;
            int interface_index = 0;
            if (picoquic_prepare_next_packet(quic_, picoquic_current_time(), buffer.data(), buffer.size(), &length,
                                             &destination, &source, &interface_index, &log_id, &last) != 0 ||
                length == 0)
                break;
            (void)::sendto(fd_, buffer.data(), length, 0, reinterpret_cast<sockaddr*>(&destination), sizeof(remote_));
        }
    }

    void ingress() {
        std::array<std::uint8_t, kPacket> buffer{};
        for (int packet = 0; packet < 16; ++packet) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const auto length =
                ::recvfrom(fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (length < 0) break;
            auto received_at = local_;
            (void)picoquic_incoming_packet(quic_, buffer.data(), static_cast<std::size_t>(length),
                                           reinterpret_cast<sockaddr*>(&peer),
                                           reinterpret_cast<sockaddr*>(&received_at), 0, 0, picoquic_current_time());
        }
        if (cnx_ != nullptr && picoquic_get_cnx_state(cnx_) == picoquic_state_disconnected) ended_ = true;
    }

    bool send_connect() {
        const std::string offered = "\"" + std::string(scenarios::kLiteAlpn) + "\"";
        std::array<std::uint8_t, 512> qpack{};
        auto* end = h3zero_create_connect_header_frame(
            qpack.data(), qpack.data() + qpack.size(), authority_.c_str(),
            reinterpret_cast<const std::uint8_t*>(target_.data()), target_.size(), "webtransport-h3", nullptr, nullptr,
            offered.c_str());
        if (end == nullptr) return false;
        std::array<std::uint8_t, 1024> frame{};
        auto* cursor = picoquic_frames_varint_encode(frame.data(), frame.data() + frame.size(), h3zero_frame_header);
        cursor = picoquic_frames_varint_encode(cursor, frame.data() + frame.size(),
                                               static_cast<std::uint64_t>(end - qpack.data()));
        if (cursor == nullptr) return false;
        std::memcpy(cursor, qpack.data(), static_cast<std::size_t>(end - qpack.data()));
        cursor += end - qpack.data();
        control_->path_callback = callback;
        control_->path_callback_ctx = this;
        control_->is_open = 1;
        control_->ps.stream_state.is_upgrade_requested = 1;
        if (picoquic_add_to_stream_with_ctx(cnx_, control_->stream_id, frame.data(),
                                            static_cast<std::size_t>(cursor - frame.data()), 0, control_) != 0)
            return false;
        connect_sent_ = true;
        return true;
    }

    void open_session() {
        const auto* peer = picoquic_get_transport_parameters(cnx_, 0);
        const std::size_t peer_datagram = peer != nullptr ? static_cast<std::size_t>(peer->max_datagram_frame_size) : 0;
        const std::size_t usable = peer_datagram > 8 ? std::min<std::size_t>(peer_datagram, 1300) - 8 : 0;
        session_ = std::make_unique<transport::WebTransportSession>(
            control_->stream_id,
            transport::WebTransportSessionLimits{4096, 1u << 20, usable, 1342, 1u << 20, true}, cnx_, h3_, control_,
            callback, this);
        session_->establish({}, {}, {});
    }

    // Runner -> mirror: every stream event the session saw, under the stream ids the mirror knows.
    void drain_runner() {
        for (auto& event : session_->poll(1024)) {
            if (auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
                (void)mirror_.write(mirror_id(data->stream_id), data->data, data->fin);
            } else if (auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
                (void)mirror_.reset(mirror_id(reset->stream_id), reset->application_error.value_or(0));
            } else if (auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) {
                (void)mirror_.stop_sending(mirror_id(stop->stream_id), stop->application_error.value_or(0));
            } else if (auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
                close_reason_ = std::string(reinterpret_cast<const char*>(close->reason.data()), close->reason.size());
                ended_ = true;
            }
        }
    }

    // The mirror's id for a real stream (publisher-opened streams were mapped when the publisher opened them).
    transport::StreamId mirror_id(transport::StreamId real) const {
        const auto found = to_mirror_.find(real);
        return found == to_mirror_.end() ? real : found->second;
    }

    // Mirror -> network: the publisher's streams are opened on demand (the mirror numbers them as a QUIC client would).
    void flush() {
        while (!outbound_.empty()) {
            const auto& event = outbound_.front();
            if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
                const auto real = real_id(data->stream_id);
                if (!real) {
                    outbound_.pop_front();
                    continue;
                }
                const auto sent = session_->write(*real, data->data, data->fin);
                if (sent.status == transport::TransportStatus::WouldBlock) return;
                if (starts_group(*data) && counted_groups_.insert(data->stream_id).second) ++groups_sent_;
            } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
                if (const auto real = real_id(reset->stream_id))
                    (void)session_->reset(*real, reset->application_error.value_or(0));
            } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) {
                if (const auto real = real_id(stop->stream_id))
                    (void)session_->stop_sending(*real, stop->application_error.value_or(0));
            } else if (const auto* datagram = std::get_if<transport::DatagramEvent>(&event)) {
                if (session_->send_datagram(datagram->data).status == transport::TransportStatus::Success)
                    ++datagrams_sent_;
            } else if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
                (void)session_->close(close->error_code, close->reason);
                ended_ = true;
            }
            outbound_.pop_front();
        }
    }

    std::optional<transport::StreamId> real_id(transport::StreamId mirror) {
        // Runner-opened streams (server-initiated: low bits 01 bidi, 11 uni) keep their ids.
        if ((mirror & 1u) == 1u) return mirror;
        const auto found = to_real_.find(mirror);
        if (found != to_real_.end()) return found->second;
        const auto opened = (mirror & 2u) == 0u ? session_->open_bidi() : session_->open_uni();
        if (opened.status != transport::TransportStatus::Success) return std::nullopt;
        to_real_[mirror] = opened.stream_id;
        to_mirror_[opened.stream_id] = mirror;
        return opened.stream_id;
    }

    static int callback(picoquic_cnx_t* cnx, std::uint8_t* bytes, std::size_t length, picohttp_call_back_event_t event,
                        h3zero_stream_ctx_t* stream_ctx, void* context) {
        auto* self = static_cast<WtDriver*>(context);
        if (self == nullptr) return -1;
        if (event == picohttp_callback_connect_accepted) {
            self->accepted_ = true;
            // Streams and datagrams may follow in the same flight: the session must exist before they are delivered.
            if (!self->session_) self->open_session();
            return 0;
        }
        if (event == picohttp_callback_connect_refused) {
            self->ended_ = true;
            return 0;
        }
        if (self->session_ == nullptr || stream_ctx == nullptr) return 0;
        auto& session = *self->session_;
        if (event == picohttp_callback_post_data || event == picohttp_callback_post_fin) {
            if (stream_ctx->stream_id == session.connect_stream_id()) {
                if (event == picohttp_callback_post_fin) {
                    session.ingest_peer_close(0, {});
                    self->ended_ = true;
                }
                return 0;
            }
            const auto* payload = reinterpret_cast<const std::byte*>(bytes);
            return session.ingest_stream(stream_ctx->stream_id, stream_ctx->ps.stream_state.control_stream_id,
                                         {payload, length}, event == picohttp_callback_post_fin)
                       ? 0
                       : -1;
        }
        if (event == picohttp_callback_post_datagram) {
            const auto* payload = reinterpret_cast<const std::byte*>(bytes);
            (void)session.ingest_datagram(stream_ctx->stream_id, {payload, length});
        } else if (event == picohttp_callback_reset) {
            session.ingest_reset(stream_ctx->stream_id, picoquic_get_remote_stream_error(cnx, stream_ctx->stream_id));
        } else if (event == picohttp_callback_stop_sending) {
            if (const auto* stream = picoquic_find_stream(cnx, stream_ctx->stream_id))
                session.ingest_stop_sending(stream_ctx->stream_id, stream->remote_stop_error);
        } else if (event == picohttp_callback_deregister) {
            session.detach();
            self->ended_ = true;
        }
        return 0;
    }

    std::uint16_t port_;
    std::string target_;
    std::string authority_;
    ConformingLitePublisher publisher_;
    ScriptedLitePeer mirror_;
    std::unique_ptr<transport::WebTransportSession> session_;
    std::deque<transport::TransportEvent> outbound_;
    std::map<transport::StreamId, transport::StreamId> to_real_, to_mirror_;
    std::set<transport::StreamId> counted_groups_;
    std::uint64_t groups_sent_{0}, datagrams_sent_{0};
    std::string defect_, close_reason_;
    int fd_{-1};
    sockaddr_in local_{};
    sockaddr_in remote_{};
    picoquic_quic_t* quic_{nullptr};
    picoquic_cnx_t* cnx_{nullptr};
    h3zero_callback_ctx_t* h3_{nullptr};
    h3zero_stream_ctx_t* control_{nullptr};
    bool ok_{false}, connect_sent_{false}, accepted_{false}, ended_{false};
};

// scheme://host:port/path -> host, port, target (path?query).
bool split_dial_url(const std::string& url, std::string& host, std::uint16_t& port, std::string& target) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    const auto authority_start = scheme + 3;
    const auto slash = url.find('/', authority_start);
    if (slash == std::string::npos) return false;
    const auto authority = url.substr(authority_start, slash - authority_start);
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    host = authority.substr(0, colon);
    const auto number = std::stoul(authority.substr(colon + 1));
    if (number == 0 || number > 65535) return false;
    port = static_cast<std::uint16_t>(number);
    target = url.substr(slash);
    return true;
}

}  // namespace

std::unique_ptr<RefSession> RefSession::dial(const Options& options, std::string& error) {
    std::string host;
    std::uint16_t port = 0;
    std::string target;
    try {
        if (!split_dial_url(options.connect, host, port, target)) {
            error = "cannot read host, port and path from '" + options.connect + "'";
            return nullptr;
        }
    } catch (const std::exception&) {
        error = "bad port in '" + options.connect + "'";
        return nullptr;
    }
    if (options.publisher.binding == scenarios::LiteBinding::WebTransport) {
        auto driver = std::make_unique<WtDriver>(host, port, target, options.publisher);
        if (!driver->valid()) {
            error = "cannot start the WebTransport connection to " + options.connect;
            return nullptr;
        }
        return driver;
    }
    auto driver = std::make_unique<QuicDriver>(host, port, options.publisher);
    if (!driver->valid()) {
        error = "cannot start the QUIC connection to " + options.connect;
        return nullptr;
    }
    return driver;
}

}  // namespace moq::interop::lite_ref
