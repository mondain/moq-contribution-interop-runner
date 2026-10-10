#pragma once

// Shared plumbing of the live moq-lite-06 run tests (tests/integration/lite_run_live_test.cpp): the production
// NativeRunManager with the moq-lite-06 catalog, and two publisher stand-ins on the loopback:
//   - LiteQuicPublisher: a PicoquicTestClient (native QUIC, ALPN moq-lite-06) whose behavior is the draft-conforming
//     ConformingLitePublisher (tests/support/scripted_lite_peer.h). The publisher reacts to an in-memory mirror
//     (a ScriptedLitePeer) of what the runner did on the wire; what it emits is written to the real connection.
//   - LiteWebTransportPublisher: a picowt client that offers moq-lite-06 in its CONNECT and sends only its own
//     Setup stream (enough for l06-setup-stream on the WebTransport binding).
#include "moq/interop/app/lite_run.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"
#include "support/picoquic_client.h"
#include "support/scripted_lite_peer.h"

#include <gtest/gtest.h>
#include <pico_webtransport.h>
#include <picoquic_internal.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace moq::interop::lite_live {

using namespace std::chrono_literals;
using Client = transport::test::PicoquicTestClient;
using Bytes = std::vector<std::byte>;
using test::lite::ConformingLitePublisher;
using test::lite::ConformingLitePublisherConfig;
using test::lite::ScriptedLitePeer;

inline const std::filesystem::path kRoot{MOQ_INTEROP_PROJECT_SOURCE_DIR};

inline std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto source = requirements::load_draft_source(draft, kRoot / "docs", kRoot / "requirements/draft-digests.json");
    if (draft == 106)
        return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source, kRoot / "requirements/moq-lite-06.json", requirements::CatalogLoadMode::AllowIncomplete));
    const auto path = kRoot / ("requirements/draft" + std::to_string(draft) + ".json");
    if (draft == 22)
        return std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog::load(source, path, requirements::CatalogLoadMode::AllowIncomplete));
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(source, path));
}

inline app::NativeRunManagerConfig manager_config(std::uint16_t port_start = 0, std::uint16_t port_end = 0) {
    return {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = port_start, .port_end = port_end, .maximum_active_runs = 1,
            .certificate_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "cert.pem",
            .private_key_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "key.pem"};
}

// The fixture the live publisher serves: broadcast "demo/live", track "video" (as in tests/support/lite_conformance.h).
inline app::TrackFixture fixture() { return app::TrackFixture{{"demo", "live"}, "video"}; }

inline app::RunConfig lite_config(std::vector<std::string> ids, std::chrono::milliseconds timeout,
                                  app::TransportKind transport = app::TransportKind::NativeQuic,
                                  std::optional<app::TrackFixture> track = fixture()) {
    return app::RunConfig{app::DraftVersion::MoqLite06, transport, app::RunMode::Observed, std::move(ids), timeout,
                          std::move(track), {}};
}

// The conforming publisher the live tests run, on `binding`, dialing the runner's fixed session URL (the path and
// query of app::lite_session_url, as the adapter passes them to a real publisher): on native QUIC its SETUP carries
// Path = "/moq?token=l1d"; on WebTransport the URL is the CONNECT :path and the SETUP carries no Path.
inline ConformingLitePublisherConfig publisher_config(scenarios::LiteBinding binding = scenarios::LiteBinding::NativeQuic) {
    ConformingLitePublisherConfig config;
    config.broadcast = "demo/live";
    config.track = "video";
    config.hop_id = 7;
    // Probe level Report (row 072 judged), four frames in every group and in every group a FETCH may ask for (the
    // fetch probe learns a group of at least three frames): the same settings as the conformance table.
    config.setup_parameters = {{wire::moqlite06::kParamHop, Bytes{std::byte{7}}},
                               {wire::moqlite06::kParamCost, Bytes{std::byte{0}}},
                               {wire::moqlite06::kParamProbe, Bytes{std::byte{1}}}};
    config.frames_per_group = 4;
    config.fetch_frames_per_group = 4;
    config.fetch_last_group = 1000;
    config.binding = binding;
    const auto url = app::lite_session_url(binding == scenarios::LiteBinding::WebTransport
                                               ? app::TransportKind::WebTransport : app::TransportKind::NativeQuic);
    config.session_url_path = url.path;
    config.session_url_query = url.query;
    return config;
}

