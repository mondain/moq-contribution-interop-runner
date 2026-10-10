#include "moq/interop/transport/webtransport_listener.h"

#include <gtest/gtest.h>

#include <pico_webtransport.h>
#include <picoquic.h>
#include <picoquic_internal.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <functional>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

using namespace moq::interop::transport;

namespace {

WebTransportListenerConfig config() {
    WebTransportListenerConfig result;
    result.quic.bind_address = "127.0.0.1";
    result.quic.bind_port = 0;
    result.quic.certificate_path =
        std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem";
    result.quic.private_key_path =
        std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem";
    result.application_protocol = "moqt-21";
    result.allowed_origins = {"https://publisher.test"};
    return result;
}

TEST(WebTransportListener, RejectsInvalidProtocolAndMissingOriginPolicy) {
    auto settings = config();
    settings.application_protocol = "moqt-16";
    const auto wrong = WebTransportListener::create(std::move(settings));
    EXPECT_EQ(wrong.error, NativeQuicListenerError::InvalidConfiguration);
    settings = config();
    settings.allowed_origins.clear();
    settings.require_origin = true;
    const auto no_origin = WebTransportListener::create(std::move(settings));
    EXPECT_EQ(no_origin.error, NativeQuicListenerError::InvalidConfiguration);
}

TEST(WebTransportListener, AcceptsMoqLiteProtocol) {
    auto settings = config();
    settings.application_protocol = "moq-lite-06";
    const auto result = WebTransportListener::create(std::move(settings));
    EXPECT_NE(result.listener, nullptr);
    EXPECT_EQ(result.error, std::nullopt);
}

TEST(WebTransportListener, AcceptsDraft22ProtocolAndRejectsMoqt23) {
    auto settings = config();
    settings.application_protocol = "moqt-22";
    EXPECT_EQ(WebTransportListener::create(std::move(settings)).error, std::nullopt);
    settings = config();
    settings.application_protocol = "moqt-23";
    EXPECT_EQ(WebTransportListener::create(std::move(settings)).error,
              NativeQuicListenerError::InvalidConfiguration);
}

TEST(WebTransportListener, BindsAndReleasesUdpPortWithoutAdmittingSession) {
    auto created = WebTransportListener::create(config());
    ASSERT_EQ(created.error, std::nullopt);
    ASSERT_NE(created.listener, nullptr);
    const auto port = created.listener->bound_endpoint().port;
    EXPECT_NE(port, 0);
    EXPECT_TRUE(created.listener->poll(1).empty());
    EXPECT_EQ(created.listener->open_bidi().status, TransportStatus::InvalidState);
    created.listener.reset();
    auto repeat = config();
    repeat.quic.bind_port = port;
    auto rebound = WebTransportListener::create(std::move(repeat));
    ASSERT_EQ(rebound.error, std::nullopt);
    ASSERT_NE(rebound.listener, nullptr);
    EXPECT_EQ(rebound.listener->bound_endpoint().port, port);
}

struct ClientState {
    bool accepted = false;
    bool refused = false;
    std::uint64_t expected_server_stream = 0;
    std::vector<std::uint8_t> server_bytes;
    std::vector<std::uint64_t> reset_streams;
};

int client_callback(picoquic_cnx_t*, std::uint8_t* bytes, std::size_t length,
                    picohttp_call_back_event_t event, h3zero_stream_ctx_t* stream,
                    void* context) {
    auto* state = static_cast<ClientState*>(context);
    if (event == picohttp_callback_connect_accepted) state->accepted = true;
    if (event == picohttp_callback_connect_refused) state->refused = true;
    if (event == picohttp_callback_reset && stream != nullptr)
        state->reset_streams.push_back(stream->stream_id);
    if (event == picohttp_callback_post_data && stream != nullptr &&
        stream->stream_id == state->expected_server_stream && bytes != nullptr)
        state->server_bytes.insert(state->server_bytes.end(), bytes, bytes + length);
    return 0;
}

// The client callback state of the session a test body is running in (exercise_connect).
ClientState* live_state = nullptr;

// What a test body sees once the CONNECT is accepted: the server listener, the
// client connection, and a step() that moves packets both ways and collects the
// server's events.
struct LiveSession {
    WebTransportListener* listener = nullptr;
    picoquic_cnx_t* cnx = nullptr;
    h3zero_callback_ctx_t* h3 = nullptr;
    h3zero_stream_ctx_t* control = nullptr;
    std::function<void()> step;
    std::vector<TransportEvent> events;
};

void exercise_connect(const char* token, const char* offered,
                      const char* origin, bool expected_accept,
                      const char* application_protocol = "moqt-21",
                      bool scheme_http = false,
                      bool require_origin = false,
                      const std::function<void(WebTransportListenerConfig&)>& tune = {},
                      const std::function<void(LiveSession&)>& body = {},
                      const std::function<void(picoquic_cnx_t*)>& tune_client = {}) {
    auto settings = config();
    if (tune) tune(settings);
    settings.application_protocol = application_protocol;
    settings.require_origin = require_origin;
    auto created = WebTransportListener::create(std::move(settings));
    ASSERT_EQ(created.error, std::nullopt);
    const auto port = created.listener->bound_endpoint().port;
    int socket_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(socket_fd, 0);
    const int flags = ::fcntl(socket_fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK), 0);
    sockaddr_in client_address{};
    client_address.sin_family = AF_INET;
    client_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client_address.sin_port = 0;
    ASSERT_EQ(::bind(socket_fd, reinterpret_cast<sockaddr*>(&client_address),
                     sizeof(client_address)), 0);
    socklen_t address_size = sizeof(client_address);
    ASSERT_EQ(::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&client_address),
                            &address_size), 0);
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port = htons(port);

    auto* quic = picoquic_create(1, nullptr, nullptr, nullptr, "h3", nullptr,
                                 nullptr, nullptr, nullptr, nullptr,
                                 picoquic_current_time(), nullptr, nullptr,
                                 nullptr, 0);
    ASSERT_NE(quic, nullptr);
    picoquic_set_null_verifier(quic);
    picoquic_cnx_t* cnx = nullptr;
    h3zero_callback_ctx_t* h3 = nullptr;
    h3zero_stream_ctx_t* control = nullptr;
    ASSERT_EQ(picowt_prepare_client_cnx(
        quic, reinterpret_cast<sockaddr*>(&server_address), &cnx, &h3,
        &control, picoquic_current_time(), "runner.test"), 0);
    if (tune_client) tune_client(cnx);
    ASSERT_EQ(picoquic_start_client_cnx(cnx), 0);
    ClientState state;
    std::array<std::uint8_t, 2048> outgoing{};
    std::array<std::uint8_t, 2048> incoming{};
    bool connect_sent = false;
    bool established = false;
    for (int step = 0; step < 3000 &&
         !(expected_accept ? (established && state.accepted) : state.refused);
         ++step) {
        for (int packet = 0; packet < 8; ++packet) {
            sockaddr_storage destination{};
            sockaddr_storage source{};
            picoquic_connection_id_t log_id{};
            picoquic_cnx_t* last = nullptr;
            std::size_t length = 0;
            int interface_index = 0;
            ASSERT_EQ(picoquic_prepare_next_packet(
                quic, picoquic_current_time(), outgoing.data(), outgoing.size(),
                &length, &destination, &source, &interface_index, &log_id,
                &last), 0);
            if (length == 0) break;
            ASSERT_EQ(::sendto(socket_fd, outgoing.data(), length, 0,
                               reinterpret_cast<sockaddr*>(&destination),
                               sizeof(server_address)), static_cast<ssize_t>(length));
        }
        const auto server_events = created.listener->poll(8);
        for (const auto& event : server_events) {
            if (const auto* ready = std::get_if<ConnectionEstablishedEvent>(&event)) {
                established = true;
                std::vector<std::byte> expected_protocol;
                for (const char c : std::string_view{application_protocol})
                    expected_protocol.push_back(
                        static_cast<std::byte>(static_cast<unsigned char>(c)));
                EXPECT_EQ(ready->alpn, expected_protocol);
            }
        }
        for (int packet = 0; packet < 8; ++packet) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const auto length = ::recvfrom(socket_fd, incoming.data(), incoming.size(),
                                           0, reinterpret_cast<sockaddr*>(&peer),
                                           &peer_size);
            if (length < 0) break;
            auto local = client_address;
            ASSERT_EQ(picoquic_incoming_packet(
                quic, incoming.data(), static_cast<std::size_t>(length),
                reinterpret_cast<sockaddr*>(&peer),
                reinterpret_cast<sockaddr*>(&local), 0, 0,
                picoquic_current_time()), 0);
        }
        if (!connect_sent && h3->settings.settings_received) {
            const std::string authority = "127.0.0.1:" + std::to_string(port);
            std::array<std::uint8_t, 512> qpack{};
            const auto* path = reinterpret_cast<const std::uint8_t*>("/moq");
            auto* qpack_end = h3zero_create_connect_header_frame(
                qpack.data(), qpack.data() + qpack.size(), authority.c_str(),
                path, 4, token, origin, nullptr, offered);
            ASSERT_NE(qpack_end, nullptr);
            if (scheme_http) {
                const auto scheme = std::find(qpack.begin(), qpack.begin() +
                    (qpack_end - qpack.data()), std::uint8_t{0xd7});
                ASSERT_NE(scheme, qpack.begin() + (qpack_end - qpack.data()));
                *scheme = 0xd6;
            }
            std::array<std::uint8_t, 1024> frame{};
            auto* cursor = picoquic_frames_varint_encode(
                frame.data(), frame.data() + frame.size(), h3zero_frame_header);
            ASSERT_NE(cursor, nullptr);
            cursor = picoquic_frames_varint_encode(
                cursor, frame.data() + frame.size(), qpack_end - qpack.data());
            ASSERT_NE(cursor, nullptr);
            std::memcpy(cursor, qpack.data(), qpack_end - qpack.data());
            cursor += qpack_end - qpack.data();
            control->path_callback = client_callback;
            control->path_callback_ctx = &state;
            control->is_open = 1;
            control->ps.stream_state.is_upgrade_requested = 1;
            ASSERT_EQ(picoquic_add_to_stream_with_ctx(
                cnx, control->stream_id, frame.data(), cursor - frame.data(),
                0, control), 0);
            connect_sent = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(connect_sent);
    EXPECT_EQ(established, expected_accept);
    EXPECT_EQ(state.accepted, expected_accept);
    EXPECT_EQ(state.refused, !expected_accept);
    if (expected_accept && state.accepted) {
        auto* data_stream = picowt_create_local_stream(cnx, 1, h3,
                                                       control->stream_id);
        ASSERT_NE(data_stream, nullptr);
        static constexpr std::array<std::uint8_t, 3> payload{'m', 'o', 'q'};
        ASSERT_EQ(picoquic_add_to_stream_with_ctx(
            cnx, data_stream->stream_id, payload.data(), payload.size(), 1,
            data_stream), 0);
        static constexpr std::array<std::uint8_t, 3> datagram{0, 'd', 'g'};
        ASSERT_EQ(picoquic_queue_datagram_frame(cnx, datagram.size(),
                                                datagram.data()), 0);
        bool delivered = false;
        bool datagram_delivered = false;
        for (int step = 0; step < 1000 && !(delivered && datagram_delivered); ++step) {
            for (int packet = 0; packet < 8; ++packet) {
                sockaddr_storage destination{};
                sockaddr_storage source{};
                picoquic_connection_id_t log_id{};
                picoquic_cnx_t* last = nullptr;
                std::size_t length = 0;
                int interface_index = 0;
                ASSERT_EQ(picoquic_prepare_next_packet(
                    quic, picoquic_current_time(), outgoing.data(), outgoing.size(),
                    &length, &destination, &source, &interface_index, &log_id,
                    &last), 0);
                if (length == 0) break;
                ASSERT_EQ(::sendto(socket_fd, outgoing.data(), length, 0,
                    reinterpret_cast<sockaddr*>(&destination), sizeof(server_address)),
                    static_cast<ssize_t>(length));
            }
            for (const auto& event : created.listener->poll(8)) {
                if (const auto* data = std::get_if<StreamDataEvent>(&event)) {
                    if (data->stream_id == data_stream->stream_id) {
                        EXPECT_EQ(data->data, (std::vector<std::byte>{
                            std::byte{'m'}, std::byte{'o'}, std::byte{'q'}}));
                        EXPECT_TRUE(data->fin);
                        delivered = true;
                    }
                } else if (const auto* received = std::get_if<DatagramEvent>(&event)) {
                    EXPECT_EQ(received->data, (std::vector<std::byte>{
                        std::byte{'d'}, std::byte{'g'}}));
                    datagram_delivered = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        EXPECT_TRUE(delivered);
        EXPECT_TRUE(datagram_delivered);

        ASSERT_EQ(h3zero_declare_stream_prefix(h3, control->stream_id,
                                               client_callback, &state), 0);
        const auto server_stream = created.listener->open_bidi();
        ASSERT_EQ(server_stream.status, TransportStatus::Success);
        state.expected_server_stream = server_stream.stream_id;
        static constexpr std::array<std::byte, 3> request{
            std::byte{'r'}, std::byte{'e'}, std::byte{'q'}};
        ASSERT_EQ(created.listener->write(server_stream.stream_id, request, false).status,
                  TransportStatus::Success);
        for (int step = 0; step < 1000 && state.server_bytes.size() < request.size();
             ++step) {
            (void)created.listener->poll(8);
            for (int packet = 0; packet < 8; ++packet) {
                sockaddr_in peer{};
                socklen_t peer_size = sizeof(peer);
                const auto length = ::recvfrom(socket_fd, incoming.data(), incoming.size(),
                    0, reinterpret_cast<sockaddr*>(&peer), &peer_size);
                if (length < 0) break;
                auto local = client_address;
                ASSERT_EQ(picoquic_incoming_packet(quic, incoming.data(),
                    static_cast<std::size_t>(length),
                    reinterpret_cast<sockaddr*>(&peer),
                    reinterpret_cast<sockaddr*>(&local), 0, 0,
                    picoquic_current_time()), 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        EXPECT_EQ(state.server_bytes,
                  (std::vector<std::uint8_t>{'r', 'e', 'q'}));
        auto* reply_stream = h3zero_find_stream(h3, server_stream.stream_id);
        ASSERT_NE(reply_stream, nullptr);
        static constexpr std::array<std::uint8_t, 3> reply{'o', 'k', '!'};
        ASSERT_EQ(picoquic_add_to_stream_with_ctx(cnx, server_stream.stream_id,
            reply.data(), reply.size(), 0, reply_stream), 0);
        bool reply_received = false;
        for (int step = 0; step < 1000 && !reply_received; ++step) {
            for (int packet = 0; packet < 8; ++packet) {
                sockaddr_storage destination{};
                sockaddr_storage source{};
                picoquic_connection_id_t log_id{};
                picoquic_cnx_t* last = nullptr;
                std::size_t length = 0;
                int interface_index = 0;
                ASSERT_EQ(picoquic_prepare_next_packet(quic, picoquic_current_time(),
                    outgoing.data(), outgoing.size(), &length, &destination,
                    &source, &interface_index, &log_id, &last), 0);
                if (length == 0) break;
                ASSERT_EQ(::sendto(socket_fd, outgoing.data(), length, 0,
                    reinterpret_cast<sockaddr*>(&destination), sizeof(server_address)),
                    static_cast<ssize_t>(length));
            }
            for (const auto& event : created.listener->poll(8)) {
                if (const auto* data = std::get_if<StreamDataEvent>(&event)) {
                    if (data->stream_id == server_stream.stream_id) {
                        EXPECT_EQ(data->data, (std::vector<std::byte>{
                            std::byte{'o'}, std::byte{'k'}, std::byte{'!'}}));
                        reply_received = true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        EXPECT_TRUE(reply_received);
    }
    if (expected_accept && state.accepted && body) {
        live_state = &state;
        LiveSession live;
        live.listener = created.listener.get();
        live.cnx = cnx;
        live.h3 = h3;
        live.control = control;
        live.step = [&] {
            for (int packet = 0; packet < 8; ++packet) {
                sockaddr_storage destination{};
                sockaddr_storage source{};
                picoquic_connection_id_t log_id{};
                picoquic_cnx_t* last = nullptr;
                std::size_t length = 0;
                int interface_index = 0;
                if (picoquic_prepare_next_packet(quic, picoquic_current_time(), outgoing.data(),
                        outgoing.size(), &length, &destination, &source, &interface_index,
                        &log_id, &last) != 0 || length == 0) break;
                (void)::sendto(socket_fd, outgoing.data(), length, 0,
                               reinterpret_cast<sockaddr*>(&destination), sizeof(server_address));
            }
            for (auto& event : created.listener->poll(64)) live.events.push_back(std::move(event));
            for (int packet = 0; packet < 8; ++packet) {
                sockaddr_in peer{};
                socklen_t peer_size = sizeof(peer);
                const auto length = ::recvfrom(socket_fd, incoming.data(), incoming.size(), 0,
                                               reinterpret_cast<sockaddr*>(&peer), &peer_size);
                if (length < 0) break;
                auto local = client_address;
                (void)picoquic_incoming_packet(quic, incoming.data(), static_cast<std::size_t>(length),
                    reinterpret_cast<sockaddr*>(&peer), reinterpret_cast<sockaddr*>(&local), 0, 0,
                    picoquic_current_time());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        };
        body(live);
        live_state = nullptr;
    }
    h3zero_callback_delete_context(cnx, h3);
    picoquic_free(quic);
    ::close(socket_fd);
}

TEST(WebTransportListener, AdmitsExactDraft21ConnectOverHttp3) {
    exercise_connect("webtransport-h3", "\"moqt-21\"",
                     "https://publisher.test", true);
}

TEST(WebTransportListener, AdmitsExactDraft18ConnectOverHttp3) {
    exercise_connect("webtransport-h3", "\"moqt-18\"",
                     "https://publisher.test", true, "moqt-18");
}

TEST(WebTransportListener, AdmitsMoqLiteConnectOverHttp3) {
    exercise_connect("webtransport-h3", "\"moq-lite-06\"",
                     "https://publisher.test", true, "moq-lite-06");
}

TEST(WebTransportListener, MixedOffersResolveToTheConfiguredProtocol) {
    exercise_connect("webtransport-h3", "\"moqt-22\", \"moq-lite-06\"",
                     "https://publisher.test", true, "moq-lite-06");
    exercise_connect("webtransport-h3", "\"moq-lite-06\", \"moqt-22\"",
                     "https://publisher.test", true, "moqt-22");
}

TEST(WebTransportListener, MoqLiteListenerRefusesAMoqTransportOnlyOffer) {
    exercise_connect("webtransport-h3", "\"moqt-22\"",
                     "https://publisher.test", false, "moq-lite-06");
}

TEST(WebTransportListener, RejectsLegacyTokenBeforeMoqt) {
    exercise_connect("webtransport", "\"moqt-21\"",
                     "https://publisher.test", false);
}

TEST(WebTransportListener, RejectsWrongDraftOfferBeforeMoqt) {
    exercise_connect("webtransport-h3", "\"moqt-18\"",
                     "https://publisher.test", false);
}

TEST(WebTransportListener, RejectsDisallowedOriginBeforeMoqt) {
    exercise_connect("webtransport-h3", "\"moqt-21\"",
                     "https://other.test", false);
}

TEST(WebTransportListener, RejectsHttpSchemeBeforeMoqt) {
    exercise_connect("webtransport-h3", "\"moqt-21\"",
                     "https://publisher.test", false, "moqt-21", true);
}

TEST(WebTransportListener, AcceptsNonBrowserClientWithoutOrigin) {
    exercise_connect("webtransport-h3", "\"moqt-21\"", nullptr, true);
}

TEST(WebTransportListener, RejectsMissingOriginWhenPolicyRequiresIt) {
    exercise_connect("webtransport-h3", "\"moqt-21\"", nullptr,
                     false, "moqt-21", false, true);
}

// Bytes the listener reports for a client WebTransport unidirectional stream that
// writes 1000 bytes against a 100 byte stream window.
std::size_t received_on_wt_uni_stream(bool hold_credit) {
    std::size_t received = 0;
    exercise_connect("webtransport-h3", "\"moqt-21\"", "https://publisher.test", true, "moqt-21",
        false, false,
        [&](WebTransportListenerConfig& settings) {
            settings.quic.initial_max_stream_data_uni = 100;
            settings.quic.hold_uni_stream_credit = hold_credit;
            settings.quic.initial_max_streams_uni = 16;
        },
        [&](LiveSession& live) {
            auto* stream = picowt_create_local_stream(live.cnx, 0, live.h3, live.control->stream_id);
            ASSERT_NE(stream, nullptr);
            const std::vector<std::uint8_t> payload(1000, 'x');
            ASSERT_EQ(picoquic_add_to_stream_with_ctx(live.cnx, stream->stream_id, payload.data(),
                                                      payload.size(), 1, stream), 0);
            const auto stream_id = stream->stream_id;  // the context is freed once the stream ends
            for (int step = 0; step < 600; ++step) {
                live.step();
                for (const auto& event : live.events)
                    if (const auto* data = std::get_if<StreamDataEvent>(&event))
                        if (data->stream_id == stream_id) received += data->data.size();
                live.events.clear();
            }
        });
    return received;
}

TEST(WebTransportListener, HeldUniStreamCreditStopsAPeerStreamAtTheInitialWindow) {
    const auto held = received_on_wt_uni_stream(true);
    const auto released = received_on_wt_uni_stream(false);
    EXPECT_GT(held, 0u);
    EXPECT_LE(held, 100u);
    EXPECT_EQ(released, 1000u);
}

TEST(WebTransportListener, GrantedStreamCreditRaisesThePeerLimitsAndLetsItOpenStreams) {
    exercise_connect("webtransport-h3", "\"moqt-21\"", "https://publisher.test", true, "moqt-21",
        false, false,
        [](WebTransportListenerConfig& settings) {
            settings.quic.initial_max_streams_bidi = 2;  // the CONNECT stream and the harness data stream
            settings.quic.initial_max_streams_uni = 3;   // only the three HTTP/3 streams fit
        },
        [](LiveSession& live) {
            for (int step = 0; step < 50; ++step) live.step();
            const auto bidi_before = live.cnx->max_streams_bidir_remote;
            const auto uni_before = live.cnx->max_streams_unidir_remote;
            EXPECT_EQ(live.listener->grant_peer_streams(true, 4).status, TransportStatus::Success);
            EXPECT_EQ(live.listener->grant_peer_streams(false, 4).status, TransportStatus::Success);
            for (int step = 0; step < 300 && (live.cnx->max_streams_bidir_remote < bidi_before + 4 ||
                                              live.cnx->max_streams_unidir_remote < uni_before + 4);
                 ++step) live.step();
            EXPECT_EQ(live.cnx->max_streams_bidir_remote, bidi_before + 4);
            EXPECT_EQ(live.cnx->max_streams_unidir_remote, uni_before + 4);
            // The raised credit is usable: a new bidirectional stream reaches the listener.
            auto* stream = picowt_create_local_stream(live.cnx, 1, live.h3, live.control->stream_id);
            ASSERT_NE(stream, nullptr);
            const std::array<std::uint8_t, 3> payload{'m', 'o', 'q'};
            ASSERT_EQ(picoquic_add_to_stream_with_ctx(live.cnx, stream->stream_id, payload.data(),
                                                      payload.size(), 1, stream), 0);
            const auto stream_id = stream->stream_id;  // the context is freed once the stream ends
            bool delivered = false;
            for (int step = 0; step < 600 && !delivered; ++step) {
                live.step();
                for (const auto& event : live.events)
                    if (const auto* data = std::get_if<StreamDataEvent>(&event))
                        delivered = delivered || (data->stream_id == stream_id && data->fin);
            }
            EXPECT_TRUE(delivered);
        });
}

TEST(WebTransportListener, InboundDropHidesPeerStreamsUntilReenabled) {
    exercise_connect("webtransport-h3", "\"moqt-21\"", "https://publisher.test", true, "moqt-21",
        false, false, {},
        [](LiveSession& live) {
            const auto send = [&](std::uint8_t tag) {
                auto* stream = picowt_create_local_stream(live.cnx, 1, live.h3, live.control->stream_id);
                EXPECT_NE(stream, nullptr);
                if (stream == nullptr) return std::uint64_t{0};
                EXPECT_EQ(picoquic_add_to_stream_with_ctx(live.cnx, stream->stream_id, &tag, 1, 1, stream), 0);
                return stream->stream_id;
            };
            const auto seen = [&](std::uint64_t id) {
                for (const auto& event : live.events)
                    if (const auto* data = std::get_if<StreamDataEvent>(&event))
                        if (data->stream_id == id) return true;
                return false;
            };
            EXPECT_EQ(live.listener->set_inbound_drop(true).status, TransportStatus::Success);
            live.events.clear();
            const auto dropped = send(1);
            for (int step = 0; step < 300; ++step) live.step();
            EXPECT_FALSE(seen(dropped));
            EXPECT_EQ(live.listener->set_inbound_drop(false).status, TransportStatus::Success);
            const auto kept = send(2);
            for (int step = 0; step < 300 && !seen(kept); ++step) live.step();
            EXPECT_TRUE(seen(kept));
            EXPECT_FALSE(seen(dropped));
        });
}

// A client that does not enable RESET_STREAM_AT (the moq CLI's WebTransport stack does not).
void without_reset_stream_at(picoquic_cnx_t* cnx) {
    picoquic_tp_t parameters = *picoquic_get_transport_parameters(cnx, 1);
    parameters.is_reset_stream_at_enabled = 0;
    picoquic_set_transport_parameters(cnx, &parameters);
}

// The moq-lite profile admits a client without RESET_STREAM_AT, so resetting a runner-opened stream must still
// work there: picowt_reset_stream sends RESET_STREAM_AT (reliable size covers the WebTransport stream header),
// which picoquic refuses on such a connection; the session falls back to a plain RESET_STREAM.
TEST(WebTransportListener, MoqLiteResetsARunnerStreamWhenThePeerLacksResetStreamAt) {
    exercise_connect("webtransport-h3", "\"moq-lite-06\"", "https://publisher.test", true, "moq-lite-06",
        false, false, {},
        [](LiveSession& live) {
            EXPECT_EQ(live.cnx->is_reset_stream_at_enabled, 0u);
            const auto stream = live.listener->open_bidi();
            ASSERT_EQ(stream.status, TransportStatus::Success);
            live_state->expected_server_stream = stream.stream_id;
            live_state->server_bytes.clear();
            static constexpr std::array<std::byte, 1> byte{std::byte{'s'}};
            ASSERT_EQ(live.listener->write(stream.stream_id, byte, false).status, TransportStatus::Success);
            for (int step = 0; step < 600 && live_state->server_bytes.empty(); ++step) live.step();
            ASSERT_EQ(live_state->server_bytes, (std::vector<std::uint8_t>{'s'}));
            EXPECT_EQ(live.listener->reset(stream.stream_id, 0x4d1).status, TransportStatus::Success);
            const auto reset_seen = [&] {
                return std::find(live_state->reset_streams.begin(), live_state->reset_streams.end(),
                                 stream.stream_id) != live_state->reset_streams.end();
            };
            for (int step = 0; step < 600 && !reset_seen(); ++step) live.step();
            EXPECT_TRUE(reset_seen());
            EXPECT_EQ(picoquic_get_remote_stream_error(live.cnx, stream.stream_id),
                      0x52e4a40fa8dbULL + 0x4d1 + 0x4d1 / 0x1e);  // the HTTP/3 code of WebTransport error 0x4d1
            // A second reset of the same stream is refused (the stream is finished), as with RESET_STREAM_AT.
            EXPECT_EQ(live.listener->reset(stream.stream_id, 0x4d1).status, TransportStatus::InvalidState);
        },
        without_reset_stream_at);
}

// The same client on a MoQ Transport listener is refused at CONNECT (validate_connect requires RESET_STREAM_AT),
// so the plain RESET_STREAM fallback is unreachable for drafts 18, 21 and 22.
TEST(WebTransportListener, MoqTransportRefusesAClientWithoutResetStreamAt) {
    for (const char* draft : {"moqt-18", "moqt-21", "moqt-22"}) {
        const std::string offered = std::string{"\""} + draft + "\"";
        exercise_connect("webtransport-h3", offered.c_str(), "https://publisher.test", false, draft,
                         false, false, {}, {}, without_reset_stream_at);
    }
}

// With RESET_STREAM_AT negotiated the reset still goes through picowt_reset_stream (unchanged path).
TEST(WebTransportListener, MoqLiteResetsARunnerStreamWithResetStreamAt) {
    exercise_connect("webtransport-h3", "\"moq-lite-06\"", "https://publisher.test", true, "moq-lite-06",
        false, false, {},
        [](LiveSession& live) {
            EXPECT_EQ(live.cnx->is_reset_stream_at_enabled, 1u);
            const auto stream = live.listener->open_bidi();
            ASSERT_EQ(stream.status, TransportStatus::Success);
            live_state->expected_server_stream = stream.stream_id;
            live_state->server_bytes.clear();
            static constexpr std::array<std::byte, 1> byte{std::byte{'s'}};
            ASSERT_EQ(live.listener->write(stream.stream_id, byte, false).status, TransportStatus::Success);
            for (int step = 0; step < 600 && live_state->server_bytes.empty(); ++step) live.step();
            EXPECT_EQ(live.listener->reset(stream.stream_id, 7).status, TransportStatus::Success);
            const auto reset_seen = [&] {
                return std::find(live_state->reset_streams.begin(), live_state->reset_streams.end(),
                                 stream.stream_id) != live_state->reset_streams.end();
            };
            for (int step = 0; step < 600 && !reset_seen(); ++step) live.step();
            EXPECT_TRUE(reset_seen());
        });
}

}  // namespace
