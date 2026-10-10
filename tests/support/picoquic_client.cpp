#include "support/picoquic_client.h"

#include <picoquic_internal.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moq::interop::transport::test {
namespace {
constexpr std::size_t kPacketSize = 1350;
constexpr std::size_t kMaximumPacketsPerPump = 64;
constexpr std::size_t kMaximumStreamBytes = 1u << 20;
constexpr std::size_t kMaximumStreams = 256;
constexpr std::uint64_t kMaximumQuicInteger = (std::uint64_t{1} << 62) - 1;

struct Socket {
    int fd = -1;
    sockaddr_in local{};
};
std::optional<Socket> bind_socket() {
    Socket result;
    result.local.sin_family = AF_INET;
    result.local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    result.fd = ::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (result.fd < 0) return std::nullopt;
    const auto flags = ::fcntl(result.fd,F_GETFL,0);
    socklen_t length = sizeof(result.local);
    if (flags < 0 || ::fcntl(result.fd,F_SETFL,flags | O_NONBLOCK) != 0 ||
        ::bind(result.fd,reinterpret_cast<const sockaddr*>(&result.local),sizeof(result.local)) != 0 ||
        ::getsockname(result.fd,reinterpret_cast<sockaddr*>(&result.local),&length) != 0) {
        ::close(result.fd);
        return std::nullopt;
    }
    return result;
}
bool same_address(const sockaddr_storage& address, const sockaddr_in& expected) {
    if (address.ss_family != AF_INET) return false;
    const auto* actual = reinterpret_cast<const sockaddr_in*>(&address);
    return actual->sin_port == expected.sin_port && actual->sin_addr.s_addr == expected.sin_addr.s_addr;
}
}  // namespace

struct PicoquicTestClient::Impl {
    Config config;
    picoquic_quic_t* quic = nullptr;
    picoquic_cnx_t* connection = nullptr;
    std::vector<Socket> sockets;
    sockaddr_in peer{};
    std::string alpn;
    std::string close_reason;
    std::array<std::uint8_t,65535> receive{};
    std::array<std::uint8_t,kPacketSize> send{};
    std::unordered_map<std::uint64_t,ClientStreamObservation> streams;
    std::unordered_map<std::uint64_t,std::uint64_t> stopped;
    std::unordered_set<std::uint64_t> finished;
    std::unordered_set<std::uint64_t> outgoing;
    std::unordered_set<std::uint64_t> retired_ids;
    std::vector<std::vector<std::byte>> datagrams;
    std::optional<ClientCloseObservation> peer_close;
    struct PendingPacket {
        int fd;
        sockaddr_storage destination;
        std::vector<std::uint8_t> bytes;
    };
    std::optional<PendingPacket> pending;
    bool ready = false;
    bool closed = false;
    bool local_close = false;
    bool failed = false;

    ~Impl() {
        if (quic) picoquic_free(quic);
        for (const auto& socket : sockets) if (socket.fd >= 0) ::close(socket.fd);
    }

    void record_peer_close(picoquic_cnx_t* cnx, bool application) {
        if (local_close || peer_close) return;
        ClientCloseObservation observation;
        observation.application = application;
        observation.error_code = application ? picoquic_get_application_error(cnx)
                                             : picoquic_get_remote_error(cnx);
        // Picoquic retains close reasons as C strings, without wire length.
        if (cnx->remote_error_reason) {
            const auto length = std::strlen(cnx->remote_error_reason);
            if (length > kPacketSize) { failed = true; return; }
            const auto* reason = reinterpret_cast<const std::byte*>(cnx->remote_error_reason);
            observation.reason.assign(reason,reason + length);
        }
        peer_close = std::move(observation);
    }

