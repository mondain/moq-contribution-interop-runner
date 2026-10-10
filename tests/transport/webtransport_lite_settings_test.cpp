// The HTTP/3 SETTINGS a WebTransport listener sends and accepts, per profile (L1e Task 4 live smoke against the moq
// CLI, moq-dev/moq b8b0d235, whose WebTransport stack is web-transport-proto 0.6.2).
//
// That client refuses a server whose SETTINGS carry no identifier it knows: it needs H3_DATAGRAM (0x33, or 0xffd277)
// = 1 and then WEBTRANSPORT_MAX_SESSIONS (0xc671706a) non-zero, or the pre-draft-07 pair WEBTRANSPORT_ENABLE
// (0x2b603742) = 1 [+ MAX_SESSIONS 0x2b603743]. h3zero advertises SETTINGS_WT_ENABLED (0x2c7cf000) only, so the
// lite listener adds WEBTRANSPORT_MAX_SESSIONS = 1 (one session per connection, as picoquic keeps). The client sends
// those identifiers too and no SETTINGS_WT_ENABLED, so the lite listener also admits a client that enables
// WebTransport that way (PeerCapabilities::legacy_webtransport). The MoQ Transport profiles keep h3zero's SETTINGS
// byte for byte and still require SETTINGS_WT_ENABLED from the client.

#include "moq/interop/transport/webtransport_listener.h"

#include <gtest/gtest.h>

#include <h3zero_common.h>
#include <pico_webtransport.h>
#include <picoquic.h>
#include <picoquic_internal.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace moq::interop::transport;

namespace {

constexpr std::uint64_t kWtEnabled = 0x2c7cf000;
constexpr std::uint64_t kWtMaxSessions = 0xc671706a;
constexpr std::uint64_t kWtEnableDeprecated = 0x2b603742;
constexpr std::uint64_t kWtMaxSessionsDeprecated = 0x2b603743;
constexpr std::uint64_t kH3Datagram = 0x33;
constexpr std::uint64_t kH3DatagramDeprecated = 0xffd277;
constexpr std::uint64_t kEnableConnect = 0x8;

WebTransportListenerConfig config(const char* application_protocol) {
    WebTransportListenerConfig result;
    result.quic.bind_address = "127.0.0.1";
    result.quic.bind_port = 0;
    result.quic.certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem";
    result.quic.private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem";
    result.application_protocol = application_protocol;
    result.allowed_origins = {"https://publisher.test"};
    return result;
}

bool take_varint(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t& value) {
    if (p == end) return false;
    const unsigned width = 1u << (*p >> 6u);
    if (static_cast<std::size_t>(end - p) < width) return false;
    value = *p++ & 0x3fu;
    for (unsigned i = 1; i < width; ++i) value = (value << 8u) | *p++;
    return true;
}

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    std::array<std::uint8_t, 8> buffer{};
    auto* end = picoquic_frames_varint_encode(buffer.data(), buffer.data() + buffer.size(), value);
    ASSERT_NE(end, nullptr);
    out.insert(out.end(), buffer.data(), end);
}

// The SETTINGS of an HTTP/3 control stream's first bytes (stream type 0x00, then the SETTINGS frame), or nullopt
// while incomplete.
std::optional<std::map<std::uint64_t, std::uint64_t>> control_settings(const std::vector<std::uint8_t>& bytes) {
    const auto* p = bytes.data();
    const auto* const end = p + bytes.size();
    std::uint64_t stream_type = 0, frame_type = 0, length = 0;
    if (!take_varint(p, end, stream_type) || stream_type != 0) return std::nullopt;
    if (!take_varint(p, end, frame_type) || frame_type != 4) return std::nullopt;
    if (!take_varint(p, end, length) || static_cast<std::uint64_t>(end - p) < length) return std::nullopt;
    const auto* const frame_end = p + length;
    std::map<std::uint64_t, std::uint64_t> settings;
    while (p < frame_end) {
        std::uint64_t id = 0, value = 0;
        if (!take_varint(p, frame_end, id) || !take_varint(p, frame_end, value)) return std::nullopt;
        if (!settings.emplace(id, value).second) return std::nullopt;
    }
    return settings;
}