inline bool any_event(const storage::RunRecord& run, std::string_view kind, std::string_view scenario = {}) {
    return std::any_of(run.events.begin(), run.events.end(), [&](const storage::EvidenceEvent& event) {
        return event.kind == kind && (scenario.empty() || event.scenario_id == std::string(scenario));
    });
}

inline std::size_t count_events(const storage::RunRecord& run, std::string_view kind) {
    return static_cast<std::size_t>(std::count_if(run.events.begin(), run.events.end(),
        [&](const storage::EvidenceEvent& event) { return event.kind == kind; }));
}

// Whether context `ordinal` (1-based) has a listener (its context_ready event is stored).
inline bool context_started(const storage::RunRecord& run, unsigned ordinal) {
    const auto suffix = " ordinal=" + std::to_string(ordinal);
    return std::any_of(run.events.begin(), run.events.end(), [&](const storage::EvidenceEvent& event) {
        return event.kind == "context_ready" && event.detail.ends_with(suffix);
    });
}

// The highest ordinal with a listener so far (0: none).
inline unsigned latest_ready(const storage::RunRecord& run) {
    unsigned latest = 0;
    for (const auto& event : run.events) {
        if (event.kind != "context_ready") continue;
        const auto at = event.detail.rfind(" ordinal=");
        if (at != std::string::npos) latest = std::max(latest, static_cast<unsigned>(std::stoul(event.detail.substr(at + 9))));
    }
    return latest;
}

inline bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

inline bool finalized(const std::shared_ptr<storage::SqliteRunStore>& store, const app::RunId& id) {
    return store->load(id).state == storage::RunState::Finalized;
}

inline requirements::OutcomeState state_of(const storage::RunRecord& run, std::string_view id) {
    const auto found = std::find_if(run.outcomes.begin(), run.outcomes.end(),
                                    [&](const auto& outcome) { return outcome.requirement_id == id; });
    EXPECT_NE(found, run.outcomes.end()) << id;
    return found == run.outcomes.end() ? requirements::OutcomeState::NotRun : found->state;
}

// A free UDP port on the loopback (bound and released).
inline std::uint16_t free_udp_port() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        if (fd >= 0) ::close(fd);
        return 0;
    }
    const auto port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

// The conforming publisher over a real native QUIC connection. step() pumps the connection, mirrors what the
// runner did (stream bytes, FINs, RESET_STREAMs, STOP_SENDINGs) into the ScriptedLitePeer the publisher reacts to,
// lets it react and writes what it emitted (stream data, resets, stop-sendings, a session close) to the wire.
class LiteQuicPublisher {
public:
    LiteQuicPublisher(std::uint16_t port, ConformingLitePublisherConfig config,
                      std::string_view alpn = scenarios::kLiteAlpn)
        : publisher_(std::move(config)), mirror_(publisher_.reaction()) {
        Client::Config client;
        client.port = port;
        client.alpn.assign(reinterpret_cast<const std::byte*>(alpn.data()),
                           reinterpret_cast<const std::byte*>(alpn.data()) + alpn.size());
        // moq-lite needs no QUIC DATAGRAM (decision (c)): the lite listener must accept a client without it.
        client.enable_datagrams = false;
        client_ = Client::create(client);
    }

    [[nodiscard]] bool valid() const { return client_ != nullptr; }
    [[nodiscard]] bool established() const { return client_ && client_->established(); }
    [[nodiscard]] const ConformingLitePublisher& publisher() const { return publisher_; }
    [[nodiscard]] Client& client() { return *client_; }