    static int callback(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* data,
                        std::size_t length, picoquic_call_back_event_t event, void* context, void*) {
        if (!context) return -1;
        auto& self = *static_cast<Impl*>(context);
        if (event == picoquic_callback_ready) {
            self.ready = true;
        } else if (event == picoquic_callback_stream_data || event == picoquic_callback_stream_fin) {
            if ((!self.streams.contains(stream_id) && self.streams.size() == kMaximumStreams) ||
                length > kMaximumStreamBytes) {
                self.failed = true;
                return -1;
            }
            auto& observation = self.streams[stream_id];
            if (length > kMaximumStreamBytes - observation.data.size()) {
                self.failed = true;
                return -1;
            }
            if (length) {
                observation.data.insert(observation.data.end(),reinterpret_cast<const std::byte*>(data),
                    reinterpret_cast<const std::byte*>(data) + length);
                observation.chunk_sizes.push_back(length);
            }
            if (event == picoquic_callback_stream_fin) {
                observation.fin = true;
                ++observation.fin_count;
            }
        } else if (event == picoquic_callback_stream_reset) {
            if (!self.streams.contains(stream_id) && self.streams.size() == kMaximumStreams) {
                self.failed = true;
                return -1;
            }
            self.streams[stream_id].reset_error = picoquic_get_remote_stream_error(cnx,stream_id);
        } else if (event == picoquic_callback_stop_sending) {
            const auto* stream = picoquic_find_stream(cnx,stream_id);
            if (!stream || self.stopped.size() >= kMaximumStreams) {
                self.failed = true;
                return -1;
            }
            self.stopped[stream_id] = stream->remote_stop_error;
        } else if (event == picoquic_callback_datagram) {
            if (self.datagrams.size() >= self.config.datagram_queue || length > kPacketSize) {
                self.failed = true;
                return -1;
            }
            std::vector<std::byte> payload(length);
            if (length) std::memcpy(payload.data(),data,length);
            self.datagrams.push_back(std::move(payload));
        } else if (event == picoquic_callback_application_close || event == picoquic_callback_close ||
                   event == picoquic_callback_stateless_reset) {
            // Generic close callbacks also report local idle disconnection.
            // Record only actual peer close evidence, never a timer as a frame.
            if (event != picoquic_callback_close || cnx->remote_error != 0 || cnx->remote_error_reason)
                self.record_peer_close(cnx,event == picoquic_callback_application_close);
            self.closed = true;
        }
        return 0;
    }

    void remember_retired_ids() {
        if (!connection) return;
        for (auto* stash = connection->first_remote_cnxid_stash; stash; stash = stash->next_stash)
            for (auto* id = stash->cnxid_stash_first; id; id = id->next)
                if (id->retire_sent || id->retire_acked) retired_ids.insert(id->sequence);
    }
    bool flush() {
        for (std::size_t count = 0; count < kMaximumPacketsPerPump; ++count) {
            if (!pending) {
                sockaddr_storage destination{},source{};
                picoquic_connection_id_t log_id{};
                picoquic_cnx_t* last_connection = nullptr;
                std::size_t length = 0;
                int interface_index = 0;
                if (picoquic_prepare_next_packet(quic,picoquic_current_time(),send.data(),send.size(),
                    &length,&destination,&source,&interface_index,&log_id,&last_connection) != 0) return false;
                remember_retired_ids();
                if (!length) return !failed;
                int fd = -1;
                for (const auto& socket : sockets)
                    if (same_address(source,socket.local)) { fd = socket.fd; break; }
                if (fd < 0) return false;
                pending = PendingPacket{fd,destination,{send.begin(),send.begin() + static_cast<std::ptrdiff_t>(length)}};
            }
            ssize_t sent;
            do {
                sent = ::sendto(pending->fd,pending->bytes.data(),pending->bytes.size(),0,
                    reinterpret_cast<const sockaddr*>(&pending->destination),sizeof(sockaddr_in));
            } while (sent < 0 && errno == EINTR);
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return !failed;
            if (sent < 0 || static_cast<std::size_t>(sent) != pending->bytes.size()) return false;
            pending.reset();
        }
        return !failed;
    }
    std::size_t queued_stream_bytes() const {
        std::size_t total = 0;
        for (const auto id : outgoing) {
            const auto* stream = picoquic_find_stream(connection,id);
            if (!stream) continue;
            for (auto* node = stream->send_queue; node; node = node->next_stream_data) {
                const auto remaining = node->length - node->offset;
                if (remaining > kMaximumStreamBytes - total) return kMaximumStreamBytes;
                total += remaining;
            }
        }
        return total;
    }
};