// web-transport-proto 0.6.2 Settings::supports_webtransport (the moq CLI's check of the server's SETTINGS).
std::uint64_t deployed_client_view(const std::map<std::uint64_t, std::uint64_t>& settings) {
    const auto find = [&](std::uint64_t id) -> std::optional<std::uint64_t> {
        const auto it = settings.find(id);
        if (it == settings.end()) return std::nullopt;
        return it->second;
    };
    auto datagram = find(kH3Datagram);
    if (!datagram) datagram = find(kH3DatagramDeprecated);
    if (datagram != std::optional<std::uint64_t>{1}) return 0;
    if (const auto max = find(kWtMaxSessions)) return *max;
    if (find(kWtEnableDeprecated) != std::optional<std::uint64_t>{1}) return 0;
    return find(kWtMaxSessionsDeprecated).value_or(1);
}

struct Client {
    h3zero_callback_ctx_t* h3 = nullptr;
    // Bytes of the server's first unidirectional stream with stream type 0x00 (its control stream), by stream id.
    std::map<std::uint64_t, std::vector<std::uint8_t>> server_uni;
    // When set: the SETTINGS components this client sends instead of h3zero's.
    std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> own_settings;
    bool own_settings_sent = false;
    // The CONNECT :protocol (the moq CLI sends the pre-draft-09 token "webtransport").
    const char* upgrade_token = "webtransport-h3";
    bool accepted = false;
    bool refused = false;
    std::string refused_connect;  // the listener's account of a refused CONNECT
    std::vector<std::uint8_t> server_control_raw;  // the server's control stream bytes as received (SETTINGS prefix)
};

int connect_callback(picoquic_cnx_t*, std::uint8_t*, std::size_t, picohttp_call_back_event_t event,
                     h3zero_stream_ctx_t*, void* context) {
    auto* client = static_cast<Client*>(context);
    if (event == picohttp_callback_connect_accepted) client->accepted = true;
    if (event == picohttp_callback_connect_refused) client->refused = true;
    return 0;
}

// Sends the client's own control stream (stream type, SETTINGS) and the QPACK encoder/decoder streams, as
// h3zero_protocol_init does with its own SETTINGS.
int send_own_settings(picoquic_cnx_t* cnx, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& components) {
    std::vector<std::uint8_t> payload;
    for (const auto& [id, value] : components) {
        put_varint(payload, id);
        put_varint(payload, value);
    }
    std::vector<std::uint8_t> stream{0x00};
    put_varint(stream, 4);
    put_varint(stream, payload.size());
    stream.insert(stream.end(), payload.begin(), payload.end());
    const auto control = picoquic_get_next_local_stream_id(cnx, 1);
    if (picoquic_add_to_stream(cnx, control, stream.data(), stream.size(), 0) != 0) return -1;
    // First on the wire, as h3zero's own control stream: the listener judges the CONNECT on the SETTINGS it holds.
    if (picoquic_set_stream_priority(cnx, control, 0) != 0) return -1;
    const std::uint8_t encoder = 0x02, decoder = 0x03;
    if (picoquic_add_to_stream(cnx, picoquic_get_next_local_stream_id(cnx, 1), &encoder, 1, 0) != 0) return -1;
    if (picoquic_add_to_stream(cnx, picoquic_get_next_local_stream_id(cnx, 1), &decoder, 1, 0) != 0) return -1;
    return 0;
}

// The client of the running exchange (one at a time; picoquic's callback context is h3zero's).
Client* g_client = nullptr;