    // False once the connection failed or ended.
    bool step() {
        if (!client_ || !client_->pump()) return false;
        if (!client_->established()) return true;
        mirror_runner();
        for (auto& event : mirror_.poll(1024)) outbound_.push_back(std::move(event));
        flush();
        return !client_->peer_close().has_value();
    }

private:
    static constexpr std::uint64_t kScannedStreams = 64;

    void mirror_runner() {
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

    void flush() {
        while (!outbound_.empty()) {
            const auto& event = outbound_.front();
            if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
                const auto sent = client_->try_send_stream(data->stream_id, data->data, data->fin);
                if (sent.status == transport::test::ClientStreamSendStatus::WouldBlock) return;
            } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
                (void)client_->reset_stream(reset->stream_id, reset->application_error.value_or(0));
            } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) {
                (void)client_->stop_stream(stop->stream_id, stop->application_error.value_or(0));
            } else if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
                (void)client_->close(close->error_code, close->reason);
            }
            outbound_.pop_front();
        }
    }

    ConformingLitePublisher publisher_;
    ScriptedLitePeer mirror_;
    std::unique_ptr<Client> client_;
    std::deque<transport::TransportEvent> outbound_;
    std::map<std::uint64_t, std::size_t> forwarded_;
    std::set<std::uint64_t> fin_forwarded_;
    std::set<std::uint64_t> reset_forwarded_;
    std::set<std::uint64_t> stop_forwarded_;
};