PicoquicTestClient::PicoquicTestClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PicoquicTestClient::~PicoquicTestClient() = default;

std::unique_ptr<PicoquicTestClient> PicoquicTestClient::create(const Config& input) {
    constexpr auto maximum_stream_count = std::uint64_t{1} << 60;
    if (!input.port || input.alpn.empty() || input.alpn.size() > 255 || !input.datagram_queue ||
        input.initial_max_streams_bidi > maximum_stream_count ||
        input.initial_max_streams_uni > maximum_stream_count ||
        std::ranges::find(input.alpn,std::byte{0}) != input.alpn.end()) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->config = input;
    impl->alpn.assign(reinterpret_cast<const char*>(input.alpn.data()),input.alpn.size());
    impl->peer.sin_family = AF_INET;
    impl->peer.sin_port = htons(input.port);
    if (::inet_pton(AF_INET,input.host.c_str(),&impl->peer.sin_addr) != 1) return nullptr;
    const auto socket = bind_socket();
    if (!socket) return nullptr;
    impl->sockets.push_back(*socket);
    impl->quic = picoquic_create(1,nullptr,nullptr,nullptr,impl->alpn.c_str(),Impl::callback,impl.get(),
        nullptr,nullptr,nullptr,picoquic_current_time(),nullptr,nullptr,nullptr,0);
    if (!impl->quic) return nullptr;
    picoquic_set_null_verifier(impl->quic);
    const std::array parameters{
        std::pair{picoquic_tp_idle_timeout,std::uint64_t{5000}},
        std::pair{picoquic_tp_max_packet_size,std::uint64_t{kPacketSize}},
        std::pair{picoquic_tp_initial_max_data,std::uint64_t{1u << 20}},
        std::pair{picoquic_tp_initial_max_stream_data_bidi_local,std::uint64_t{1u << 18}},
        std::pair{picoquic_tp_initial_max_stream_data_bidi_remote,std::uint64_t{1u << 18}},
        std::pair{picoquic_tp_initial_max_stream_data_uni,std::uint64_t{1u << 18}},
        std::pair{picoquic_tp_initial_max_streams_bidi,input.initial_max_streams_bidi},
        std::pair{picoquic_tp_initial_max_streams_uni,input.initial_max_streams_uni},
        std::pair{picoquic_tp_active_connection_id_limit,std::uint64_t{8}},
        std::pair{picoquic_tp_max_datagram_frame_size,input.enable_datagrams ? std::uint64_t{kPacketSize} : 0},
    };
    for (const auto& [type,value] : parameters)
        if (picoquic_set_default_tp_value(impl->quic,static_cast<std::uint64_t>(type),value) != 0) return nullptr;
    impl->connection = picoquic_create_cnx(impl->quic,picoquic_null_connection_id,picoquic_null_connection_id,
        reinterpret_cast<const sockaddr*>(&impl->peer),picoquic_current_time(),0,"localhost",impl->alpn.c_str(),1);
    if (!impl->connection || picoquic_set_local_addr(impl->connection,
        reinterpret_cast<sockaddr*>(&impl->sockets.front().local)) != 0 ||
        picoquic_start_client_cnx(impl->connection) != 0) return nullptr;
    if (input.keep_alive) picoquic_enable_keep_alive(impl->connection, 0);
    if (!impl->flush()) return nullptr;
    return std::unique_ptr<PicoquicTestClient>(new PicoquicTestClient(std::move(impl)));
}