// Records the server's unidirectional streams and sends the client's own SETTINGS, then hands every event to
// h3zero (whose context is non-default, so it never replaces this callback).
int sniffing_callback(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes, std::size_t length,
                      picoquic_call_back_event_t event, void* callback_ctx, void* stream_ctx) {
    auto* client = g_client;
    if (client != nullptr) {
        if ((event == picoquic_callback_stream_data || event == picoquic_callback_stream_fin) &&
            (stream_id & 3u) == 3u && bytes != nullptr && length != 0) {
            auto& seen = client->server_uni[stream_id];
            seen.insert(seen.end(), bytes, bytes + length);
        }
        if (client->own_settings && !client->own_settings_sent &&
            (event == picoquic_callback_almost_ready || event == picoquic_callback_ready)) {
            client->own_settings_sent = true;
            if (send_own_settings(cnx, *client->own_settings) != 0) return -1;
        }
    }
    return h3zero_callback(cnx, stream_id, bytes, length, event, callback_ctx, stream_ctx);
}

// Connects to a listener of `application_protocol`, sends a CONNECT to /moq offering it once the server's SETTINGS
// arrived, and runs until the CONNECT is answered (or 3000 steps). Returns the server's control-stream SETTINGS.
std::optional<std::map<std::uint64_t, std::uint64_t>> exchange(const char* application_protocol, Client& client) {
    auto created = WebTransportListener::create(config(application_protocol));
    EXPECT_EQ(created.error, std::nullopt);
    if (created.listener == nullptr) return std::nullopt;
    const auto port = created.listener->bound_endpoint().port;
    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    EXPECT_GE(socket_fd, 0);
    (void)::fcntl(socket_fd, F_SETFL, ::fcntl(socket_fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in client_address{};
    client_address.sin_family = AF_INET;
    client_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(socket_fd, reinterpret_cast<sockaddr*>(&client_address), sizeof(client_address)), 0);
    socklen_t address_size = sizeof(client_address);
    (void)::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&client_address), &address_size);
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port = htons(port);

    auto* quic = picoquic_create(1, nullptr, nullptr, nullptr, "h3", nullptr, nullptr, nullptr, nullptr, nullptr,
                                 picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    EXPECT_NE(quic, nullptr);
    picoquic_set_null_verifier(quic);
    picoquic_cnx_t* cnx = nullptr;
    h3zero_stream_ctx_t* control = nullptr;
    EXPECT_EQ(picowt_prepare_client_cnx(quic, reinterpret_cast<sockaddr*>(&server_address), &cnx, &client.h3,
                                        &control, picoquic_current_time(), "runner.test"),
              0);
    if (client.own_settings) client.h3->settings_sent = 1;  // h3zero then never sends its own SETTINGS
    g_client = &client;
    picoquic_set_callback(cnx, sniffing_callback, client.h3);
    EXPECT_EQ(picoquic_start_client_cnx(cnx), 0);

    std::array<std::uint8_t, 2048> outgoing{};
    std::array<std::uint8_t, 2048> incoming{};
    bool connect_sent = false;
    for (int step = 0; step < 3000 && !client.accepted && !client.refused; ++step) {
        for (int packet = 0; packet < 8; ++packet) {
            sockaddr_storage destination{}, source{};
            picoquic_connection_id_t log_id{};
            picoquic_cnx_t* last = nullptr;
            std::size_t length = 0;
            int interface_index = 0;
            if (picoquic_prepare_next_packet(quic, picoquic_current_time(), outgoing.data(), outgoing.size(), &length,
                                             &destination, &source, &interface_index, &log_id, &last) != 0 ||
                length == 0)
                break;
            (void)::sendto(socket_fd, outgoing.data(), length, 0, reinterpret_cast<sockaddr*>(&destination),
                           sizeof(server_address));
        }
        (void)created.listener->poll(8);
        for (int packet = 0; packet < 8; ++packet) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const auto length = ::recvfrom(socket_fd, incoming.data(), incoming.size(), 0,
                                           reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (length < 0) break;
            auto local = client_address;
            (void)picoquic_incoming_packet(quic, incoming.data(), static_cast<std::size_t>(length),
                                           reinterpret_cast<sockaddr*>(&peer), reinterpret_cast<sockaddr*>(&local),
                                           0, 0, picoquic_current_time());
        }
        if (!connect_sent && client.h3->settings.settings_received) {
            const std::string authority = "127.0.0.1:" + std::to_string(port);
            const std::string offered = std::string{"\""} + application_protocol + "\"";
            std::array<std::uint8_t, 512> qpack{};
            const auto* path = reinterpret_cast<const std::uint8_t*>("/moq");
            auto* qpack_end = h3zero_create_connect_header_frame(qpack.data(), qpack.data() + qpack.size(),
                                                                 authority.c_str(), path, 4, client.upgrade_token,
                                                                 "https://publisher.test", nullptr, offered.c_str());
            EXPECT_NE(qpack_end, nullptr);
            std::vector<std::uint8_t> frame;
            put_varint(frame, h3zero_frame_header);
            put_varint(frame, static_cast<std::uint64_t>(qpack_end - qpack.data()));
            frame.insert(frame.end(), qpack.data(), qpack_end);
            control->path_callback = connect_callback;
            control->path_callback_ctx = &client;
            control->is_open = 1;
            control->ps.stream_state.is_upgrade_requested = 1;
            EXPECT_EQ(picoquic_add_to_stream_with_ctx(cnx, control->stream_id, frame.data(), frame.size(), 0, control),
                      0);
            connect_sent = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(connect_sent);
    client.refused_connect = created.listener->refused_connect();
    std::optional<std::map<std::uint64_t, std::uint64_t>> server_settings;
    for (const auto& [id, bytes] : client.server_uni) {
        if (auto decoded = control_settings(bytes)) {
            server_settings = std::move(decoded);
            client.server_control_raw = bytes;
        }
    }
    h3zero_callback_delete_context(cnx, client.h3);
    picoquic_free(quic);
    ::close(socket_fd);
    g_client = nullptr;
    return server_settings;
}

// What h3zero_protocol_init sends with datagrams enabled: the SETTINGS every MoQ Transport listener keeps.
std::vector<std::uint8_t> h3zero_server_control_bytes() {
    h3zero_settings_t settings{};
    settings.enable_connect_protocol = 1;
    settings.h3_datagram = 1;
    settings.webtransport_enabled = 1;
    settings.webtransport_max_sessions = 1;
    std::array<std::uint8_t, 256> buffer{};
    buffer[0] = 0x00;
    auto* end = h3zero_settings_encode(buffer.data() + 1, buffer.data() + buffer.size(), &settings);
    EXPECT_NE(end, nullptr);
    return std::vector<std::uint8_t>(buffer.data(), end);
}

std::map<std::uint64_t, std::uint64_t> h3zero_server_settings() {
    return control_settings(h3zero_server_control_bytes()).value();
}

TEST(WebTransportLiteSettings, LiteServerSettingsSatisfyTheMoqCliWebTransportStack) {
    Client client;
    const auto settings = exchange("moq-lite-06", client);
    ASSERT_TRUE(settings.has_value());
    EXPECT_TRUE(client.accepted);
    EXPECT_EQ(deployed_client_view(*settings), 1u);
    // h3zero's own identifiers are all still there, unchanged.
    for (const auto& [id, value] : h3zero_server_settings()) EXPECT_EQ(settings->at(id), value) << id;
    EXPECT_EQ(settings->at(kWtMaxSessions), 1u);
    EXPECT_EQ(settings->size(), h3zero_server_settings().size() + 1);
}

TEST(WebTransportLiteSettings, MoqTransportServerSettingsStayH3zeros) {
    for (const char* protocol : {"moqt-18", "moqt-21", "moqt-22"}) {
        Client client;
        const auto settings = exchange(protocol, client);
        ASSERT_TRUE(settings.has_value()) << protocol;
        EXPECT_TRUE(client.accepted) << protocol;
        EXPECT_EQ(*settings, h3zero_server_settings()) << protocol;
        // The raw control-stream prefix (stream type and SETTINGS frame) is h3zero's own, byte for byte.
        const auto expected = h3zero_server_control_bytes();
        ASSERT_GE(client.server_control_raw.size(), expected.size()) << protocol;
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), client.server_control_raw.begin())) << protocol;
        EXPECT_EQ(settings->count(kWtMaxSessions), 0u) << protocol;
        EXPECT_EQ(deployed_client_view(*settings), 0u) << protocol;
    }
}

