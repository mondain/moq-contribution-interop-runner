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
#include <cstring>
#include <filesystem>
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
};

int client_callback(picoquic_cnx_t*, std::uint8_t*, std::size_t,
                    picohttp_call_back_event_t event, h3zero_stream_ctx_t*,
                    void* context) {
    auto* state = static_cast<ClientState*>(context);
    if (event == picohttp_callback_connect_accepted) state->accepted = true;
    if (event == picohttp_callback_connect_refused) state->refused = true;
    return 0;
}

void exercise_connect(const char* token, const char* offered,
                      const char* origin, bool expected_accept,
                      const char* application_protocol = "moqt-21",
                      bool scheme_http = false,
                      bool require_origin = false) {
    auto settings = config();
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

}  // namespace