bool PicoquicTestClient::pump() {
    if (!impl_ || impl_->failed) return false;
    for (const auto& socket : impl_->sockets) {
        for (std::size_t count = 0; count < kMaximumPacketsPerPump; ++count) {
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            ssize_t received;
            do {
                received = ::recvfrom(socket.fd,impl_->receive.data(),impl_->receive.size(),0,
                    reinterpret_cast<sockaddr*>(&from),&length);
            } while (received < 0 && errno == EINTR);
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (received < 0) return false;
            auto local = socket.local;
            // Packet rejection is not a harness failure: picoquic can discard
            // unrelated or stale UDP packets while the connection stays usable.
            picoquic_incoming_packet(impl_->quic,impl_->receive.data(),static_cast<std::size_t>(received),
                reinterpret_cast<sockaddr*>(&from),reinterpret_cast<sockaddr*>(&local),0,0,picoquic_current_time());
            // An established transport CLOSE (including code0/empty reason)
            // enters closing_received without the application-close callback.
            if (!impl_->local_close && impl_->connection->cnx_state == picoquic_state_closing_received) {
                impl_->record_peer_close(impl_->connection,false);
                impl_->closed = true;
            }
            impl_->remember_retired_ids();
            if (impl_->failed) return false;
        }
    }
    return impl_->flush();
}
bool PicoquicTestClient::established() const { return impl_ && impl_->ready; }