// The SETTINGS the moq CLI sends (its log: "sending SETTINGS frame"), without SETTINGS_WT_ENABLED.
const std::vector<std::pair<std::uint64_t, std::uint64_t>> kMoqCliClientSettings{
    {kWtMaxSessions, 1},  {kEnableConnect, 1}, {kWtMaxSessionsDeprecated, 1},
    {kH3DatagramDeprecated, 1}, {kH3Datagram, 1}, {kWtEnableDeprecated, 1}};

TEST(WebTransportLiteSettings, LiteAdmitsTheMoqCliClientSettings) {
    Client client;
    client.own_settings = kMoqCliClientSettings;
    (void)exchange("moq-lite-06", client);
    EXPECT_TRUE(client.accepted) << client.refused_connect;
    EXPECT_FALSE(client.refused);
}

// The whole moq CLI handshake: its SETTINGS and its CONNECT :protocol "webtransport".
TEST(WebTransportLiteSettings, LiteAdmitsTheMoqCliConnect) {
    Client client;
    client.own_settings = kMoqCliClientSettings;
    client.upgrade_token = "webtransport";
    (void)exchange("moq-lite-06", client);
    EXPECT_TRUE(client.accepted) << client.refused_connect;
    EXPECT_FALSE(client.refused);
}

TEST(WebTransportLiteSettings, LiteStillRefusesOtherUpgradeTokens) {
    for (const char* token : {"connect-udp", "webtransport-h4", "WebTransport"}) {
        Client client;
        client.upgrade_token = token;
        (void)exchange("moq-lite-06", client);
        EXPECT_FALSE(client.accepted) << token;
        EXPECT_TRUE(client.refused) << token;
    }
}