// Plays every context of run `id` that gets a listener with a fresh publisher from `make` (one connection per
// context, as the runner recreates its listener per context) until the run is finalized. A context the runner
// skips gets no publisher; `make` may return nullptr for a context nobody should dial. `contexts` bounds the
// number of publishers.
template <class Make>
void drive_contexts(const std::shared_ptr<storage::SqliteRunStore>& store, const app::RunId& id, std::uint16_t port,
                    unsigned contexts, Make make, std::chrono::milliseconds limit = 60s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    unsigned served = 0;
    while (served < contexts && std::chrono::steady_clock::now() < deadline) {
        unsigned ordinal = 0;
        if (!wait_for([&] {
                const auto run = store->load(id);
                ordinal = latest_ready(run);
                return run.state == storage::RunState::Finalized || ordinal > served;
            }, limit))
            break;
        if (finalized(store, id)) break;
        served = ordinal;
        auto publisher = make(port, ordinal);
        auto next_check = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            if (publisher) (void)publisher->step();
            if (std::chrono::steady_clock::now() >= next_check) {
                next_check = std::chrono::steady_clock::now() + 20ms;
                const auto run = store->load(id);
                if (run.state == storage::RunState::Finalized || latest_ready(run) > ordinal) break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    EXPECT_TRUE(wait_for([&] { return finalized(store, id); }, 30s));
}

// The CONNECT :path of a lite WebTransport session: the fixed session path and query ("/moq?token=l1d").
inline std::string lite_webtransport_target() {
    const auto url = app::lite_session_url(app::TransportKind::WebTransport);
    return url.path + "?" + url.query;
}

// A WebTransport publisher offering moq-lite-06: CONNECT to https://127.0.0.1:port/moq?token=l1d (or `target`), then
// its own Setup stream
// (STREAM_TYPE 0x1 + `setup`, FIN) on a WebTransport unidirectional stream.
class LiteWebTransportPublisher {
public:
    LiteWebTransportPublisher(std::uint16_t port, Bytes setup_stream, std::string target = lite_webtransport_target())
        : port_(port), setup_(std::move(setup_stream)), target_(std::move(target)) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) return;
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
            return;
        socklen_t size = sizeof(local);
        (void)::getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &size);
        local_ = local;
        remote_.sin_family = AF_INET;
        remote_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        remote_.sin_port = htons(port);
        quic_ = picoquic_create(1, nullptr, nullptr, nullptr, "h3", nullptr, nullptr, nullptr, nullptr, nullptr,
                                picoquic_current_time(), nullptr, nullptr, nullptr, 0);
        if (!quic_) return;
        picoquic_set_null_verifier(quic_);
        ok_ = picowt_prepare_client_cnx(quic_, reinterpret_cast<sockaddr*>(&remote_), &cnx_, &h3_, &control_,
                                        picoquic_current_time(), "runner.test") == 0 &&
              h3zero_declare_stream_prefix(h3_, control_->stream_id, callback, this) == 0 &&
              picoquic_start_client_cnx(cnx_) == 0;
    }
    ~LiteWebTransportPublisher() {
        if (h3_ != nullptr) h3zero_callback_delete_context(cnx_, h3_);
        if (quic_ != nullptr) picoquic_free(quic_);
        if (fd_ >= 0) ::close(fd_);
    }
    LiteWebTransportPublisher(const LiteWebTransportPublisher&) = delete;
    LiteWebTransportPublisher& operator=(const LiteWebTransportPublisher&) = delete;

    [[nodiscard]] bool accepted() const { return accepted_; }
    [[nodiscard]] bool setup_sent() const { return setup_sent_; }

    bool step() {
        if (!ok_) return false;
        std::array<std::uint8_t, 2048> buffer{};
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
        for (int packet = 0; packet < 16; ++packet) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const auto length = ::recvfrom(fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&peer),
                                           &peer_size);
            if (length < 0) break;
            auto received_at = local_;
            (void)picoquic_incoming_packet(quic_, buffer.data(), static_cast<std::size_t>(length),
                                           reinterpret_cast<sockaddr*>(&peer),
                                           reinterpret_cast<sockaddr*>(&received_at), 0, 0, picoquic_current_time());
        }
        if (!connect_sent_ && h3_->settings.settings_received) {
            const std::string authority = "127.0.0.1:" + std::to_string(port_);
            const std::string offered = "\"" + std::string(scenarios::kLiteAlpn) + "\"";
            std::array<std::uint8_t, 512> qpack{};
            auto* end = h3zero_create_connect_header_frame(qpack.data(), qpack.data() + qpack.size(),
                                                           authority.c_str(),
                                                           reinterpret_cast<const std::uint8_t*>(target_.data()), target_.size(),
                                                           "webtransport-h3", nullptr, nullptr, offered.c_str());
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
        }
        if (accepted_ && !setup_sent_) {
            auto* stream = picowt_create_local_stream(cnx_, 0, h3_, control_->stream_id);
            if (stream == nullptr ||
                picoquic_add_to_stream_with_ctx(cnx_, stream->stream_id, reinterpret_cast<const std::uint8_t*>(setup_.data()),
                                                setup_.size(), 1, stream) != 0)
                return false;
            setup_sent_ = true;
        }
        return true;
    }

private:
    static int callback(picoquic_cnx_t*, std::uint8_t*, std::size_t, picohttp_call_back_event_t event,
                        h3zero_stream_ctx_t*, void* context) {
        if (event == picohttp_callback_connect_accepted) static_cast<LiteWebTransportPublisher*>(context)->accepted_ = true;
        return 0;
    }

    std::uint16_t port_;
    Bytes setup_;
    std::string target_;
    int fd_{-1};
    sockaddr_in local_{};
    sockaddr_in remote_{};
    picoquic_quic_t* quic_{nullptr};
    picoquic_cnx_t* cnx_{nullptr};
    h3zero_callback_ctx_t* h3_{nullptr};
    h3zero_stream_ctx_t* control_{nullptr};
    bool ok_{false};
    bool connect_sent_{false};
    bool accepted_{false};
    bool setup_sent_{false};
};

}  // namespace moq::interop::lite_live