bool PicoquicTestClient::send_stream(std::uint64_t stream_id, std::span<const std::byte> data, bool fin) {
    return try_send_stream(stream_id,data,fin).status == ClientStreamSendStatus::Success;
}
ClientStreamSendResult PicoquicTestClient::try_send_stream(
    std::uint64_t stream_id, std::span<const std::byte> data, bool fin) {
    if (!impl_ || !impl_->connection || impl_->closed || impl_->local_close || impl_->failed ||
        stream_id > kMaximumQuicInteger || (stream_id & 3u) == 3u) return {};
    if (const auto stop = impl_->stopped.find(stream_id); stop != impl_->stopped.end())
        return {ClientStreamSendStatus::PeerStopped,0,stop->second};
    if (impl_->finished.contains(stream_id)) return {};
    if (data.size() > kMaximumStreamBytes - impl_->queued_stream_bytes())
        return {ClientStreamSendStatus::WouldBlock,0,0};
    if (!impl_->outgoing.contains(stream_id) && impl_->outgoing.size() == kMaximumStreams) return {};
    if ((stream_id & 1u) == 0) {
        const auto count = (stream_id & 2u) ? impl_->connection->max_streams_unidir_remote
                                            : impl_->connection->max_streams_bidir_remote;
        if ((stream_id >> 2u) >= count) return {ClientStreamSendStatus::WouldBlock,0,0};
    }
    static constexpr std::uint8_t empty = 0;
    const auto queued = picoquic_add_to_stream(impl_->connection,stream_id,
        data.empty() ? &empty : reinterpret_cast<const std::uint8_t*>(data.data()),data.size(),fin ? 1 : 0);
    if (queued != 0) {
        if (queued == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED) {
            const auto reset = impl_->streams.find(stream_id);
            if (reset != impl_->streams.end() && reset->second.reset_error)
                return {ClientStreamSendStatus::PeerReset,0,*reset->second.reset_error};
        }
        return {};
    }
    impl_->outgoing.insert(stream_id);
    if (fin) impl_->finished.insert(stream_id);
    if (!impl_->flush()) return {ClientStreamSendStatus::Error,data.size(),0};
    return {ClientStreamSendStatus::Success,data.size(),0};
}
bool PicoquicTestClient::send_datagram(std::span<const std::byte> data) {
    if (!impl_ || !impl_->connection || impl_->closed || impl_->local_close || !impl_->config.enable_datagrams)
        return false;
    const auto* peer = picoquic_get_transport_parameters(impl_->connection,0);
    if (!peer || !peer->max_datagram_frame_size || data.size() > kPacketSize - 50 ||
        data.size() + 3 > peer->max_datagram_frame_size) return false;
    std::size_t queued = 0;
    for (auto* frame = impl_->connection->first_datagram; frame; frame = frame->next_misc_frame) ++queued;
    if (queued >= impl_->config.datagram_queue) return false;
    static constexpr std::uint8_t empty = 0;
    return picoquic_queue_datagram_frame(impl_->connection,data.size(),
        data.empty() ? &empty : reinterpret_cast<const std::uint8_t*>(data.data())) == 0 && impl_->flush();
}
bool PicoquicTestClient::send_invalid_transport_frame() {
    if (!impl_ || !impl_->connection || !impl_->ready || impl_->closed || impl_->local_close || impl_->failed)
        return false;
    // Pinned frames.c rejects unknown0x21; 0x1f is supported IMMEDIATE_ACK.
    static constexpr std::array<std::uint8_t,1> frame{0x21};
    return picoquic_queue_misc_frame(impl_->connection,frame.data(),frame.size(),0,
        picoquic_packet_context_application) == 0 && impl_->flush();
}
bool PicoquicTestClient::send_transport_close_frame() {
    if (!impl_ || !impl_->connection || !impl_->ready || impl_->closed || impl_->local_close || impl_->failed)
        return false;
    // Transport CLOSE with code zero, offending frame zero, and empty reason.
    static constexpr std::array<std::uint8_t,4> frame{0x1c,0,0,0};
    return picoquic_queue_misc_frame(impl_->connection,frame.data(),frame.size(),0,
        picoquic_packet_context_application) == 0 && impl_->flush();
}
bool PicoquicTestClient::reset_stream(std::uint64_t stream_id, std::uint64_t application_error) {
    if (!impl_ || !impl_->connection || impl_->closed || impl_->local_close ||
        application_error > kMaximumQuicInteger) return false;
    if (picoquic_reset_stream(impl_->connection,stream_id,application_error) != 0) return false;
    impl_->finished.insert(stream_id);
    return impl_->flush();
}
bool PicoquicTestClient::stop_stream(std::uint64_t stream_id, std::uint64_t application_error) {
    return impl_ && impl_->connection && !impl_->closed && !impl_->local_close &&
        application_error <= kMaximumQuicInteger &&
        picoquic_stop_sending(impl_->connection,stream_id,application_error) == 0 && impl_->flush();
}
std::size_t PicoquicTestClient::available_destination_ids() const {
    if (!impl_ || !impl_->connection || impl_->closed) return 0;
    std::size_t count = 0;
    for (auto* stash = impl_->connection->first_remote_cnxid_stash; stash; stash = stash->next_stash)
        for (auto* id = stash->cnxid_stash_first; id; id = id->next)
            if (!id->nb_path_references && !id->needs_removal && !id->retire_sent) ++count;
    return count;
}
std::optional<std::uint64_t> PicoquicTestClient::migrate_source() {
    if (!impl_ || !impl_->connection || !impl_->ready || impl_->closed || impl_->local_close ||
        !available_destination_ids() || impl_->sockets.size() >= 8) return std::nullopt;
    const auto socket = bind_socket();
    if (!socket) return std::nullopt;
    const auto probe = picoquic_probe_new_path(impl_->connection,reinterpret_cast<const sockaddr*>(&impl_->peer),
        reinterpret_cast<const sockaddr*>(&socket->local),picoquic_current_time());
    if (probe != 0) {
        ::close(socket->fd);
        return std::nullopt;
    }
    impl_->sockets.push_back(*socket);
    // Return the actual CID selected for the newly requested tuple. Validation
    // remains asynchronous and requires both peers to continue pumping.
    for (int path = 0; path < impl_->connection->nb_paths; ++path)
        for (auto* tuple = impl_->connection->path[path]->first_tuple; tuple; tuple = tuple->next_tuple)
            if (same_address(tuple->local_addr,socket->local) && tuple->p_remote_cnxid) {
                const auto sequence = tuple->p_remote_cnxid->sequence;
                return impl_->flush() ? std::optional{sequence} : std::nullopt;
            }
    return std::nullopt;
}
bool PicoquicTestClient::retire_destination_id(std::uint64_t sequence) {
    if (!impl_ || !impl_->connection || impl_->closed || impl_->local_close) return false;
    impl_->remember_retired_ids();
    if (impl_->retired_ids.contains(sequence)) return impl_->flush();
    auto* cnx = impl_->connection;
    auto* stash = picoquic_find_or_create_remote_cnxid_stash(cnx,0,0);
    if (!stash) return false;
    picoquic_remote_cnxid_t* selected = nullptr;
    for (auto* id = stash->cnxid_stash_first; id; id = id->next)
        if (id->sequence == sequence) { selected = id; break; }
    if (!selected) return false;
    // Renew every tuple still using this CID before queuing its retirement.
    // Never retire an ID that an active tuple would keep using.
    for (int path = 0; path < cnx->nb_paths; ++path) {
        auto* active = cnx->path[path];
        for (auto* tuple = active->first_tuple; tuple; tuple = tuple->next_tuple) {
            if (!tuple->p_remote_cnxid || tuple->p_remote_cnxid->sequence != sequence) continue;
            auto* replacement = picoquic_obtain_stashed_cnxid(cnx,0);
            if (!replacement || replacement->sequence == sequence) return false;
            ++replacement->nb_path_references;
            picoquic_dereference_stashed_cnxid_tuple(cnx,active,tuple,0);
            tuple->p_remote_cnxid = replacement;
            if (path == 0 && tuple == active->first_tuple && picoquic_register_net_secret(cnx) != 0)
                return false;
        }
    }
    if (!selected->retire_sent) {
        if (picoquic_queue_retire_connection_id_frame(cnx,0,sequence) != 0) return false;
        selected->retire_sent = 1;
    }
    selected->needs_removal = 1;
    impl_->retired_ids.insert(sequence);
    return impl_->flush();
}
std::optional<ClientStreamObservation> PicoquicTestClient::stream(std::uint64_t stream_id) const {
    if (!impl_) return std::nullopt;
    const auto found = impl_->streams.find(stream_id);
    return found == impl_->streams.end() ? std::nullopt : std::optional{found->second};
}
std::optional<std::uint64_t> PicoquicTestClient::stop_sending_error(std::uint64_t stream_id) const {
    if (!impl_) return std::nullopt;
    const auto found = impl_->stopped.find(stream_id);
    return found == impl_->stopped.end() ? std::nullopt : std::optional{found->second};
}
std::vector<std::vector<std::byte>> PicoquicTestClient::take_datagrams() {
    if (!impl_) return {};
    auto result = std::move(impl_->datagrams);
    impl_->datagrams.clear();
    return result;
}
std::optional<ClientCloseObservation> PicoquicTestClient::peer_close() const {
    return impl_ ? impl_->peer_close : std::nullopt;
}
bool PicoquicTestClient::close(std::uint64_t application_error, std::span<const std::byte> reason) {
    if (!impl_ || !impl_->connection || impl_->closed || impl_->local_close || reason.size() > 1024 ||
        application_error > kMaximumQuicInteger || std::ranges::find(reason,std::byte{0}) != reason.end()) return false;
    impl_->close_reason.clear();
    if (!reason.empty()) impl_->close_reason.assign(reinterpret_cast<const char*>(reason.data()),reason.size());
    if (picoquic_close_ex(impl_->connection,application_error,impl_->close_reason.c_str()) != 0) return false;
    impl_->local_close = true;
    return impl_->flush();
}
}  // namespace moq::interop::transport::test