TEST(WebTransportLiteSettings, MoqTransportStillRefusesTheLegacyUpgradeToken) {
    for (const char* protocol : {"moqt-18", "moqt-21", "moqt-22"}) {
        Client client;
        client.upgrade_token = "webtransport";
        (void)exchange(protocol, client);
        EXPECT_FALSE(client.accepted) << protocol;
        EXPECT_TRUE(client.refused) << protocol;
    }
}

TEST(WebTransportLiteSettings, LiteAdmitsPreDraft07EnableOnly) {
    Client client;
    client.own_settings = {{kEnableConnect, 1}, {kH3Datagram, 1}, {kWtEnableDeprecated, 1}};
    (void)exchange("moq-lite-06", client);
    EXPECT_TRUE(client.accepted) << client.refused_connect;
}

TEST(WebTransportLiteSettings, LiteRefusesClientSettingsWithoutWebTransport) {
    for (const auto& settings : std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>>{
             {{kEnableConnect, 1}, {kH3Datagram, 1}},
             {{kEnableConnect, 1}, {kH3Datagram, 1}, {kWtMaxSessions, 0}},
             {{kEnableConnect, 1}, {kH3Datagram, 1}, {kWtEnableDeprecated, 0}}}) {
        Client client;
        client.own_settings = settings;
        (void)exchange("moq-lite-06", client);
        EXPECT_FALSE(client.accepted);
        EXPECT_TRUE(client.refused);
    }
}

TEST(WebTransportLiteSettings, MoqTransportStillRequiresWtEnabledFromClients) {
    for (const char* protocol : {"moqt-18", "moqt-21", "moqt-22"}) {
        Client client;
        client.own_settings = kMoqCliClientSettings;
        (void)exchange(protocol, client);
        EXPECT_FALSE(client.accepted) << protocol;
        EXPECT_TRUE(client.refused) << protocol;
    }
}

}  // namespace
