#include "moq/interop/scenarios/immutable_repeat.h"
#include "moq/interop/scenarios/object_repeat.h"
#include "moq/interop/transport/native_quic_listener.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/http/server.h"
#include "moq/interop/scenarios/draft18.h"
#include "moq/interop/scenarios/draft18_close.h"
#include "moq/interop/scenarios/fetch_probe.h"
#include "moq/interop/scenarios/fetch_response.h"
#include "moq/interop/scenarios/request_response.h"
#include "moq/interop/scenarios/range_filter.h"
#include "moq/interop/scenarios/subscription_cancel.h"
#include "moq/interop/scenarios/discovery_overlap.h"
#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/fetch_group_order.h"
#include "moq/interop/scenarios/request_goaway.h"
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/run_controller.h"
#include "support/picoquic_client.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <string_view>
#include <span>
#include <thread>
#include <variant>
#include <vector>

namespace moq::interop::transport {
namespace {

constexpr std::string_view kCertificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIC7TCCAdUCFDuGBhl3l5Z++VCLkvaav4yteBonMA0GCSqGSIb3DQEBCwUAMEUx
CzAJBgNVBAYTAkFVMRMwEQYDVQQIDApTb21lLVN0YXRlMSEwHwYDVQQKDBhJbnRl
cm5ldCBXaWRnaXRzIFB0eSBMdGQwHhcNMjAwMzIzMTYwNzU0WhcNNDcwODA5MTYw
NzU0WjAhMQswCQYDVQQGEwJHQjESMBAGA1UEAwwJcXVpYy50ZWNoMIIBIjANBgkq
hkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAz5bOL7LD9kiIagcVrZqZ13ZcR0KhMuzs
brqULbZKyqC+uBRgINxYJ7LPnJ4LPYuCt/nAaQ7CLXfKgzAMFu8eIK6UEvZA6+7b
20E4rvOpPbTB/T4JbYZNQKyM9AEwr6j0P6vFgrWT7aBzhkmiqEe5vv/7ZOEGb+Ab
+cvMeszfBbk93nyzKdNaUuh95x7/p0Ow315np2PRuoT0QQnA9zE/9eZ3Jah3cNZn
NuQ6BDHlkegzTV5JhYYblRo/pmt2E9E0ha+NWsRLf3ZJUYhkYR3UqMltEKuLglCO
VWBbPmKd4IZUNIotpKMVQSVb9agNBF49hH9iBhN3fBm7Hp8KBpjJLwIDAQABMA0G
CSqGSIb3DQEBCwUAA4IBAQCo/Rn4spa5XFk0cCoKypP27DxePkGD9rQZk/CY4inV
JV16anZ1pr9yfO61+m3fRKTZq7yxtHRDWxDdROHx9LqV1dXLAmh1ecV9Kn6/796O
EHsOcVB0Lfi9Ili7//oUqlhGNploRuQbgWAXU+Eo1xJRWIXeedhzBSgEOMaQk3Zn
TdYFhP0/Ao/fEdI4VULv1A43ztnZIB2KXWgUQoFT32woL47eWge8LxxVmmH3STtz
nNcGnYxIorCQemDHDzMrvxRWgHxkpFGGqAhkFFyCmhKFPglKwt+yVTx26T8tShID
ISMj0rgVMptmtWKJfzNCvFG52gsuO4w3yGdjgjRRrBDm
-----END CERTIFICATE-----
)PEM";

constexpr std::string_view kPrivateKey = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDPls4vssP2SIhq
BxWtmpnXdlxHQqEy7OxuupQttkrKoL64FGAg3Fgnss+cngs9i4K3+cBpDsItd8qD
MAwW7x4grpQS9kDr7tvbQTiu86k9tMH9Pglthk1ArIz0ATCvqPQ/q8WCtZPtoHOG
SaKoR7m+//tk4QZv4Bv5y8x6zN8FuT3efLMp01pS6H3nHv+nQ7DfXmenY9G6hPRB
CcD3MT/15nclqHdw1mc25DoEMeWR6DNNXkmFhhuVGj+ma3YT0TSFr41axEt/dklR
iGRhHdSoyW0Qq4uCUI5VYFs+Yp3ghlQ0ii2koxVBJVv1qA0EXj2Ef2IGE3d8Gbse
nwoGmMkvAgMBAAECggEBAMtFkpUmablKgTnBwjqCvs47OlUVK6AgW8x5qwuwC0Cr
ctXyLcc/vJry/1UPdVZIvDHGv+Cf8Qhw2r7nV49FiqzaBmki9aOR+3uRPB4kvr6L
t8Fw8+5pqlAAJu3wFGqN+M44N2mswDPaAAWpKTu7MGmVY+f+aT03qG1MYOiGoISK
gP6DHiinddD38spM2muyCUyFZk9a+aBEfaQzZoU3gc0yB6R/qBOWZ7NIoIUMicku
Zf3L6/06uunyZp+ueR83j1YWbg3JoYKlGAuQtDRF709+MQrim8lKTnfuHiBeZKYZ
GNLSo7lGjrp6ccSyfXmlA36hSfdlrWtZJ4+utZShftECgYEA+NNOFNa1BLfDw3ot
a6L4W6FE45B32bLbnBdg8foyEYrwzHLPFCbws1Z60pNr7NaCHDIMiKVOXvKQa78d
qdWuPUVJ83uVs9GI8tAo00RAvBn6ut9yaaLa8mIv6ZpfU20IgE5sDjB7IBY9tTVd
EDyJcDuKQXzQ48qmEw86wINQMd0CgYEA1ZMdt7yLnpDiYa6M/BuKjp7PWKcRlzVM
BcCEYHA4LJ6xEOH4y9DEx2y5ljwOcXgJhXAfAyGQr7s1xiP/nXurqfmdP8u7bawp
VwuWJ8Vv0ZXITaU0isezG2Dpnseuion3qSraWlmWUlWLVVgKETZmk7cF7VIXa0NT
LFREdObI5HsCgYBUbm8KRyi5Zxm4VNbgtTYM8ZYMmdLxPe2i85PjyAABT+IRncuC
jQwT7n5Swc9XWBpiMuFp5J3JPgmfZgRMwsMS61YClqbfk3Qi4FtaBMjqiu43Rubt
zWL56DNV0xoRlufRkcq8rdq5spJR0L+5aLFCMhHh0taW1QaxZPOMq4IkyQKBgQC3
GetubGzewqPyzuz77ri5URm+jW0dT4ofnE9hRpRCXMK9EJ52TkOGHYZ2cIKJcTno
dpl/27Tpk/ykJJSu9SnVDbVszkOf4OuIPty6uCAHdPxG5Q3ItTCulkVz5QmUqHf1
RlHxB8FCUSilQFdRLmx+03h3X9vID+4soQoXlwxAJQKBgE5SQpN+TG5V+E4zHgNd
6cy6gA5dGDJ0KbsgxJwlKTFA9nIcs2ssBxLY9U4x75EGuqpeVNmq6xwwmPtBs0rp
M3W4zdFrZQ3BneFRW7WbSBbsUSprkJW/p4GXa17GzGUq/MDXlGhNlApP1nknzFvE
xGaH0/H/TZxpLCogVP9npUkj
-----END PRIVATE KEY-----
)PEM";

class TestPemFiles {
public:
    TestPemFiles() {
        auto name = std::filesystem::temp_directory_path() /
                    ("moq-interop-pem-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
        std::filesystem::create_directory(name);
        directory_ = std::move(name);
        certificate_ = directory_ / "certificate.pem";
        key_ = directory_ / "private-key.pem";
        std::ofstream(certificate_) << kCertificate;
        std::ofstream(key_) << kPrivateKey;
    }
    ~TestPemFiles() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }
    const std::filesystem::path& certificate() const { return certificate_; }
    const std::filesystem::path& key() const { return key_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path certificate_;
    std::filesystem::path key_;
};

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> output;
    output.reserve(values.size());
    for (const auto value : values) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

std::vector<std::byte> expected_alpn() {
    return bytes({'m', 'o', 'q', 't', '-', '1', '8'});
}

bool terminal_event(const TransportEvent& event) {
    return std::holds_alternative<PeerCloseEvent>(event) ||
           std::holds_alternative<LocalCloseEvent>(event) ||
           std::holds_alternative<IdleTimeoutEvent>(event) ||
           std::holds_alternative<TransportErrorEvent>(event) ||
           std::holds_alternative<EventQueueOverflowEvent>(event);
}

NativeQuicListenerConfig base_config() {
    NativeQuicListenerConfig config;
    config.expected_alpn = expected_alpn();
    config.certificate_path = "/definitely/missing/certificate.pem";
    config.private_key_path = "/definitely/missing/private-key.pem";
    return config;
}

NativeQuicListenerConfig live_config(const TestPemFiles& pem) {
    auto config = base_config();
    config.certificate_path = pem.certificate();
    config.private_key_path = pem.key();
    return config;
}

template <typename Predicate>
bool pump_until(test::PicoquicTestClient& client, Predicate predicate,
                std::chrono::milliseconds limit = std::chrono::seconds{2}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

bool send_raw(std::uint16_t port, std::span<const std::byte> packet) {
    const auto fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto sent = ::sendto(
        fd, packet.data(), packet.size(), 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    ::close(fd);
    return sent == static_cast<ssize_t>(packet.size());
}

TEST(NativeQuicConfiguration, RejectsInvalidValuesBeforePathLoading) {
    struct Case {
        void (*mutate)(NativeQuicListenerConfig&);
    };
    const std::array cases{
        Case{[](auto& value) { value.expected_alpn.clear(); }},
        Case{[](auto& value) { value.expected_alpn = bytes({'m', 'o', 'q'}); }},
        Case{[](auto& value) { value.idle_timeout = std::chrono::milliseconds{0}; }},
        Case{[](auto& value) { value.retry_token_lifetime = std::chrono::seconds{0}; }},
        Case{[](auto& value) { value.max_udp_payload = 1199; }},
        Case{[](auto& value) { value.max_datagrams_per_poll = 0; }},
        Case{[](auto& value) { value.max_egress_datagrams_per_call = 0; }},
        Case{[](auto& value) { value.max_events = 0; }},
        Case{[](auto& value) { value.max_event_payload_bytes = 0; }},
        Case{[](auto& value) {
            value.initial_max_data = (std::uint64_t{1} << 62u);
        }},
        Case{[](auto& value) {
            value.initial_max_streams_bidi = (std::uint64_t{1} << 60u) + 1u;
        }},
        Case{[](auto& value) {
            value.missing_datagram_application_error =
                (std::uint64_t{1} << 62u);
        }},
    };

    for (const auto& item : cases) {
        auto config = base_config();
        item.mutate(config);
        auto result = NativeQuicListener::create(std::move(config));
        ASSERT_EQ(result.listener, nullptr);
        EXPECT_TRUE(result.error == NativeQuicListenerError::InvalidConfiguration ||
                    result.error == NativeQuicListenerError::UnsupportedBindAddress);
    }
}

TEST(NativeQuicConfiguration, ReportsCertificateFailurePrecisely) {
    auto result = NativeQuicListener::create(base_config());
    EXPECT_EQ(result.listener, nullptr);
    EXPECT_EQ(result.error, NativeQuicListenerError::CertificateLoadFailed);
}

TEST(NativeQuicLifecycle, MovedFromListenerIsInert) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    NativeQuicListener moved = std::move(*created.listener);
    EXPECT_TRUE(created.listener->poll(8).empty());
    EXPECT_EQ(created.listener->open_bidi().status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->open_uni().status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->write(0, {}, false).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->reset(0, 1).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->stop_sending(0, 1).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->send_datagram({}).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->close(1, {}).status,
              TransportStatus::InvalidState);
    EXPECT_NE(moved.bound_endpoint().port, 0);
}

TEST(NativeQuicLifecycle, ApplicationOperationsRequireEstablishedState) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    EXPECT_EQ(created.listener->open_bidi().status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->open_uni().status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->write(0, bytes({1}), false).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->reset(0, 1).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->stop_sending(0, 1).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->send_datagram(bytes({1})).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->close(1, bytes({1})).status,
              TransportStatus::InvalidState);
}

TEST(NativeQuicLifecycle, CloseImmediatelyRejectsEveryApplicationOperation) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    const auto stream = created.listener->open_bidi();
    ASSERT_EQ(stream.status, TransportStatus::Success);
    ASSERT_EQ(created.listener->close(61, bytes({1})).status,
              TransportStatus::Success);

    EXPECT_EQ(created.listener->open_bidi().status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->open_uni().status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->write(stream.stream_id, bytes({2}), false).status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->reset(stream.stream_id, 2).status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->stop_sending(stream.stream_id, 2).status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->send_datagram(bytes({2})).status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->close(62, bytes({2})).status,
              TransportStatus::ConnectionClosed);
}

TEST(NativeQuicLive, WildcardUdpBindAcceptsLoopbackPublisher) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.bind_address = "0.0.0.0";
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    EXPECT_EQ(created.listener->bound_endpoint().address, "0.0.0.0");
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(64)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
}

TEST(NativeQuicLive, RetryHandshakeOwnsEvidenceAndDeliversStreamAndDatagram) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);

    std::vector<TransportEvent> events;
    ASSERT_TRUE(pump_until(*client, [&] {
        auto next = created.listener->poll(64);
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        return std::ranges::any_of(events, [](const auto& event) {
            return std::holds_alternative<ConnectionEstablishedEvent>(event);
        });
    }));
    const auto established = std::ranges::find_if(events, [](const auto& event) {
        return std::holds_alternative<ConnectionEstablishedEvent>(event);
    });
    ASSERT_NE(established, events.end());
    const auto& evidence = std::get<ConnectionEstablishedEvent>(*established);
    EXPECT_EQ(evidence.alpn, expected_alpn());
    EXPECT_FALSE(evidence.local_connection_id.empty());
    EXPECT_FALSE(evidence.peer_connection_id.empty());
    EXPECT_GT(evidence.max_datagram_payload, 0u);

    ASSERT_TRUE(client->send_stream(0, bytes({1, 0, 2}), true));
    ASSERT_TRUE(client->send_datagram(bytes({9, 0, 8})));
    bool got_stream = false;
    bool got_datagram = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (auto& event : created.listener->poll(64)) {
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) {
                got_stream = stream->stream_id == 0 && stream->fin &&
                             stream->data == bytes({1, 0, 2});
            }
            if (const auto* datagram = std::get_if<DatagramEvent>(&event)) {
                got_datagram = datagram->data == bytes({9, 0, 8});
            }
        }
        return got_stream && got_datagram;
    }));
}

// A peer that writes more than the advertised unidirectional stream credit is stopped
// there when the listener holds credit, and is not when it does not (RFC 9000 Section 4.1).
std::size_t received_on_peer_uni_stream(bool hold_credit) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.initial_max_stream_data_uni = 100;
    config.hold_uni_stream_credit = hold_credit;
    auto created = NativeQuicListener::create(config);
    if (created.listener == nullptr) return SIZE_MAX;
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port, .alpn = expected_alpn()});
    if (client == nullptr) return SIZE_MAX;
    bool established = false;
    if (!pump_until(*client, [&] {
            for (const auto& event : created.listener->poll(64))
                established |= std::holds_alternative<ConnectionEstablishedEvent>(event);
            return established;
        })) return SIZE_MAX;
    // Client-initiated unidirectional stream 2: 1000 bytes and a FIN.
    if (!client->send_stream(2, std::vector<std::byte>(1000, std::byte{'x'}), true)) return SIZE_MAX;
    std::size_t received = 0;
    bool finished = false;
    (void)pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(64)) {
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) {
                received += stream->data.size();
                finished = finished || stream->fin;
            }
        }
        return finished;
    }, std::chrono::milliseconds{600});
    return received;
}

TEST(NativeQuicLive, HeldUniStreamCreditStopsAPeerStreamAtTheInitialWindow) {
    EXPECT_EQ(received_on_peer_uni_stream(true), 100u);
    EXPECT_EQ(received_on_peer_uni_stream(false), 1000u);
}

// Bidirectional and unidirectional stream credit raised mid-connection through
// grant_peer_streams lets a peer that was held at its first stream open more.
TEST(NativeQuicLive, GrantedStreamCreditLetsThePeerOpenMoreStreams) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.initial_max_streams_bidi = 1;
    config.initial_max_streams_uni = 1;
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    std::set<StreamId> received;
    const auto drain = [&] {
        for (const auto& event : created.listener->poll(64))
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) received.insert(stream->stream_id);
    };
    ASSERT_TRUE(pump_until(*client, [&] { drain(); return client->established(); }));
    const auto payload = bytes({1});
    // Stream 0 (bidi) and 2 (uni) are the first of each kind; the second is over the limit.
    ASSERT_TRUE(client->send_stream(0, payload, true));
    ASSERT_TRUE(client->send_stream(2, payload, true));
    EXPECT_EQ(client->try_send_stream(4, payload, true).status, test::ClientStreamSendStatus::WouldBlock);
    EXPECT_EQ(client->try_send_stream(6, payload, true).status, test::ClientStreamSendStatus::WouldBlock);

    EXPECT_EQ(created.listener->grant_peer_streams(true, 2).status, TransportStatus::Success);
    EXPECT_EQ(created.listener->grant_peer_streams(false, 2).status, TransportStatus::Success);
    bool bidi_sent = false;
    bool uni_sent = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        drain();
        if (!bidi_sent) bidi_sent = client->try_send_stream(4, payload, true).status == test::ClientStreamSendStatus::Success;
        if (!uni_sent) uni_sent = client->try_send_stream(6, payload, true).status == test::ClientStreamSendStatus::Success;
        return bidi_sent && uni_sent;
    }));
    ASSERT_TRUE(pump_until(*client, [&] { drain(); return received.contains(4) && received.contains(6); }));
    // The grant of two covers a third stream of each kind and no more.
    EXPECT_TRUE(client->send_stream(8, payload, true));
    EXPECT_TRUE(client->send_stream(10, payload, true));
    EXPECT_EQ(client->try_send_stream(12, payload, true).status, test::ClientStreamSendStatus::WouldBlock);
    EXPECT_EQ(client->try_send_stream(14, payload, true).status, test::ClientStreamSendStatus::WouldBlock);
}

TEST(NativeQuicLive, InboundDropHidesPeerStreamsUntilReenabled) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    std::set<StreamId> received;
    const auto drain = [&] {
        for (const auto& event : created.listener->poll(64))
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) received.insert(stream->stream_id);
    };
    ASSERT_TRUE(pump_until(*client, [&] { drain(); return client->established(); }));
    ASSERT_EQ(created.listener->set_inbound_drop(true).status, TransportStatus::Success);
    ASSERT_TRUE(client->send_stream(0, bytes({1}), true));
    (void)pump_until(*client, [&] { drain(); return received.contains(0); }, std::chrono::milliseconds{400});
    EXPECT_FALSE(received.contains(0));
    ASSERT_EQ(created.listener->set_inbound_drop(false).status, TransportStatus::Success);
    ASSERT_TRUE(client->send_stream(4, bytes({2}), true));
    EXPECT_TRUE(pump_until(*client, [&] { drain(); return received.contains(4); }));
    EXPECT_FALSE(received.contains(0));
}

TEST(NativeQuicLive, Draft18ControllerCompletesSubscribeResponseOverQuic) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    namespace scenarios = moq::interop::scenarios;
    scenarios::Draft18RunController controller(
        *created.listener,
        scenarios::subscribe_to_publisher_track(
            {{{std::byte{'n'}}}}, {{std::byte{'x'}}}, 1,
            std::chrono::milliseconds(500), std::chrono::milliseconds(20)));

    ASSERT_TRUE(pump_until(*client, [&] {
        controller.poll(scenarios::Clock::now());
        const auto setup = client->stream(3);
        return client->established() && setup && setup->data.size() == 4;
    }));
    EXPECT_EQ(client->stream(3)->data,
              bytes({0xaf, 0x00, 0x00, 0x00}));
    ASSERT_TRUE(client->send_stream(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        controller.poll(scenarios::Clock::now());
        const auto request = client->stream(1);
        return request && request->data.size() == 10;
    }));
    EXPECT_EQ(client->stream(1)->data,
              bytes({0x03, 0x00, 0x07, 0x01, 0x01, 0x01,
                     'n', 0x01, 'x', 0x00}));
    ASSERT_TRUE(controller.context().stimulus_delivered);
    ASSERT_TRUE(client->send_stream(
        1, bytes({0x04, 0x00, 0x04, 0x05, 0x00, 0x02, 0x09}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto state = controller.poll(scenarios::Clock::now());
        return state.status == scenarios::ScenarioStatus::Passed;
    }));
    EXPECT_TRUE(controller.context().complete);
}

TEST(NativeQuicLive, RunManagerScoresAnIsolatedPublisherSession) {
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    requirements::Requirement response_requirement{
        "D18-5.1-MUST-003", requirements::Strength::Must,
        {"5.1", 1936, 1937, 1, 1}, "publisher",
        "exactly one SUBSCRIBE response",
        requirements::Applicability::Applicable,
        requirements::Testability::Testable,
        {"subscribe-to-publisher-track"},
        {"exactly-one-subscribe-ok-or-request-error"}, ""};
    auto catalog = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            18, "test", true, {std::move(response_requirement)}});
    app::NativeRunManager manager(
        catalog, store,
        {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
         .port_start = 0, .port_end = 0,
         .maximum_active_runs = 1, .certificate_path = pem.certificate(),
         .private_key_path = pem.key()});
    const app::RunConfig config{
        app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"subscribe-to-publisher-track"},
        std::chrono::milliseconds(1000), app::TrackFixture{{"n"}, "x"}};
    const auto started = manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    ASSERT_GT(started.endpoint.port, 0u);
    EXPECT_EQ(manager.start(config).status,
              app::RunStartStatus::PortExhausted);

    auto client = test::PicoquicTestClient::create(
        {.port = started.endpoint.port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto setup = client->stream(3);
        return setup && setup->data.size() == 4;
    }));
    ASSERT_TRUE(client->send_stream(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(1);
        return request && request->data.size() == 10;
    }));
    EXPECT_EQ(client->stream(1)->data,
              bytes({0x03, 0x00, 0x07, 0x01, 0x01, 0x01,
                     'n', 0x01, 'x', 0x00}));
    ASSERT_TRUE(client->send_stream(
        1, bytes({0x04, 0x00, 0x04, 0x05, 0x00, 0x02, 0x09}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        return store->load(started.id).state == storage::RunState::Finalized;
    }));
    const auto completed = store->load(started.id);
    ASSERT_EQ(completed.outcomes.size(), 1u);
    EXPECT_EQ(completed.outcomes[0].state,
              requirements::OutcomeState::Pass);
    ASSERT_TRUE(completed.score.has_value());
    EXPECT_EQ(completed.score->verdict, requirements::RunVerdict::Pass);
    EXPECT_FALSE(completed.events.empty());
}

TEST(NativeQuicLive, RunManagerScoresDuplicatePublisherSubscription) {
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    requirements::Requirement duplicate_requirement{
        "D18-5-1-MUST-004", requirements::Strength::Must,
        {"5.1", 1981, 1982, 1, 1}, "endpoint",
        "reject second same-role subscription",
        requirements::Applicability::Applicable,
        requirements::Testability::Testable,
        {"subscribe-again-to-established-publisher-track"},
        {"duplicate-subscription-rejected"}, ""};
    auto catalog = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            18, "test", true, {std::move(duplicate_requirement)}});
    app::NativeRunManager manager(
        catalog, store,
        {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
         .port_start = 0, .port_end = 0,
         .maximum_active_runs = 1, .certificate_path = pem.certificate(),
         .private_key_path = pem.key()});
    const app::RunConfig config{
        app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
        app::RunMode::Observed,
        {"subscribe-again-to-established-publisher-track"},
        std::chrono::milliseconds(1000), app::TrackFixture{{"n"}, "x"}};
    auto too_short = config;
    too_short.timeout = std::chrono::milliseconds(2);
    EXPECT_EQ(manager.start(too_short).status,
              app::RunStartStatus::InvalidConfig);
    const auto started = manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = test::PicoquicTestClient::create(
        {.port = started.endpoint.port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto setup = client->stream(3);
        return setup && setup->data.size() == 4;
    }));
    ASSERT_TRUE(client->send_stream(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(1);
        return request && request->data.size() == 10;
    }));
    ASSERT_TRUE(client->send_stream(
        1, bytes({0x04, 0x00, 0x04, 0x05, 0x00, 0x02, 0x09}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(5);
        return request && request->data.size() == 10;
    }));
    EXPECT_EQ(client->stream(5)->data,
              bytes({0x03, 0x00, 0x07, 0x03, 0x01, 0x01,
                     'n', 0x01, 'x', 0x00}));
    ASSERT_TRUE(client->send_stream(
        5, bytes({0x05, 0x00, 0x03, 0x19, 0x00, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        return store->load(started.id).state == storage::RunState::Finalized;
    }));
    const auto completed = store->load(started.id);
    ASSERT_EQ(completed.outcomes.size(), 1u);
    EXPECT_EQ(completed.outcomes[0].state,
              requirements::OutcomeState::Pass);
    ASSERT_TRUE(completed.score.has_value());
    EXPECT_EQ(completed.score->verdict, requirements::RunVerdict::Pass);
}

TEST(NativeQuicLive, RunManagerScoresPublisherFetchResponse) {
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    requirements::Requirement fetch_requirement{
        "D18-5-2-MUST-001", requirements::Strength::Must,
        {"5.2", 2159, 2160, 1, 1}, "publisher",
        "exactly one FETCH response",
        requirements::Applicability::Applicable,
        requirements::Testability::Testable,
        {"fetch-publisher-track-range"},
        {"exactly-one-fetch-ok-or-request-error"}, ""};
    auto catalog = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            18, "test", true, {std::move(fetch_requirement)}});
    app::NativeRunManager manager(
        catalog, store,
        {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
         .port_start = 0, .port_end = 0,
         .maximum_active_runs = 1, .certificate_path = pem.certificate(),
         .private_key_path = pem.key()});
    const app::RunConfig config{
        app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {"fetch-publisher-track-range"},
        std::chrono::milliseconds(1000), app::TrackFixture{{"n"}, "x"}};
    const auto started = manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto client = test::PicoquicTestClient::create(
        {.port = started.endpoint.port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto setup = client->stream(3);
        return setup && setup->data.size() == 4;
    }));
    ASSERT_TRUE(client->send_stream(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto request = client->stream(1);
        return request && request->data.size() == 15;
    }));
    EXPECT_EQ(client->stream(1)->data,
              bytes({0x16, 0x00, 0x0c, 0x01, 0x01, 0x01, 0x01,
                     'n', 0x01, 'x', 0x00, 0x00, 0x00, 0x01, 0x00}));
    ASSERT_TRUE(client->send_stream(
        1, bytes({0x05, 0x00, 0x03, 0x11, 0x00, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        return store->load(started.id).state == storage::RunState::Finalized;
    }));
    const auto completed = store->load(started.id);
    ASSERT_EQ(completed.outcomes.size(), 1u);
    EXPECT_EQ(completed.outcomes[0].state,
              requirements::OutcomeState::Pass);
    ASSERT_TRUE(completed.score.has_value());
    EXPECT_EQ(completed.score->verdict, requirements::RunVerdict::Pass);
}

TEST(NativeQuicLive, RunManagerScoresNamespaceAndTrackDiscoveryResponses) {
    TestPemFiles pem;
    for (const bool tracks : {false, true}) {
        const std::string scenario = tracks
            ? "subscribe-tracks-at-publisher"
            : "subscribe-namespace-at-publisher";
        requirements::Requirement requirement{
            tracks ? "D18-6-1-MUST-003" : "D18-6-1-MUST-001",
            requirements::Strength::Must,
            {"6.1", 2223, 2225, 1, tracks ? 3u : 1u}, "publisher",
            "single discovery response",
            requirements::Applicability::Applicable,
            requirements::Testability::Testable,
            {scenario},
            {tracks ? "exactly-one-track-subscription-response"
                    : "exactly-one-namespace-subscription-response"}, ""};
        auto catalog = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{
                18, "test", true, {std::move(requirement)}});
        auto store = std::make_shared<storage::SqliteRunStore>(
            ":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(
            catalog, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
             .port_start = 0, .port_end = 0,
             .maximum_active_runs = 1, .certificate_path = pem.certificate(),
             .private_key_path = pem.key()});
        const app::RunConfig config{
            app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
            app::RunMode::Observed, {scenario},
            std::chrono::milliseconds(1000), app::TrackFixture{{"n"}, "x"}};
        const auto started = manager.start(config);
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create(
            {.port = started.endpoint.port, .alpn = expected_alpn()});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto setup = client->stream(3);
            return setup && setup->data.size() == 4;
        }));
        ASSERT_TRUE(client->send_stream(
            2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto request = client->stream(1);
            return request && request->data.size() == 8;
        }));
        EXPECT_EQ(client->stream(1)->data,
                  bytes({tracks ? 0x51u : 0x50u, 0x00, 0x05,
                         0x01, 0x01, 0x01, 'n', 0x00}));
        ASSERT_TRUE(client->send_stream(
            1, bytes({0x05, 0x00, 0x03, 0x11, 0x00, 0x00}), false));
        ASSERT_TRUE(pump_until(*client, [&] {
            return store->load(started.id).state == storage::RunState::Finalized;
        }));
        const auto completed = store->load(started.id);
        ASSERT_EQ(completed.outcomes.size(), 1u);
        EXPECT_EQ(completed.outcomes[0].state,
                  requirements::OutcomeState::Pass);
        ASSERT_TRUE(completed.score.has_value());
        EXPECT_EQ(completed.score->verdict, requirements::RunVerdict::Pass);
    }
}

TEST(NativeQuicLive, HttpRunCreationReturnsUsablePublisherEndpoint) {
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    requirements::Requirement response_requirement{
        "D18-5.1-MUST-003", requirements::Strength::Must,
        {"5.1", 1936, 1937, 1, 1}, "publisher",
        "exactly one SUBSCRIBE response",
        requirements::Applicability::Applicable,
        requirements::Testability::Testable,
        {"subscribe-to-publisher-track"},
        {"exactly-one-subscribe-ok-or-request-error"}, ""};
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            18, "test", true, {std::move(response_requirement)}});
    auto draft21 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{21, "test", true, {}});
    auto runs = std::make_shared<app::NativeRunManager>(
        draft18, store,
        app::NativeRunManagerConfig{
            .bind_address = "0.0.0.0", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0,
            .maximum_active_runs = 1, .certificate_path = pem.certificate(),
            .private_key_path = pem.key()});
    http::HttpServer server(draft18, draft21, store,
                            app::BuildInfo{"test", "test", {}},
                            {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    const auto health = api.Get("/healthz");
    ASSERT_TRUE(health);
    EXPECT_TRUE(nlohmann::json::parse(health->body)
                    .at("executable_profiles").at(0).at("configured"));
    const nlohmann::json request = {
        {"draft", 18}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", nlohmann::json::array({"subscribe-to-publisher-track"})},
        {"timeout_ms", 1000},
        {"track", {{"namespace_hex", nlohmann::json::array({"006e", "ff"})},
                   {"name_hex", "7800"}}}};
    const auto response = api.Post("/api/v1/runs", request.dump(),
                                   "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 201) << response->body;
    const auto created = nlohmann::json::parse(response->body);
    EXPECT_EQ(created.at("publisher_endpoint").at("alpn"), "moqt-18");
    const auto port = created.at("publisher_endpoint").at("port").get<std::uint16_t>();
    const auto id = created.at("run").at("id").get<std::string>();
    ASSERT_TRUE(store->load(id).config.track_fixture.has_value());
    EXPECT_EQ(store->load(id).config.track_fixture->namespace_fields[0],
              std::string("\0n", 2));
    EXPECT_EQ(created.at("run").at("config").at("track"), request.at("track"));
    auto client = test::PicoquicTestClient::create(
        {.port = port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto setup = client->stream(3);
        return setup && setup->data.size() == 4;
    }));
    ASSERT_TRUE(client->send_stream(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
    const bool subscribe_received = pump_until(*client, [&] {
        const auto subscribe = client->stream(1);
        return subscribe && subscribe->data.size() == 14;
    });
    ASSERT_TRUE(subscribe_received)
        << "received stream bytes: "
        << (client->stream(1) ? client->stream(1)->data.size() : 0);
    ASSERT_TRUE(client->send_stream(
        1, bytes({0x04, 0x00, 0x04, 0x05, 0x00, 0x02, 0x09}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        return store->load(id).state == storage::RunState::Finalized;
    }));
    const auto result = api.Get("/api/v1/runs/" + id);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(nlohmann::json::parse(result->body)
                  .at("run").at("score").at("verdict"), "pass");
}

TEST(NativeQuicLive, HttpDraft21AnnouncementPersistsScoredPublisherRun) {
    // draft-ietf-moq-transport-21 sections 6.2, 6.3, 9.3 and 9.8.
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    requirements::Requirement opening{
        "D21-9-MUST-282", requirements::Strength::Must,
        {"9", 3368, 3369, 1, 1}, "publisher",
        "PUBLISH begins its request stream.",
        requirements::Applicability::Applicable,
        requirements::Testability::Testable,
        {"d21-publisher-request-stream-placement"},
        {"d21-publisher-first-message-placement"}, ""};
    auto draft21 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            21, "test", true, {std::move(opening)}});
    auto runs = std::make_shared<app::NativeRunManager>(
        draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0,
            .maximum_active_runs = 1, .certificate_path = pem.certificate(),
            .private_key_path = pem.key()});
    http::HttpServer server(draft18, draft21, store,
                            app::BuildInfo{"test", "test", {}},
                            {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    const nlohmann::json request = {
        {"draft", 21}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", nlohmann::json::array(
            {"d21-publisher-request-stream-placement"})},
        {"timeout_ms", 1000},
        {"track", {{"namespace_hex", nlohmann::json::array({"6d65646961"})},
                   {"name_hex", "74657374"}}}};
    const auto created = api.Post("/api/v1/runs", request.dump(),
                                  "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto body = nlohmann::json::parse(created->body);
    EXPECT_EQ(body.at("publisher_endpoint").at("alpn"), "moqt-21");
    const auto port = body.at("publisher_endpoint").at("port")
                          .get<std::uint16_t>();
    const auto id = body.at("run").at("id").get<std::string>();
    auto client = test::PicoquicTestClient::create(
        {.port = port,
         .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto setup = client->stream(3);
        return setup && setup->data == bytes({0xaf, 0x00, 0x00, 0x00});
    }));
    ASSERT_TRUE(client->send_stream(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
    ASSERT_TRUE(client->send_stream(
        0, bytes({0x1d, 0x00, 0x0f,
                  0x00, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                  0x04, 't', 'e', 's', 't', 0x02, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        const auto response = client->stream(0);
        return response && response->data == bytes({0x07, 0x00, 0x01, 0x00}) &&
               store->load(id).state == storage::RunState::Finalized;
    }));
    const auto result = api.Get("/api/v1/runs/" + id);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->status, 200);
    const auto run = nlohmann::json::parse(result->body).at("run");
    EXPECT_EQ(run.at("config").at("draft"), 21);
    EXPECT_EQ(run.at("score").at("verdict"), "pass");
    ASSERT_EQ(run.at("outcomes").size(), 1u);
    EXPECT_EQ(run.at("outcomes").at(0).at("state"), "pass");
    EXPECT_FALSE(store->load(id).events.empty());
}

TEST(NativeQuicLive, HttpDraft21InvalidRequestOpenerFailsMustNot) {
    // draft-ietf-moq-transport-21 section 6.3: only seven request openers.
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    requirements::Requirement opening{
        "D21-6-3-MUST-NOT-141", requirements::Strength::MustNot,
        {"6.3", 2111, 2114, 1, 1}, "publisher",
        "Only permitted messages start a request stream.",
        requirements::Applicability::Applicable,
        requirements::Testability::Testable,
        {"d21-publisher-request-stream-placement"},
        {"d21-request-stream-first-message-allowed"}, ""};
    auto draft21 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{21, "test", true, {std::move(opening)}});
    auto runs = std::make_shared<app::NativeRunManager>(
        draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0,
            .maximum_active_runs = 1, .certificate_path = pem.certificate(),
            .private_key_path = pem.key()});
    http::HttpServer server(draft18, draft21, store,
                            app::BuildInfo{"test", "test", {}},
                            {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    const nlohmann::json request = {
        {"draft", 21}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", nlohmann::json::array(
            {"d21-publisher-request-stream-placement"})},
        {"timeout_ms", 1000},
        {"track", {{"namespace_hex", nlohmann::json::array({"6d65646961"})},
                   {"name_hex", "74657374"}}}};
    const auto created = api.Post("/api/v1/runs", request.dump(),
                                  "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto body = nlohmann::json::parse(created->body);
    const auto port = body.at("publisher_endpoint").at("port")
                          .get<std::uint16_t>();
    const auto id = body.at("run").at("id").get<std::string>();
    auto client = test::PicoquicTestClient::create(
        {.port = port,
         .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(pump_until(*client, [&] {
        return client->stream(3).has_value();
    }));
    ASSERT_TRUE(client->send_stream(0, bytes({0x1e, 0x00, 0x00}), false));
    ASSERT_TRUE(pump_until(*client, [&] {
        return client->peer_close().has_value() &&
               store->load(id).state == storage::RunState::Finalized;
    }));
    EXPECT_EQ(client->peer_close()->error_code, 0x3u);
    const auto result = api.Get("/api/v1/runs/" + id);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->status, 200);
    const auto run = nlohmann::json::parse(result->body).at("run");
    EXPECT_EQ(run.at("score").at("verdict"), "fail");
    ASSERT_EQ(run.at("outcomes").size(), 1u);
    EXPECT_EQ(run.at("outcomes").at(0).at("state"), "fail");
    const auto stored = store->load(id);
    EXPECT_TRUE(std::any_of(
        stored.events.begin(), stored.events.end(),
        [](const storage::EvidenceEvent& event) {
            return event.kind == "invalid_request_opener" &&
                   event.stream_id == "0";
        }));
}

TEST(NativeQuicLive, RawCloseProbesRouteBothDraftsWithoutTrackAndPersistEvidence) {
    TestPemFiles pem;
    for (const unsigned draft : {18u, 21u}) {
        SCOPED_TRACE(draft);
        const std::string scenario = draft == 18 ? "receive-unknown-message-type" : "d21-unknown-control-message";
        const std::string requirement_id = draft == 18 ? "D18-10-MUST-008" : "D21-9-MUST-284";
        const std::string evaluator = draft == 18 ? "session-closed" : "d21-unknown-message-session-close";
        requirements::Requirement row{requirement_id, requirements::Strength::Must,
            {"test", 1, 1, 1, 1}, "receiver", "close on unknown message",
            requirements::Applicability::Applicable, requirements::Testability::Testable,
            {scenario}, {evaluator}, ""};
        auto d18 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{18, "test", true,
                draft == 18 ? std::vector{row} : std::vector<requirements::Requirement>{}});
        auto d21 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{21, "test", true,
                draft == 21 ? std::vector{row} : std::vector<requirements::Requirement>{}});
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(d18, d21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
             .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
             .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const app::RunConfig config{static_cast<app::DraftVersion>(draft),
            app::TransportKind::NativeQuic, app::RunMode::Observed, {scenario},
            std::chrono::milliseconds(1000), std::nullopt};
        const auto started = manager.start(config);
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
            .alpn = draft == 18 ? expected_alpn() : bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto setup = client->stream(3);
            return setup && setup->data.size() == 4;
        }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto control = client->stream(3);
            return control && control->data.size() == 7;
        }));
        EXPECT_EQ(client->stream(3)->data,
            bytes({0xaf, 0, 0, 0, draft == 18 ? 0x3fu : 0x7eu, 0, 0}));
        ASSERT_TRUE(client->close(3, {}));
        ASSERT_TRUE(pump_until(*client, [&] {
            return store->load(started.id).state == storage::RunState::Finalized;
        }));
        const auto run = store->load(started.id);
        ASSERT_EQ(run.outcomes.size(), 1u);
        EXPECT_EQ(run.outcomes.front().state, requirements::OutcomeState::Pass);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_stimulus" && event.detail.find("accepted=3") != std::string::npos;
        }));
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "peer_close" && event.detail.find("application close code=3") != std::string::npos;
        }));
    }
}

TEST(NativeQuicLive, PeerResponseProbesWaitForMatchingPublisherRequestOnActualStream) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    for (const unsigned draft : {18u, 21u}) {
        SCOPED_TRACE(draft);
        const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        auto checked = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source, root / "requirements" / (draft == 18 ? "draft18.json" : "draft21.json")));
        auto empty = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{draft == 18 ? 21u : 18u, "test", true, {}});
        const std::string scenario = draft == 18 ? "receive-reason-phrase-length-over-1024" : "d21-publish-request-error-oversized-reason";
        const std::string requirement = draft == 18 ? "D18-1-4-4-MUST-001" : "D21-8-5-MUST-248";
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(draft == 18 ? checked : empty, draft == 21 ? checked : empty, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
             .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({static_cast<app::DraftVersion>(draft), app::TransportKind::NativeQuic,
            app::RunMode::Observed, {scenario}, std::chrono::milliseconds(1500), std::nullopt});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
            .alpn = draft == 18 ? expected_alpn() : bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3); return setup && setup->data.size() == 4; }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        ASSERT_TRUE(client->send_stream(0, bytes({6, 0, 5, 0, 1, 1, 'n', 0}), false));
        // A different complete opener must not select stream 0. The matching
        // request is deliberately fragmented on another client-owned stream.
        ASSERT_TRUE(client->send_stream(4, bytes({0x1d, 0}), false));
        ASSERT_TRUE(client->pump());
        EXPECT_EQ(store->load(started.id).state, storage::RunState::Active);
        ASSERT_TRUE(client->send_stream(4, draft == 18 ? bytes({8, 2, 1, 1, 'n', 1, 'x', 0, 0}) :
                                                       bytes({10, 2, 1, 1, 'n', 1, 'x', 0, 0, 4, 1}), false));
        ASSERT_TRUE(pump_until(*client, [&] { const auto response = client->stream(4); return response && response->data.size() == 1032; }));
        auto expected = draft == 18 ? bytes({5, 4, 5, 0x20, 0, 0x84, 1}) : bytes({5, 4, 5, 0, 0, 0x84, 1});
        expected.insert(expected.end(), 1025, draft == 18 ? std::byte{'a'} : std::byte{'x'});
        EXPECT_EQ(client->stream(4)->data, expected);
        const auto unrelated = client->stream(0);
        EXPECT_TRUE(!unrelated || unrelated->data.empty());
        ASSERT_TRUE(client->close(3, {}));
        ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        const auto run = store->load(started.id);
        const auto outcome = std::find_if(run.outcomes.begin(), run.outcomes.end(), [&](const auto& row) { return row.requirement_id == requirement; });
        ASSERT_NE(outcome, run.outcomes.end());
        EXPECT_EQ(outcome->state, requirements::OutcomeState::Pass);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_stimulus" && event.detail.find("stream=4") != std::string::npos &&
                   event.detail.find("accepted=1032") != std::string::npos;
        }));
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_transport_event" && event.stream_id == "4";
        }));
    }
}

TEST(NativeQuicLive, PeerUpdateProbesEstablishPublishBeforeRespondingToActualUpdate) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    for (const unsigned draft : {18u, 21u}) {
        SCOPED_TRACE(draft);
        const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        auto checked = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source, root / "requirements" / (draft == 18 ? "draft18.json" : "draft21.json")));
        auto empty = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{draft == 18 ? 21u : 18u, "test", true, {}});
        const std::string scenario = draft == 18 ? "receive-request-update-ok-with-track-properties" : "d21-publish-update-ok-with-track-properties";
        const std::string requirement = draft == 18 ? "D18-10-5-MUST-002" : "D21-9-3-MUST-337";
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(draft == 18 ? checked : empty, draft == 21 ? checked : empty, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
             .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({static_cast<app::DraftVersion>(draft), app::TransportKind::NativeQuic,
            app::RunMode::Observed, {scenario}, std::chrono::milliseconds(1500), std::nullopt});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
            .alpn = draft == 18 ? expected_alpn() : bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3); return setup && setup->data.size() == 4; }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        ASSERT_TRUE(client->send_stream(4, draft == 18 ? bytes({0x1d, 0, 8, 0, 1, 1, 'n', 1, 'x', 0, 0}) :
                                                       bytes({0x1d, 0, 10, 0, 1, 1, 'n', 1, 'x', 0, 0, 4, 1}), false));
        ASSERT_TRUE(pump_until(*client, [&] { const auto response = client->stream(4); return response && response->data.size() == 4; }));
        EXPECT_EQ(client->stream(4)->data, bytes({7, 0, 1, 0}));
        ASSERT_TRUE(client->send_stream(4, bytes({2, 0}), false));
        ASSERT_TRUE(client->pump());
        EXPECT_EQ(store->load(started.id).state, storage::RunState::Active);
        EXPECT_EQ(client->stream(4)->data.size(), 4u);
        ASSERT_TRUE(client->send_stream(4, bytes({2, 2, 0}), false));
        ASSERT_TRUE(pump_until(*client, [&] { const auto response = client->stream(4); return response && response->data.size() == 10; }));
        EXPECT_EQ(client->stream(4)->data, draft == 18 ? bytes({7, 0, 1, 0, 7, 0, 3, 0, 0x22, 1}) :
                                                                  bytes({7, 0, 1, 0, 7, 0, 3, 0, 4, 1}));
        ASSERT_TRUE(client->close(3, {}));
        ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        const auto run = store->load(started.id);
        const auto outcome = std::find_if(run.outcomes.begin(), run.outcomes.end(), [&](const auto& row) { return row.requirement_id == requirement; });
        ASSERT_NE(outcome, run.outcomes.end());
        // Draft21 also requires the two initial-OK contexts in its shared row.
        EXPECT_EQ(outcome->state, draft == 18 ? requirements::OutcomeState::Pass : requirements::OutcomeState::NotRun);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_stimulus" && event.detail.find("stream=4") != std::string::npos &&
                   event.detail.find("accepted=6") != std::string::npos;
        }));
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_transport_event" && event.stream_id == "4";
        }));
    }
}

TEST(NativeQuicLive, EstablishedServerUpdateProbesUseActualResponseAndSameRequestStream) {
    TestPemFiles pem;
    struct Case {
        const char* scenario;
        const char* requirement;
        const char* evaluator;
        bool discovery;
        bool duplicate;
        std::vector<std::byte> update;
    };
    for (const auto& fixture : std::vector<Case>{
        {"d21-group-order-in-subscription-update", "D21-9-20-1-MUST-404",
         "d21-out-of-scope-parameter-protocol-violation", false, false, bytes({2, 0, 4, 3, 1, 0x22, 1})},
        {"d21-discovery-update-invalid-forward", "D21-9-20-19-MUST-460",
         "d21-forward-bounds-protocol-violation", true, false, bytes({2, 0, 4, 3, 1, 0x10, 255})},
        {"d21-duplicate-request-update-id", "D21-6-4-2-1-MUST-155",
         "d21-duplicate-invalid-request-id", false, true, bytes({2, 0, 2, 3, 0})}}) {
        SCOPED_TRACE(fixture.scenario);
        // A leaf catalog isolates runtime routing; separate catalog evaluator
        // tests require every real family's named context before its row passes.
        requirements::Requirement row{fixture.requirement, requirements::Strength::Must,
            {"test", 1, 1, 1, 1}, "receiver", "reject the established request update",
            requirements::Applicability::Applicable, requirements::Testability::Testable,
            {fixture.scenario}, {fixture.evaluator}, ""};
        auto d18 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{18, "test", true, {}});
        auto d21 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{21, "test", true, {row}});
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(d18, d21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
             .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
            app::RunMode::Observed, {fixture.scenario}, std::chrono::milliseconds(1500), std::nullopt});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
            .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3); return setup && setup->data.size() == 4; }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        const auto initial = fixture.discovery ? bytes({0x51, 0, 3, 1, 0, 0}) : bytes({3, 0, 5, 1, 0, 1, 'x', 0});
        ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1); return request && request->data.size() == initial.size(); }));
        EXPECT_EQ(client->stream(1)->data, initial);
        // Both responses exercise legal EXPIRES. SUBSCRIBE_OK additionally
        // carries a Location parameter and MAX_CACHE_DURATION Track Property.
        const auto response = fixture.discovery ? bytes({7, 0, 3, 1, 8, 10}) :
            bytes({4, 0, 9, 0, 2, 8, 10, 1, 0, 0, 4, 1});
        ASSERT_TRUE(client->send_stream(1, std::span(response).first(3), false));
        ASSERT_TRUE(client->pump());
        EXPECT_EQ(client->stream(1)->data, initial);
        ASSERT_TRUE(client->send_stream(1, std::span(response).subspan(3), false));
        auto expected = initial;
        expected.insert(expected.end(), fixture.update.begin(), fixture.update.end());
        ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1); return request && request->data.size() == expected.size(); }));
        EXPECT_EQ(client->stream(1)->data, expected);
        if (fixture.duplicate) {
            EXPECT_EQ(store->load(started.id).state, storage::RunState::Active);
            ASSERT_TRUE(client->send_stream(1, bytes({7, 0, 3, 1, 8, 10}), false));
            expected.insert(expected.end(), fixture.update.begin(), fixture.update.end());
            ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1); return request && request->data.size() == expected.size(); }));
            EXPECT_EQ(client->stream(1)->data, expected);
        }
        ASSERT_TRUE(client->close(fixture.duplicate ? 4 : 3, {}));
        ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        const auto run = store->load(started.id);
        ASSERT_EQ(run.outcomes.size(), 1u);
        EXPECT_EQ(run.outcomes.front().state, requirements::OutcomeState::Pass);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_stimulus" && event.detail.find("accepted_event_count=") != std::string::npos;
        }));
        const auto unrelated = client->stream(5);
        EXPECT_TRUE(!unrelated || unrelated->data.empty());
    }
}

TEST(NativeQuicLive, ResponseAndFailedUpdateCleanupProbesRouteWithoutTrack) {
    TestPemFiles pem;
    struct Case {
        const char* scenario;
        const char* requirement;
        const char* evaluator;
        unsigned opener;
    };
    for (const auto& fixture : std::vector<Case>{
        {"d21-responder-update-on-publish-namespace", "D21-9-5-MUST-344", "d21-request-update-context-and-direction", 6},
        {"d21-subscriber-update-on-publish", "D21-9-5-MUST-344", "d21-request-update-context-and-direction", 0x1d},
        {"d21-failed-subscription-update-cleanup", "D21-9-5-1-MUST-346", "d21-failed-update-publish-done-update-failed", 3},
        {"d21-failed-subscribe-namespace-update-close", "D21-9-5-1-MUST-348", "d21-failed-namespace-update-stream-close", 0x50},
        {"d21-failed-subscribe-tracks-update-close", "D21-9-5-1-MUST-349", "d21-failed-subscribe-tracks-update-stream-close", 0x51}}) {
        SCOPED_TRACE(fixture.scenario);
        requirements::Requirement row{fixture.requirement, requirements::Strength::Must,
            {"test", 1, 1, 1, 1}, "receiver", "handle an established request update",
            requirements::Applicability::Applicable, requirements::Testability::Testable,
            {fixture.scenario}, {fixture.evaluator}, ""};
        auto d18 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{18, "test", true, {}});
        auto d21 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{21, "test", true, {row}});
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(d18, d21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
             .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
            app::RunMode::Observed, {fixture.scenario}, std::chrono::milliseconds(1500), std::nullopt});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
            .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3); return setup && setup->data.size() == 4; }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        const bool publish = fixture.opener == 0x1d;
        const bool forbidden = fixture.opener == 6;
        const bool peer_request = publish || forbidden;
        const std::uint64_t stream = peer_request ? 4 : 1;
        std::vector<std::byte> initial;
        if (peer_request) {
            const auto opening = publish ? bytes({0x1d, 0, 10, 0, 1, 1, 'n', 1, 'x', 0, 0, 4, 1})
                : bytes({6, 0, 5, 0, 1, 1, 'n', 0});
            ASSERT_TRUE(client->send_stream(4, opening, false));
            initial = bytes({7, 0, 1, 0});
        } else {
            initial = fixture.opener == 3 ? bytes({3, 0, 5, 1, 0, 1, 'x', 0})
                : bytes({fixture.opener, 0, 3, 1, 0, 0});
            ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(stream); return request && request->data.size() == initial.size(); }));
            EXPECT_EQ(client->stream(stream)->data, initial);
            auto reply = fixture.opener == 3 ? bytes({4, 0, 4, 0, 0, 4, 1}) : bytes({7, 0, 1, 0});
            if (fixture.opener == 0x50) {
                const auto announcement = bytes({8, 0, 3, 1, 1, 'n'});
                reply.insert(reply.end(), announcement.begin(), announcement.end());
            }
            ASSERT_TRUE(client->send_stream(stream, std::span(reply).first(3), false));
            ASSERT_TRUE(client->pump());
            ASSERT_TRUE(client->send_stream(stream, std::span(reply).subspan(3), false));
        }
        auto expected = initial;
        const auto update = peer_request ? bytes({2, 0, 2, 1, 0}) : bytes({2, 0, 6, 3, 1, 3, 2, 2, 0});
        expected.insert(expected.end(), update.begin(), update.end());
        ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(stream);
            return request && request->data.size() == expected.size() && (peer_request || request->fin); }));
        EXPECT_EQ(client->stream(stream)->data, expected);
        if (forbidden) {
            ASSERT_TRUE(client->close(3, {}));
        } else {
            auto reply = publish ? bytes({7, 0, 1, 0}) : bytes({5, 0, 3, 1, 0, 0});
            if (fixture.opener == 3) {
                const auto done = bytes({0x0b, 0, 3, 8, 0, 0});
                reply.insert(reply.end(), done.begin(), done.end());
            }
            ASSERT_TRUE(client->send_stream(stream, std::span(reply).first(2), false));
            ASSERT_TRUE(client->pump());
            ASSERT_TRUE(client->send_stream(stream, std::span(reply).subspan(2), !publish));
        }
        ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        const auto run = store->load(started.id);
        ASSERT_EQ(run.outcomes.size(), 1u);
        EXPECT_EQ(run.outcomes.front().state, requirements::OutcomeState::Pass);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "raw_probe_transport_event" && event.stream_id == std::to_string(stream);
        }));
        EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "compatibility_error_mapping" || event.kind == "unresolved_error_mapping";
        }));
    }
}

TEST(NativeQuicLive, Draft18FailedUpdateCleanupProducesActualCatalogOutcomes) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(18, root / "docs", root / "requirements/draft-digests.json");
    auto d18 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / "requirements/draft18.json"));
    auto d21 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{21, "fixture", true, {}});
    struct Case { const char* scenario; const char* requirement; bool subscription; };
    for (const auto& fixture : std::vector<Case>{
        {"reject-subscription-request-update", "D18-10-9-1-MUST-001", true},
        {"reject-subscribe-namespace-request-update", "D18-10-9-1-MUST-003", false}}) {
        SCOPED_TRACE(fixture.scenario);
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(d18, d21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
             .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
            app::RunMode::Observed, {fixture.scenario}, std::chrono::milliseconds(1500), std::nullopt});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port, .alpn = expected_alpn()});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3); return setup && setup->data.size() == 4; }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        const auto opening = fixture.subscription ? bytes({3, 0, 5, 1, 0, 1, 'x', 0}) : bytes({0x50, 0, 3, 1, 0, 0});
        ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1); return request && request->data.size() == opening.size(); }));
        EXPECT_EQ(client->stream(1)->data, opening);
        auto success = fixture.subscription ? bytes({4, 0, 4, 0, 0, 4, 1}) : bytes({7, 0, 1, 0});
        if (!fixture.subscription) {
            const auto announcement = bytes({8, 0, 3, 1, 1, 'n'});
            success.insert(success.end(), announcement.begin(), announcement.end());
        }
        ASSERT_TRUE(client->send_stream(1, std::span(success).first(3), false));
        ASSERT_TRUE(client->pump());
        ASSERT_TRUE(client->send_stream(1, std::span(success).subspan(3), false));
        auto expected = opening;
        const auto update = bytes({2, 0, 6, 3, 1, 3, 2, 2, 0});
        expected.insert(expected.end(), update.begin(), update.end());
        ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
            return request && request->data.size() == expected.size() && request->fin; }));
        EXPECT_EQ(client->stream(1)->data, expected);
        auto reply = bytes({5, 0, 3, 1, 0, 0});
        if (fixture.subscription) {
            const auto done = bytes({0x0b, 0, 3, 8, 0, 0});
            reply.insert(reply.end(), done.begin(), done.end());
        }
        ASSERT_TRUE(client->send_stream(1, std::span(reply).first(2), false));
        ASSERT_TRUE(client->pump());
        ASSERT_TRUE(client->send_stream(1, std::span(reply).subspan(2), true));
        ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        const auto run = store->load(started.id);
        const auto outcome = std::find_if(run.outcomes.begin(), run.outcomes.end(), [&](const auto& item) {
            return item.requirement_id == fixture.requirement;
        });
        ASSERT_NE(outcome, run.outcomes.end());
        EXPECT_EQ(outcome->state, requirements::OutcomeState::Pass);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_stimulus" && event.detail.find("accepted_event_count=") != std::string::npos;
        }));
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_transport_event" && event.stream_id == "1";
        }));
        EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "compatibility_error_mapping" || event.kind == "unresolved_error_mapping";
        }));
        const auto unrelated = client->stream(5);
        EXPECT_TRUE(!unrelated || unrelated->data.empty());
    }
}

TEST(NativeQuicLive, Draft21RangeFiltersUseActualPublisherCapacityAndEstablishedUpdates) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    auto d21 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / "requirements/draft21.json"));
    auto d18 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{18, "fixture", true, {}});
    struct Case { const char* scenario; const char* requirement; unsigned capacity; bool update; bool omitted; };
    for (const auto& fixture : std::vector<Case>{
        {"d21-range-filter-total-limit", "D21-9-1-6-MUST-315", 2, false, false},
        {"d21-range-filter-default-zero-limit", "D21-9-1-6-MUST-315", 0, false, true},
        {"d21-range-filter-update-total-limit", "D21-9-1-6-MUST-315", 1, true, false},
        {"d21-range-filter-update-total-limit", "D21-9-1-6-MUST-315", 2, true, false},
        {"d21-range-filter-update-total-limit", "D21-9-1-6-MUST-315", 16, true, false},
        {"d21-range-filter-total-exceeds-negotiated-limit", "D21-3-3-2-MUST-065", 1, false, false},
        {"d21-range-filter-total-exceeds-negotiated-limit", "D21-3-3-2-MUST-065", 2, false, false},
        {"d21-range-filter-total-exceeds-negotiated-limit", "D21-3-3-2-MUST-065", 16, false, false},
        {"d21-range-filter-with-zero-negotiated-limit", "D21-3-3-2-MUST-065", 0, false, false},
        {"d21-range-filter-with-zero-negotiated-limit", "D21-3-3-2-MUST-065", 0, false, true},
        {"d21-duplicate-range-filter-key-in-update", "D21-3-3-2-MUST-064", 2, true, false},
        {"d21-duplicate-range-filter-key-in-update", "D21-3-3-2-MUST-064", 16, true, false}}) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            SCOPED_TRACE(std::string(fixture.scenario) + " cap=" + std::to_string(fixture.capacity) + " mode=" + std::to_string(mode));
            auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
            app::NativeRunManager manager(d18, d21, store,
                {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
                 .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
            EXPECT_EQ(manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic, app::RunMode::Observed,
                {fixture.scenario}, std::chrono::milliseconds(1000), std::nullopt}).status, app::RunStartStatus::InvalidConfig);
            const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic, app::RunMode::Observed,
                {fixture.scenario}, std::chrono::milliseconds(1000), app::TrackFixture{{"n"}, "t"}});
            ASSERT_EQ(started.status, app::RunStartStatus::Started);
            auto client = test::PicoquicTestClient::create({.port = started.endpoint.port, .alpn = bytes({'m','o','q','t','-','2','1'})});
            ASSERT_NE(client, nullptr);
            ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3); return setup && setup->data == bytes({0xaf,0,0,0}); }));
            const auto setup = fixture.omitted ? bytes({0xaf,0,0,0}) : bytes({0xaf,0,0,2,6,fixture.capacity});
            ASSERT_TRUE(client->send_stream(2, std::span(setup).first(3), false));
            ASSERT_TRUE(client->pump());
            const auto early = client->stream(1);
            EXPECT_TRUE(!early || early->data.empty());
            ASSERT_TRUE(client->send_stream(2, std::span(setup).subspan(3), false));
            const bool retained = std::string_view(fixture.scenario) == "d21-range-filter-update-total-limit";
            auto body = bytes({1,1,1,'n',1,'t',retained ? 1u : fixture.update ? 0u : fixture.capacity == 0 ? 1u : 2u});
            if (!fixture.update || retained) {
                if (fixture.capacity == 0) {
                    const auto parameter = bytes({0x26,3,0,1,0});
                    body.insert(body.end(), parameter.begin(), parameter.end());
                } else {
                    const auto prefix = bytes({0x26,1 + 2 * fixture.capacity,0});
                    body.insert(body.end(), prefix.begin(), prefix.end());
                    for (unsigned range = 0; range < fixture.capacity; ++range) {
                        body.push_back(std::byte{1}); body.push_back(std::byte{0});
                    }
                    if (!retained) {
                        const auto parameter = bytes({0,3,1,1,0});
                        body.insert(body.end(), parameter.begin(), parameter.end());
                    }
                }
            }
            auto opening = bytes({3,0,static_cast<unsigned>(body.size())});
            opening.insert(opening.end(), body.begin(), body.end());
            ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
                return request && request->data.size() == opening.size() && request->fin == !fixture.update; }));
            EXPECT_EQ(client->stream(1)->data, opening);
            auto expected_request = opening;
            if (fixture.update) {
                ASSERT_TRUE(client->send_stream(1, bytes({4,0,2,7}), false));
                ASSERT_TRUE(client->pump());
                EXPECT_EQ(client->stream(1)->data, opening);
                ASSERT_TRUE(client->send_stream(1, bytes({0}), false));
                const auto update = retained ? bytes({2,0,7,3,1,0x26,3,1,1,0}) : bytes({2,0,12,3,2,0x26,3,0,1,0,0,3,0,1,0});
                expected_request.insert(expected_request.end(), update.begin(), update.end());
                ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
                    return request && request->data == expected_request && request->fin; }));
            }
            auto reply = mode == 2 ? fixture.update ? bytes({7,0,1,0}) : bytes({4,0,2,7,0})
                : bytes({5,0,3,mode == 0 ? 0x36u : 0x10u,0,0});
            if (fixture.update && mode != 2) {
                const auto done = bytes({0x0b,0,3,8,0,0}); reply.insert(reply.end(), done.begin(), done.end());
            } else if (!fixture.update && mode == 2) {
                const auto done = bytes({0x0b,0,3,0,0,0}); reply.insert(reply.end(), done.begin(), done.end());
            }
            ASSERT_TRUE(client->send_stream(1, std::span(reply).first(2), false));
            ASSERT_TRUE(client->pump());
            ASSERT_TRUE(client->send_stream(1, std::span(reply).subspan(2), true));
            ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
            const auto run = store->load(started.id);
            const auto outcome = std::find_if(run.outcomes.begin(), run.outcomes.end(), [&](const auto& row) { return row.requirement_id == fixture.requirement; });
            ASSERT_NE(outcome, run.outcomes.end());
            // The other named catalogue context is absent from this run.
            EXPECT_EQ(outcome->state, mode == 0 ? requirements::OutcomeState::NotRun : requirements::OutcomeState::Fail);
            const auto hex = [](std::span<const std::byte> input) {
                constexpr char digits[] = "0123456789abcdef"; std::string output;
                for (const auto value : input) { const auto octet = std::to_integer<unsigned>(value); output += digits[octet >> 4]; output += digits[octet & 15]; }
                return output;
            };
            const auto stimulus = std::find_if(run.events.begin(), run.events.end(), [](const auto& event) { return event.kind == "raw_probe_stimulus"; });
            ASSERT_NE(stimulus, run.events.end());
            EXPECT_NE(stimulus->detail.find("bytes=" + hex(opening)), std::string::npos);
            if (!fixture.update || retained) {
                const auto opening_bytes = stimulus->detail.find(" bytes=" + hex(opening) + " accepted_event_count=");
                ASSERT_NE(opening_bytes, std::string::npos);
                const auto marker = stimulus->detail.find(" prepared_event_count=", opening_bytes);
                ASSERT_NE(marker, std::string::npos);
                const auto next_write = stimulus->detail.find(" stream=", opening_bytes);
                EXPECT_TRUE(next_write == std::string::npos || marker < next_write);
                const auto value_start = marker + std::string(" prepared_event_count=").size();
                const auto value_end = stimulus->detail.find(' ', value_start);
                const auto prepared = stimulus->detail.substr(value_start, value_end - value_start);
                ASSERT_FALSE(prepared.empty());
                EXPECT_TRUE(std::all_of(prepared.begin(), prepared.end(), [](char digit) {
                    return digit >= '0' && digit <= '9';
                }));
            } else {
                EXPECT_NE(stimulus->detail.find("bytes=02000c030226030001000003000100"), std::string::npos);
            }
            if (retained) {
                EXPECT_NE(stimulus->detail.find("fin=true bytes=02000703012603010100"),std::string::npos);
            }
            EXPECT_NE(stimulus->detail.find("fin=true bytes="), std::string::npos);
            std::string saved_reply;
            for (const auto& event : run.events) {
                if (event.kind != "raw_probe_transport_event" || event.stream_id != "1") continue;
                const auto marker = event.detail.find(" bytes="); if (marker == std::string::npos) continue;
                const auto first = marker + 7; saved_reply += event.detail.substr(first, event.detail.find(' ', first) - first);
            }
            const auto incoming_prefix = fixture.update ? hex(bytes({4,0,2,7,0})) : std::string{};
            EXPECT_EQ(saved_reply, incoming_prefix + hex(reply));
        }
    }
}

TEST(NativeQuicLive, Draft21SubscriptionAndDiscoveryResponseFamiliesUseActualStreams) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    auto d21 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / "requirements/draft21.json"));
    auto d18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "fixture", true, {}});
    struct Case { const char* scenario; const char* requirement; unsigned type; bool accepted; };
    for (const auto& fixture : std::vector<Case>{
        {"d21-subscribe-accepted", "D21-3-1-MUST-033", 3, true},
        {"d21-subscribe-rejected", "D21-3-1-MUST-033", 3, false},
        {"d21-subscribe-namespace-accepted", "D21-4-1-MUST-082", 0x50, true},
        {"d21-subscribe-namespace-rejected", "D21-4-1-MUST-082", 0x50, false},
        {"d21-subscribe-tracks-accepted", "D21-4-1-MUST-083", 0x51, true},
        {"d21-subscribe-tracks-rejected", "D21-4-1-MUST-083", 0x51, false}}) {
        // Singleton, duplicate, empty FIN, RESET without FIN, and wrong first
        // discovery message exercise distinct runtime proof boundaries.
        for (unsigned mode = 0; mode < (fixture.type == 3 ? 4u : 5u); ++mode) {
            SCOPED_TRACE(std::string(fixture.scenario) + " mode " + std::to_string(mode));
            auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
            app::NativeRunManager manager(d18, d21, store,
                {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
                 .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
            EXPECT_EQ(manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
                app::RunMode::Observed, {fixture.scenario}, std::chrono::milliseconds(1000), std::nullopt}).status,
                app::RunStartStatus::InvalidConfig);
            const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
                app::RunMode::Observed, {fixture.scenario}, std::chrono::milliseconds(1000), app::TrackFixture{{"n"}, "t"}});
            ASSERT_EQ(started.status, app::RunStartStatus::Started);
            auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
                .alpn = bytes({'m','o','q','t','-','2','1'})});
            ASSERT_NE(client, nullptr);
            ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3);
                return setup && setup->data == bytes({0xaf,0,0,0}); }));
            ASSERT_TRUE(client->send_stream(2, bytes({0xaf,0,0,0}), false));
            const auto opening = fixture.type == 3 ? bytes({3,0,7,1,1,1,'n',1,'t',0})
                : bytes({fixture.type,0,5,1,1,1,'n',0});
            ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
                return request && request->data.size() == opening.size() && request->fin; }));
            EXPECT_EQ(client->stream(1)->data, opening);
            const auto hex = [](std::span<const std::byte> input) {
                constexpr char digits[] = "0123456789abcdef";
                std::string result;
                for (const auto value : input) {
                    const auto octet = std::to_integer<unsigned>(value);
                    result += digits[octet >> 4]; result += digits[octet & 15];
                }
                return result;
            };
            const auto saved_reply_hex = [](const auto& run) {
                std::string result;
                for (const auto& event : run.events) {
                    if (event.kind != "raw_probe_transport_event" || event.stream_id != "1") continue;
                    const auto start = event.detail.find(" bytes=");
                    if (start == std::string::npos) continue;
                    const auto first = start + 7;
                    result += event.detail.substr(first, event.detail.find(' ', first) - first);
                }
                return result;
            };
            const auto reply = !fixture.accepted ? bytes({5,0,3,1,0,0}) : fixture.type == 3
                ? bytes({4,0,2,7,0}) : bytes({7,0,1,0});
            std::vector<std::byte> sent;
            if (mode == 4) {
                sent = bytes({8,0,3,1,1,'n'});
                ASSERT_TRUE(client->send_stream(1, sent, false));
            } else {
                if (mode != 2) {
                    sent = reply;
                    ASSERT_TRUE(client->send_stream(1, std::span(reply).first(2), false));
                    ASSERT_TRUE(client->pump());
                    ASSERT_TRUE(client->send_stream(1, std::span(reply).subspan(2), false));
                    ASSERT_TRUE(pump_until(*client, [&] { return saved_reply_hex(store->load(started.id)) == hex(reply); }));
                    EXPECT_NE(store->load(started.id).state, storage::RunState::Finalized);
                }
                if (mode == 1) {
                    sent.insert(sent.end(), reply.begin(), reply.end());
                    ASSERT_TRUE(client->send_stream(1, reply, false));
                } else if (mode == 3) {
                    ASSERT_TRUE(client->reset_stream(1, 1));
                } else {
                    // Graceful established SUBSCRIBE completion includes DONE;
                    // discovery notifications follow its initial REQUEST_OK.
                    const auto trailing = mode == 0 && fixture.accepted
                        ? fixture.type == 3 ? bytes({0x0b,0,3,0,0,0})
                        : fixture.type == 0x50 ? bytes({8,0,3,1,1,'n'}) : bytes({}) : bytes({});
                    sent.insert(sent.end(), trailing.begin(), trailing.end());
                    ASSERT_TRUE(client->send_stream(1, trailing, true));
                }
            }
            ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
            const auto run = store->load(started.id);
            const auto outcome = std::find_if(run.outcomes.begin(), run.outcomes.end(), [&](const auto& row) {
                return row.requirement_id == fixture.requirement;
            });
            ASSERT_NE(outcome, run.outcomes.end());
            EXPECT_EQ(outcome->state, mode == 1 || mode == 2 || mode == 4 ? requirements::OutcomeState::Fail
                                                                                      : requirements::OutcomeState::NotRun);
            const auto stimulus = std::find_if(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "raw_probe_stimulus";
            });
            ASSERT_NE(stimulus, run.events.end());
            EXPECT_NE(stimulus->detail.find(" stream=1 channel=1 operation=write application_error=0 operation_accepted=false"
                " accepted=" + std::to_string(opening.size()) + " fin=true bytes=" + hex(opening) + " accepted_event_count="),
                std::string::npos);
            EXPECT_EQ(saved_reply_hex(run), hex(sent));
            const auto saved_fin = std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "raw_probe_transport_event" && event.stream_id == "1" &&
                    event.detail.find(" fin=true bytes=") != std::string::npos;
            });
            const auto saved_reset = std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "raw_probe_transport_event" && event.stream_id == "1" &&
                    event.detail.find(" operation=peer-reset application_error=1 ") != std::string::npos;
            });
            EXPECT_EQ(saved_fin, mode == 0 || mode == 2);
            EXPECT_EQ(saved_reset, mode == 3);
        }
    }
}

TEST(NativeQuicLive, Draft21FetchCountsResponsesThroughActualRequestFin) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    auto d21 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / "requirements/draft21.json"));
    auto d18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "fixture", true, {}});
    for (const bool accepted : {true, false}) {
        // Singleton, duplicate, empty FIN, and RESET without FIN.
        for (unsigned mode = 0; mode < 4; ++mode) {
            SCOPED_TRACE(std::string(accepted ? "accepted " : "rejected ") + std::to_string(mode));
            const auto scenario = accepted ? "d21-fetch-accepted" : "d21-fetch-rejected";
            auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
            app::NativeRunManager manager(d18, d21, store,
                {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
                 .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
            EXPECT_EQ(manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
                app::RunMode::Observed, {scenario}, std::chrono::milliseconds(1500), std::nullopt}).status,
                app::RunStartStatus::InvalidConfig);
            const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
                app::RunMode::Observed, {scenario}, std::chrono::milliseconds(1500), app::TrackFixture{{"n"}, "t"}});
            ASSERT_EQ(started.status, app::RunStartStatus::Started);
            auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
                .alpn = bytes({'m','o','q','t','-','2','1'})});
            ASSERT_NE(client, nullptr);
            ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3);
                return setup && setup->data == bytes({0xaf,0,0,0}); }));
            ASSERT_TRUE(client->send_stream(2, bytes({0xaf,0,0,0}), false));
            const auto opening = bytes({0x16,0,20,1,1,1,'n',1,'t',1,0x21,11,0,0,
                255,255,255,255,255,255,255,255,255});
            ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
                return request && request->data.size() == opening.size() && request->fin; }));
            EXPECT_EQ(client->stream(1)->data, opening);
            const auto saved_reply_hex = [](const auto& run) {
                std::string result;
                for (const auto& event : run.events) {
                    if (event.kind != "raw_probe_transport_event" || event.stream_id != "1") continue;
                    const auto start = event.detail.find(" bytes=");
                    if (start == std::string::npos) continue;
                    const auto first = start + 7;
                    const auto end = event.detail.find(' ', first);
                    result += event.detail.substr(first, end - first);
                }
                return result;
            };
            const std::string reply_hex = accepted ? "18000400000000" : "050003010000";
            const auto reply = accepted ? bytes({0x18,0,4,0,0,0,0}) : bytes({5,0,3,1,0,0});
            if (mode != 2) {
                ASSERT_TRUE(client->send_stream(1, std::span(reply).first(2), false));
                ASSERT_TRUE(client->pump());
                ASSERT_TRUE(client->send_stream(1, std::span(reply).subspan(2), false));
                ASSERT_TRUE(pump_until(*client, [&] {
                    const auto run = store->load(started.id);
                    // Wait for every byte of the first response, regardless of
                    // how QUIC fragments or coalesces its delivery events.
                    return saved_reply_hex(run) == reply_hex;
                }));
                EXPECT_NE(store->load(started.id).state, storage::RunState::Finalized);
            }
            if (mode == 1) {
                ASSERT_TRUE(client->send_stream(1, reply, true));
            } else if (mode == 3) {
                ASSERT_TRUE(client->reset_stream(1, 1));
            } else {
                ASSERT_TRUE(client->send_stream(1, {}, true));
            }
            ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
            const auto run = store->load(started.id);
            const auto outcome = std::find_if(run.outcomes.begin(), run.outcomes.end(), [](const auto& row) {
                return row.requirement_id == "D21-3-2-1-MUST-052";
            });
            ASSERT_NE(outcome, run.outcomes.end());
            // A singleton completes this context; the full catalogue row still
            // requires the other named context. Definitive failures dominate.
            EXPECT_EQ(outcome->state, mode == 1 || mode == 2 ? requirements::OutcomeState::Fail
                                                             : requirements::OutcomeState::NotRun);
            const auto stimulus = std::find_if(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "raw_probe_stimulus";
            });
            ASSERT_NE(stimulus, run.events.end());
            EXPECT_NE(stimulus->detail.find(
                " stream=1 channel=1 operation=write application_error=0 operation_accepted=false"
                " accepted=23 fin=true bytes=1600140101016e017401210b0000ffffffffffffffffff"
                " accepted_event_count="), std::string::npos);
            EXPECT_EQ(saved_reply_hex(run), mode == 2 ? std::string{} :
                mode == 1 ? reply_hex + reply_hex : reply_hex);
            const auto saved_fin = std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "raw_probe_transport_event" && event.stream_id == "1" &&
                    event.detail.find(" fin=true bytes=") != std::string::npos;
            });
            const auto saved_reset = std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "raw_probe_transport_event" && event.stream_id == "1" &&
                    event.detail.find(" operation=peer-reset application_error=1 ") != std::string::npos;
            });
            // Duplicate response bytes are decisive even if a separately
            // delivered FIN has not arrived when the runner stops collecting.
            if (mode != 1) {
                EXPECT_EQ(saved_fin, mode != 3);
            }
            EXPECT_EQ(saved_reset, mode == 3);
        }
    }
}

TEST(NativeQuicLive, FetchCleanupUsesConfiguredTrackAndActualResetEvents) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto load = [&](unsigned draft) {
        const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source, root / (draft == 18 ? "requirements/draft18.json" : "requirements/draft21.json")));
    };
    const auto d18 = load(18);
    const auto d21 = load(21);
    for (const unsigned draft : {18u, 21u}) {
        for (const bool cancel : {true, false}) {
            for (const bool reset_data : {true, false}) {
                SCOPED_TRACE(std::to_string(draft) + (cancel ? " cancel" : " update") +
                             (reset_data ? " reset" : " fin"));
                const auto profiles = draft == 18
                    ? scenarios::draft18_fetch_probes(std::chrono::milliseconds(1500), {bytes({'n'})}, bytes({'t'}))
                    : scenarios::draft21_fetch_probes(std::chrono::milliseconds(1500), {bytes({'n'})}, bytes({'t'}));
                const auto& profile = profiles.at(cancel ? 0 : 2);
                auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
                app::NativeRunManager manager(d18, d21, store,
                    {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
                     .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
                EXPECT_EQ(manager.start({static_cast<app::DraftVersion>(draft), app::TransportKind::NativeQuic,
                    app::RunMode::Observed, {profile.definition.id}, std::chrono::milliseconds(1500),
                    std::nullopt}).status, app::RunStartStatus::InvalidConfig);
                const auto started = manager.start({static_cast<app::DraftVersion>(draft), app::TransportKind::NativeQuic,
                    app::RunMode::Observed, {profile.definition.id}, std::chrono::milliseconds(1500),
                    app::TrackFixture{{"n"}, "t"}});
                ASSERT_EQ(started.status, app::RunStartStatus::Started);
                auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
                    .alpn = draft == 18 ? expected_alpn() : bytes({'m','o','q','t','-','2','1'})});
                ASSERT_NE(client, nullptr);
                ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3);
                    return setup && setup->data.size() == 4; }));
                ASSERT_TRUE(client->send_stream(2, bytes({0xaf,0,0,0}), false));
                const auto opening = profile.definition.writes.front().bytes;
                ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
                    return request && request->data.size() == opening.size() && request->fin == cancel; }));
                EXPECT_EQ(client->stream(1)->data, opening);
                EXPECT_EQ(client->stream(1)->fin, cancel);
                ASSERT_TRUE(client->send_stream(1, bytes({0x18,0,4,0,0,0,0}), false));
                // Actual fragmented FETCH_HEADER on a distinct open client uni.
                ASSERT_TRUE(client->send_stream(6, bytes({5}), false));
                ASSERT_TRUE(client->pump());
                ASSERT_TRUE(client->send_stream(6, bytes({1}), false));
                if (cancel) {
                    ASSERT_TRUE(pump_until(*client, [&] {
                        return client->try_send_stream(1, {}, false).status == test::ClientStreamSendStatus::PeerStopped;
                    }));
                    const auto stopped = client->try_send_stream(1, {}, false);
                    EXPECT_EQ(stopped.application_error, 1U);
                    ASSERT_TRUE(client->reset_stream(1, 1));
                } else {
                    const auto update = bytes({2,0,6,3,1,3,2,2,0});
                    auto expected = opening;
                    expected.insert(expected.end(), update.begin(), update.end());
                    ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
                        return request && request->data == expected && request->fin; }));
                    ASSERT_TRUE(client->send_stream(1, bytes({5,0,3,1,0,0}), false));
                }
                if (reset_data) ASSERT_TRUE(client->reset_stream(6, 1));
                else ASSERT_TRUE(client->send_stream(6, {}, true));
                ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
                const auto run = store->load(started.id);
                const auto check = [&](const std::string& requirement, requirements::OutcomeState expected) {
                    const auto found = std::find_if(run.outcomes.begin(), run.outcomes.end(), [&](const auto& row) {
                        return row.requirement_id == requirement;
                    });
                    ASSERT_NE(found, run.outcomes.end());
                    EXPECT_EQ(found->state, expected);
                };
                if (cancel) check(profiles.at(0).requirement_id, requirements::OutcomeState::Pass);
                check(profiles.at(cancel ? 1 : 2).requirement_id,
                      reset_data ? requirements::OutcomeState::Pass : requirements::OutcomeState::NotRun);
                EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [cancel](const auto& event) {
                    return event.kind == "raw_probe_stimulus" &&
                        event.detail.find(cancel ? "operation=stop-sending application_error=1 operation_accepted=true"
                                                 : "accepted_event_count=") != std::string::npos;
                }));
                EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                    return event.kind == "raw_probe_transport_event" && event.stream_id == "6";
                }));
                if (reset_data) {
                    EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                        return event.kind == "raw_probe_transport_event" && event.stream_id == "6" &&
                               event.detail.find("operation=peer-reset application_error=1") != std::string::npos;
                    }));
                }
                if (cancel) {
                    EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                        return event.kind == "raw_probe_transport_event" && event.stream_id == "1" &&
                               event.detail.find("operation=peer-reset application_error=1") != std::string::npos;
                    }));
                }
                EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                    return event.kind == "compatibility_error_mapping" || event.kind == "unresolved_error_mapping";
                }));
            }
        }
    }
}

TEST(NativeQuicLive, SubscriptionCancellationResetsEveryActualOpenAssociatedStream) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto load = [&](unsigned draft) {
        const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source, root / (draft == 18 ? "requirements/draft18.json" : "requirements/draft21.json")));
    };
    const auto d18 = load(18);
    const auto d21 = load(21);
    for (const unsigned draft : {18u, 21u}) {
        // Two open subgroups; three open subgroups; lone FIN; one missing reset.
        for (const unsigned mode : {0u, 1u, 2u, 3u}) {
            SCOPED_TRACE(std::to_string(draft) + " mode=" + std::to_string(mode));
            const std::string scenario = draft == 18 ? "cancel-subscribe-with-multiple-open-subgroups"
                                                     : "d21-cancel-subscribe-with-open-streams";
            const std::string requirement = draft == 18 ? "D18-5-1-1-MUST-001" : "D21-3-1-1-MUST-045";
            auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
            app::NativeRunManager manager(d18, d21, store,
                {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
                 .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
            const app::RunConfig config{static_cast<app::DraftVersion>(draft), app::TransportKind::NativeQuic,
                app::RunMode::Observed, {scenario}, std::chrono::milliseconds(1500), app::TrackFixture{{"n"}, "t"}};
            auto missing_fixture = config;
            missing_fixture.track_fixture.reset();
            EXPECT_EQ(manager.start(missing_fixture).status, app::RunStartStatus::InvalidConfig);
            const auto started = manager.start(config);
            ASSERT_EQ(started.status, app::RunStartStatus::Started);
            auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
                .alpn = draft == 18 ? expected_alpn() : bytes({'m','o','q','t','-','2','1'})});
            ASSERT_NE(client, nullptr);
            ASSERT_TRUE(pump_until(*client, [&] { const auto setup = client->stream(3);
                return setup && setup->data.size() == 4; }));
            ASSERT_TRUE(client->send_stream(2, bytes({0xaf,0,0,0}), false));
            const auto opening = bytes({3,0,7,1,1,1,'n',1,'t',0});
            ASSERT_TRUE(pump_until(*client, [&] { const auto request = client->stream(1);
                return request && request->data == opening && request->fin; }));
            const auto recorded = [&](const std::string& stream, const std::string& detail) {
                const auto run = store->load(started.id);
                return std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
                    return event.kind == "raw_probe_transport_event" && event.stream_id == stream &&
                           event.detail.find(detail) != std::string::npos;
                });
            };
            ASSERT_TRUE(client->send_stream(1, mode == 1 ? bytes({4,0,2,7}) : bytes({4,0,2,7,0}), false));
            // A previously reset associated stream is excluded at cancellation.
            ASSERT_TRUE(client->send_stream(14, bytes({0x14,7,3,0,32}), false));
            ASSERT_TRUE(pump_until(*client, [&] { return recorded("14", "bytes=1407030020"); }));
            ASSERT_TRUE(client->reset_stream(14, 1));
            ASSERT_TRUE(client->send_stream(18, bytes({0x14,8,4,0,32}), false));
            ASSERT_TRUE(pump_until(*client, [&] {
                return recorded("14", "operation=peer-reset") && recorded("18", "bytes=1408040020");
            }));
            if (mode == 1) {
                ASSERT_TRUE(client->send_stream(22, bytes({0x14,7,5,0,32}), false));
                ASSERT_TRUE(pump_until(*client, [&] { return recorded("22", "bytes=1407050020"); }));
            }
            ASSERT_TRUE(client->send_stream(6, bytes({0x14,7,1,0,32}), false));
            ASSERT_TRUE(client->send_stream(10, bytes({0x14,7,2,0}), false));
            ASSERT_TRUE(pump_until(*client, [&] { return recorded("10", "bytes=14070200"); }));
            EXPECT_NE(client->try_send_stream(1, {}, false).status, test::ClientStreamSendStatus::PeerStopped);
            ASSERT_TRUE(client->send_stream(10, bytes({32}), false));
            if (mode == 1) {
                // Complete OK only after all three actual open headers arrived.
                ASSERT_TRUE(pump_until(*client, [&] { return recorded("10", "bytes=20"); }));
                ASSERT_TRUE(client->send_stream(1, bytes({0}), false));
            }
            ASSERT_TRUE(pump_until(*client, [&] {
                return client->try_send_stream(1, {}, false).status == test::ClientStreamSendStatus::PeerStopped;
            }));
            EXPECT_EQ(client->try_send_stream(1, {}, false).application_error, 1U);
            if (mode == 2) {
                // A local STOP marker cannot prove when the publisher committed FIN.
                ASSERT_TRUE(client->send_stream(6, {}, true));
            } else {
                ASSERT_TRUE(client->reset_stream(1, 1));
                ASSERT_TRUE(client->reset_stream(10, 1));
                if (mode != 3) { ASSERT_TRUE(client->reset_stream(6, 1)); }
                if (mode == 1) { ASSERT_TRUE(client->reset_stream(22, 1)); }
                if (mode == 3) { ASSERT_TRUE(client->close(0, {})); }
            }
            ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
            const auto run = store->load(started.id);
            const auto outcome = std::find_if(run.outcomes.begin(), run.outcomes.end(), [&](const auto& row) {
                return row.requirement_id == requirement;
            });
            ASSERT_NE(outcome, run.outcomes.end());
            EXPECT_EQ(outcome->state, mode >= 2 ? requirements::OutcomeState::NotRun
                                               : requirements::OutcomeState::Pass);
            EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "raw_probe_stimulus" && event.detail.find(
                    "operation=stop-sending application_error=1 operation_accepted=true") != std::string::npos;
            }));
            if (mode < 2) {
                for (const auto* stream : {"1", "6", "10"}) {
                    EXPECT_TRUE(recorded(stream, "operation=peer-reset application_error=1"));
                }
            }
            EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "compatibility_error_mapping" || event.kind == "unresolved_error_mapping";
            }));
        }
    }
}

TEST(NativeQuicLive, RequestErrorProbesRouteBothDraftsWithoutTrack) {
    TestPemFiles pem;
    for (const unsigned draft : {18u, 21u}) {
        SCOPED_TRACE(draft);
        const std::string scenario = draft == 18 ? "request-track-in-single-period-namespace" : "d21-request-single-period-namespace";
        const std::string id = draft == 18 ? "D18-3-2-1-MUST-002" : "D21-2-4-2-MUST-031";
        const std::string evaluator = draft == 18 ? "request-rejected-does-not-exist" : "d21-single-period-request-does-not-exist";
        requirements::Requirement row{id, requirements::Strength::Must, {"test", 1, 1, 1, 1},
            "receiver", "reject a single-period namespace", requirements::Applicability::Applicable,
            requirements::Testability::Testable, {scenario}, {evaluator}, ""};
        auto d18 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{18, "test", true,
                draft == 18 ? std::vector{row} : std::vector<requirements::Requirement>{}});
        auto d21 = std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog{21, "test", true,
                draft == 21 ? std::vector{row} : std::vector<requirements::Requirement>{}});
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(d18, d21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
             .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const app::RunConfig config{static_cast<app::DraftVersion>(draft), app::TransportKind::NativeQuic,
            app::RunMode::Observed, {scenario}, std::chrono::milliseconds(1000), std::nullopt};
        const auto started = manager.start(config);
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
            .alpn = draft == 18 ? expected_alpn() : bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto setup = client->stream(3);
            return setup && setup->data.size() == 4;
        }));
        ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto request = client->stream(1);
            return request && request->data.size() == 10;
        }));
        EXPECT_EQ(client->stream(1)->data, bytes({3, 0, 7, 1, 1, 1, '.', 1, 'x', 0}));
        ASSERT_TRUE(client->send_stream(1, bytes({5, 0, 3, 0x10, 0, 0}), true));
        ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        const auto run = store->load(started.id);
        ASSERT_EQ(run.outcomes.size(), 1u);
        EXPECT_EQ(run.outcomes.front().state, requirements::OutcomeState::Pass);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
            return event.kind == "raw_probe_transport_event" && event.stream_id == "1" &&
                   event.detail.find("bytes=050003100000") != std::string::npos;
        }));
        EXPECT_FALSE(client->peer_close().has_value());
    }
}

TEST(NativeQuicLive, ConfiguredUnknownAliasCompatibilityIsExplicitAndOptional) {
    TestPemFiles pem;
    for (const unsigned draft : {18u, 21u}) {
        for (const auto code : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0x19}}) {
            SCOPED_TRACE(draft);
            const std::string scenario = draft == 18 ? "receive-use-alias-for-unregistered-token" : "d21-request-unknown-token-alias";
            const std::string id = draft == 18 ? "D18-10-2-2-MUST-007" : "D21-8-9-MUST-269";
            const std::string evaluator = draft == 18 ? "request-error-unknown-auth-token-alias" : "d21-unknown-token-alias-message-error";
            requirements::Requirement row{id, requirements::Strength::Must, {"test", 1, 1, 1, 1},
                "receiver", "reject an unregistered authorization token alias", requirements::Applicability::Applicable,
                requirements::Testability::Testable, {scenario}, {evaluator}, ""};
            auto d18 = std::make_shared<const requirements::RequirementCatalog>(
                requirements::RequirementCatalog{18, "test", true,
                    draft == 18 ? std::vector{row} : std::vector<requirements::Requirement>{}});
            auto d21 = std::make_shared<const requirements::RequirementCatalog>(
                requirements::RequirementCatalog{21, "test", true,
                    draft == 21 ? std::vector{row} : std::vector<requirements::Requirement>{}});
            auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
            app::NativeRunManager manager(d18, d21, store,
                {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1", .port_start = 0, .port_end = 0,
                 .maximum_active_runs = 1, .certificate_path = pem.certificate(), .private_key_path = pem.key(),
                 .unknown_auth_token_alias_compatibility_code = code});
            const app::RunConfig config{static_cast<app::DraftVersion>(draft), app::TransportKind::NativeQuic,
                app::RunMode::Observed, {scenario}, std::chrono::milliseconds(1000), std::nullopt};
            const auto started = manager.start(config);
            ASSERT_EQ(started.status, app::RunStartStatus::Started);
            auto client = test::PicoquicTestClient::create({.port = started.endpoint.port,
                .alpn = draft == 18 ? expected_alpn() : bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
            ASSERT_NE(client, nullptr);
            ASSERT_TRUE(pump_until(*client, [&] {
                const auto setup = client->stream(3);
                return setup && setup->data.size() == 4;
            }));
            ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
            ASSERT_TRUE(pump_until(*client, [&] {
                const auto request = client->stream(1);
                return request && request->data.size() == (draft == 18 ? 14u : 12u);
            }));
            EXPECT_EQ(client->stream(1)->data, draft == 18
                ? bytes({3, 0, 11, 1, 1, 1, 'n', 1, 'x', 1, 3, 2, 2, 7})
                : bytes({3, 0, 9, 1, 0, 1, 'x', 1, 3, 2, 2, 0}));
            ASSERT_TRUE(client->send_stream(1, bytes({5, 0, 3, 0x19, 0, 0}), true));
            ASSERT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
            const auto run = store->load(started.id);
            ASSERT_EQ(run.outcomes.size(), 1u);
            EXPECT_EQ(run.outcomes.front().state, code ? requirements::OutcomeState::Pass : requirements::OutcomeState::NotRun);
            EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
                return event.kind == (code ? "compatibility_error_mapping" : "unresolved_error_mapping") &&
                       event.requirement_id == id && event.detail.find("request code is unassigned") != std::string::npos;
            }));
            EXPECT_FALSE(client->peer_close().has_value());
        }
    }
}

TEST(NativeQuicLive, HttpDraft21GreaseSetupProfilesScoreReceiverRequirements) {
    // draft-ietf-moq-transport-21 sections 9.1, 13 and 16.4.
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    const auto requirement = [](std::string id, bool duplicate_only) {
        return requirements::Requirement{
            std::move(id), requirements::Strength::Must,
            {"9.1", 3454, 3454, 1, 1}, "receiver of SETUP",
            "Ignore unknown Setup Options.",
            requirements::Applicability::Applicable,
            requirements::Testability::Testable,
            duplicate_only
                ? std::vector<std::string>{"d21-setup-duplicate-unknown-options"}
                : std::vector<std::string>{"d21-setup-unknown-options",
                                           "d21-setup-duplicate-unknown-options"},
            {duplicate_only
                 ? "d21-duplicate-unknown-setup-options-accepted"
                 : "d21-unknown-setup-options-ignored"}, ""};
    };
    auto draft21 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            21, "test", true,
            {requirement("D21-9-1-MUST-287", false),
             requirement("D21-9-1-MUST-288", false),
             requirement("D21-9-1-MUST-290", true)}});
    auto runs = std::make_shared<app::NativeRunManager>(
        draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0,
            .maximum_active_runs = 2, .certificate_path = pem.certificate(),
            .private_key_path = pem.key()});
    http::HttpServer server(draft18, draft21, store,
                            app::BuildInfo{"test", "test", {}},
                            {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    struct Probe {
        std::string scenario;
        std::vector<std::byte> setup;
        std::string verdict;
        std::string duplicate_outcome;
    };
    const std::array probes{
        Probe{"d21-setup-unknown-options",
              bytes({0xaf, 0x00, 0x00, 0x04, 0x80, 0x9d, 0x01, 0xaa}),
              "incomplete", "not_run"},
        Probe{"d21-setup-duplicate-unknown-options",
              bytes({0xaf, 0x00, 0x00, 0x07, 0x80, 0x9d, 0x01, 0xaa,
                     0x00, 0x01, 0xbb}),
              "pass", "pass"}};
    for (const auto& probe : probes) {
        SCOPED_TRACE(probe.scenario);
        const nlohmann::json request = {
            {"draft", 21}, {"transport", "native-quic"},
            {"mode", "observed"},
            {"scenarios", nlohmann::json::array({probe.scenario})},
            {"timeout_ms", 1000},
            {"track", {{"namespace_hex", nlohmann::json::array({"6d65646961"})},
                       {"name_hex", "74657374"}}}};
        const auto created = api.Post("/api/v1/runs", request.dump(),
                                      "application/json");
        ASSERT_TRUE(created);
        ASSERT_EQ(created->status, 201) << created->body;
        const auto body = nlohmann::json::parse(created->body);
        const auto port = body.at("publisher_endpoint").at("port")
                              .get<std::uint16_t>();
        const auto id = body.at("run").at("id").get<std::string>();
        auto client = test::PicoquicTestClient::create(
            {.port = port,
             .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto setup = client->stream(3);
            return setup && setup->data == probe.setup;
        }));
        ASSERT_TRUE(client->send_stream(
            2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
        ASSERT_TRUE(client->send_stream(
            0, bytes({0x1d, 0x00, 0x0f,
                      0x00, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                      0x04, 't', 'e', 's', 't', 0x02, 0x00}), false));
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto response = client->stream(0);
            return response && response->data ==
                       bytes({0x07, 0x00, 0x01, 0x00}) &&
                   store->load(id).state == storage::RunState::Finalized;
        }));
        const auto result = api.Get("/api/v1/runs/" + id);
        ASSERT_TRUE(result);
        ASSERT_EQ(result->status, 200);
        const auto run = nlohmann::json::parse(result->body).at("run");
        EXPECT_EQ(run.at("score").at("verdict"), probe.verdict);
        const auto& outcomes = run.at("outcomes");
        const auto state_for = [&](const char* requirement_id) {
            const auto found = std::find_if(
                outcomes.begin(), outcomes.end(),
                [&](const auto& outcome) {
                    return outcome.at("requirement_id") == requirement_id;
                });
            return found == outcomes.end()
                       ? std::string("missing")
                       : found->at("state").template get<std::string>();
        };
        EXPECT_EQ(state_for("D21-9-1-MUST-287"), "pass");
        EXPECT_EQ(state_for("D21-9-1-MUST-288"), "pass");
        EXPECT_EQ(state_for("D21-9-1-MUST-290"), probe.duplicate_outcome);
        const auto stored = store->load(id);
        ASSERT_FALSE(stored.events.empty());
        for (const auto& event : stored.events) {
            EXPECT_EQ(event.scenario_id, probe.scenario);
        }
    }
}

TEST(NativeQuicLive, HttpDraft21ForbiddenServerUriOptionsScorePeerClose) {
    // draft-ietf-moq-transport-21 sections 9.1.1, 9.1.2, and 12.2.
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{18, "test", true, {}});
    auto draft21 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            21, "test", true,
            {{"D21-9-1-1-MUST-293", requirements::Strength::Must,
              {"9.1.1", 3493, 3494, 1, 1}, "client receiving AUTHORITY",
              "Close with INVALID_AUTHORITY.",
              requirements::Applicability::Applicable,
              requirements::Testability::Testable,
              {"d21-server-sends-authority"},
              {"d21-server-authority-invalid-authority"}, ""},
             {"D21-9-1-2-MUST-300", requirements::Strength::Must,
              {"9.1.2", 3510, 3510, 1, 1}, "client receiving PATH",
              "Close with INVALID_PATH.",
              requirements::Applicability::Applicable,
              requirements::Testability::Testable,
              {"d21-server-sends-path"},
              {"d21-server-path-invalid-path"}, ""}}});
    auto runs = std::make_shared<app::NativeRunManager>(
        draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0,
            .maximum_active_runs = 2, .certificate_path = pem.certificate(),
            .private_key_path = pem.key()});
    http::HttpServer server(draft18, draft21, store,
                            app::BuildInfo{"test", "test", {}},
                            {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    struct Probe {
        std::string scenario;
        std::vector<std::byte> setup;
        std::uint64_t close_code;
        std::string pass_id;
        std::string not_run_id;
    };
    const std::array probes{
        Probe{"d21-server-sends-authority",
              bytes({0xaf, 0x00, 0x00, 0x0d, 0x05, 0x0b,
                     'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'o', 'r', 'g'}),
              0x19, "D21-9-1-1-MUST-293", "D21-9-1-2-MUST-300"},
        Probe{"d21-server-sends-path",
              bytes({0xaf, 0x00, 0x00, 0x03, 0x01, 0x01, '/'}),
              0x8, "D21-9-1-2-MUST-300", "D21-9-1-1-MUST-293"}};
    for (const auto& probe : probes) {
        SCOPED_TRACE(probe.scenario);
        const nlohmann::json request = {
            {"draft", 21}, {"transport", "native-quic"},
            {"mode", "observed"},
            {"scenarios", nlohmann::json::array({probe.scenario})},
            {"timeout_ms", 1000},
            {"track", {{"namespace_hex", nlohmann::json::array({"6d65646961"})},
                       {"name_hex", "74657374"}}}};
        const auto created = api.Post("/api/v1/runs", request.dump(),
                                      "application/json");
        ASSERT_TRUE(created);
        ASSERT_EQ(created->status, 201) << created->body;
        const auto body = nlohmann::json::parse(created->body);
        const auto port = body.at("publisher_endpoint").at("port")
                              .get<std::uint16_t>();
        const auto id = body.at("run").at("id").get<std::string>();
        auto client = test::PicoquicTestClient::create(
            {.port = port,
             .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto setup = client->stream(3);
            return setup && setup->data == probe.setup;
        }));
        ASSERT_TRUE(client->close(probe.close_code, {}));
        ASSERT_TRUE(pump_until(*client, [&] {
            return store->load(id).state == storage::RunState::Finalized;
        }));
        const auto result = api.Get("/api/v1/runs/" + id);
        ASSERT_TRUE(result);
        ASSERT_EQ(result->status, 200);
        const auto run = nlohmann::json::parse(result->body).at("run");
        EXPECT_EQ(run.at("score").at("verdict"), "incomplete");
        const auto& outcomes = run.at("outcomes");
        const auto state_for = [&](const std::string& requirement_id) {
            const auto found = std::find_if(
                outcomes.begin(), outcomes.end(),
                [&](const auto& outcome) {
                    return outcome.at("requirement_id") == requirement_id;
                });
            return found == outcomes.end()
                       ? std::string("missing")
                       : found->at("state").template get<std::string>();
        };
        EXPECT_EQ(state_for(probe.pass_id), "pass");
        EXPECT_EQ(state_for(probe.not_run_id), "not_run");
        const auto stored = store->load(id);
        const auto close_event = std::find_if(
            stored.events.begin(), stored.events.end(),
            [](const auto& event) { return event.kind == "peer_closed"; });
        ASSERT_NE(close_event, stored.events.end());
        EXPECT_EQ(close_event->detail,
                  "draft-21 peer application close code " +
                      std::to_string(probe.close_code));

        const auto wrong_created = api.Post("/api/v1/runs", request.dump(),
                                            "application/json");
        ASSERT_TRUE(wrong_created);
        ASSERT_EQ(wrong_created->status, 201) << wrong_created->body;
        const auto wrong_body = nlohmann::json::parse(wrong_created->body);
        const auto wrong_port = wrong_body.at("publisher_endpoint").at("port")
                                    .get<std::uint16_t>();
        const auto wrong_id = wrong_body.at("run").at("id").get<std::string>();
        auto wrong_client = test::PicoquicTestClient::create(
            {.port = wrong_port,
             .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(wrong_client, nullptr);
        ASSERT_TRUE(pump_until(*wrong_client, [&] {
            const auto setup = wrong_client->stream(3);
            return setup && setup->data == probe.setup;
        }));
        ASSERT_TRUE(wrong_client->close(3, {}));
        ASSERT_TRUE(pump_until(*wrong_client, [&] {
            return store->load(wrong_id).state == storage::RunState::Finalized;
        }));
        const auto wrong_result = api.Get("/api/v1/runs/" + wrong_id);
        ASSERT_TRUE(wrong_result);
        ASSERT_EQ(wrong_result->status, 200);
        const auto wrong_run =
            nlohmann::json::parse(wrong_result->body).at("run");
        EXPECT_EQ(wrong_run.at("score").at("verdict"), "fail");
        const auto& wrong_outcomes = wrong_run.at("outcomes");
        const auto failed = std::find_if(
            wrong_outcomes.begin(), wrong_outcomes.end(),
            [&](const auto& outcome) {
                return outcome.at("requirement_id") == probe.pass_id;
            });
        ASSERT_NE(failed, wrong_outcomes.end());
        EXPECT_EQ(failed->at("state"), "fail");
        const auto wrong_stored = store->load(wrong_id);
        const auto wrong_close = std::find_if(
            wrong_stored.events.begin(), wrong_stored.events.end(),
            [](const auto& event) { return event.kind == "peer_closed"; });
        ASSERT_NE(wrong_close, wrong_stored.events.end());
        EXPECT_EQ(wrong_close->detail,
                  "draft-21 peer application close code 3");
    }
}

TEST(NativeQuicLive, HttpStopFinalizesAnActiveRunWithoutPublisherFailure) {
    TestPemFiles pem;
    auto store = std::make_shared<storage::SqliteRunStore>(
        ":memory:", app::BuildInfo{"test", "test", {}});
    requirements::Requirement response_requirement{
        "D18-5.1-MUST-003", requirements::Strength::Must,
        {"5.1", 1936, 1937, 1, 1}, "publisher",
        "exactly one SUBSCRIBE response",
        requirements::Applicability::Applicable,
        requirements::Testability::Testable,
        {"subscribe-to-publisher-track"},
        {"exactly-one-subscribe-ok-or-request-error"}, ""};
    auto draft18 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{
            18, "test", true, {std::move(response_requirement)}});
    auto draft21 = std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog{21, "test", true, {}});
    auto runs = std::make_shared<app::NativeRunManager>(
        draft18, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
            .certificate_path = pem.certificate(),
            .private_key_path = pem.key()});
    http::HttpServer server(draft18, draft21, store,
                            app::BuildInfo{"test", "test", {}},
                            {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    const nlohmann::json request = {
        {"draft", 18}, {"transport", "native-quic"},
        {"mode", "observed"},
        {"scenarios", nlohmann::json::array({"subscribe-to-publisher-track"})},
        {"timeout_ms", 1000},
        {"track", {{"namespace_hex", nlohmann::json::array({"6e"})},
                   {"name_hex", "78"}}}};
    const auto created = api.Post("/api/v1/runs", request.dump(),
                                  "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201);
    const auto id = nlohmann::json::parse(created->body)
                        .at("run").at("id").get<std::string>();
    const auto stopped = api.Post("/api/v1/runs/" + id + "/stop", "",
                                  "application/json");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->status, 200) << stopped->body;
    EXPECT_EQ(store->load(id).state, storage::RunState::Finalized);
    ASSERT_TRUE(store->load(id).score.has_value());
    EXPECT_EQ(store->load(id).score->verdict,
              requirements::RunVerdict::Incomplete);
}

TEST(NativeQuicLive, DifferentAlpnNeverEstablishes) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
    ASSERT_NE(client, nullptr);
    bool server_established = false;
    EXPECT_FALSE(pump_until(
        *client,
        [&] {
            for (const auto& event : created.listener->poll(64)) {
                server_established |=
                    std::holds_alternative<ConnectionEstablishedEvent>(event);
            }
            return server_established;
        },
        std::chrono::milliseconds{250}));
    EXPECT_FALSE(server_established);
}

TEST(NativeQuicLive, Draft21ListenerNegotiatesOnlyDraft21) {
    // draft-ietf-moq-transport-21 section 6.2: the draft ALPN is moqt-21.
    TestPemFiles pem;
    auto config = live_config(pem);
    config.expected_alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'});
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = config.expected_alpn});
    ASSERT_NE(client, nullptr);
    bool established = false;
    std::vector<std::byte> negotiated;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(64)) {
            if (const auto* connection =
                    std::get_if<ConnectionEstablishedEvent>(&event)) {
                established = true;
                negotiated = connection->alpn;
            }
        }
        return established;
    }));
    EXPECT_EQ(negotiated, config.expected_alpn);
}

TEST(NativeQuicLive, MissingDatagramProducesOnlyConfiguredLocalClose) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.missing_datagram_application_error = 77;
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn(),
         .enable_datagrams = false});
    ASSERT_NE(client, nullptr);
    bool established = false;
    bool local_close = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(64)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
            if (const auto* close = std::get_if<LocalCloseEvent>(&event)) {
                local_close = close->error_space == CloseErrorSpace::Application &&
                              close->error_code == 77;
                EXPECT_EQ(close->reason,
                          bytes({'Q', 'U', 'I', 'C', ' ', 'D', 'A', 'T', 'A',
                                 'G', 'R', 'A', 'M', ' ', 'n', 'o', 't', ' ',
                                 'n', 'e', 'g', 'o', 't', 'i', 'a', 't', 'e', 'd'}));
            }
        }
        return local_close;
    }));
    EXPECT_FALSE(established);
}

TEST(NativeQuicLive, ReportsPeerAndLocalApplicationCloseEvidence) {
    TestPemFiles pem;
    auto peer_created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(peer_created.listener, nullptr);
    auto peer_client = test::PicoquicTestClient::create(
        {.port = peer_created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(peer_client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*peer_client, [&] {
        for (const auto& event : peer_created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    EXPECT_FALSE(peer_client->close(41, bytes({1, 0, 2})));
    ASSERT_TRUE(peer_client->close(41, bytes({1, 9, 2})));
    bool peer_close = false;
    std::size_t peer_terminal_count = 0;
    ASSERT_TRUE(pump_until(*peer_client, [&] {
        for (const auto& event : peer_created.listener->poll(8)) {
            peer_terminal_count += terminal_event(event) ? 1u : 0u;
            if (const auto* close = std::get_if<PeerCloseEvent>(&event)) {
                peer_close = close->error_space == CloseErrorSpace::Application &&
                             close->error_code == 41 &&
                             close->reason == bytes({1, 9, 2});
            }
        }
        return peer_close;
    }));
    EXPECT_EQ(peer_terminal_count, 1u);
    EXPECT_EQ(peer_created.listener->close(43, {}).status,
              TransportStatus::ConnectionClosed);
    for (int iteration = 0; iteration < 3; ++iteration) {
        EXPECT_TRUE(peer_created.listener->poll(8).empty());
    }

    auto local_created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(local_created.listener, nullptr);
    auto local_client = test::PicoquicTestClient::create(
        {.port = local_created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(local_client, nullptr);
    established = false;
    ASSERT_TRUE(pump_until(*local_client, [&] {
        for (const auto& event : local_created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    EXPECT_EQ(local_created.listener->close(42, bytes({3, 0, 4})).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(local_created.listener->close(42, bytes({3, 9, 4})).status,
              TransportStatus::Success);
    const auto local_events = local_created.listener->poll(8);
    EXPECT_EQ(std::ranges::count_if(local_events, terminal_event), 1);
    const auto local = std::ranges::find_if(local_events, [](const auto& event) {
        return std::holds_alternative<LocalCloseEvent>(event);
    });
    ASSERT_NE(local, local_events.end());
    const auto& close = std::get<LocalCloseEvent>(*local);
    EXPECT_EQ(close.error_space, CloseErrorSpace::Application);
    EXPECT_EQ(close.error_code, 42u);
    EXPECT_EQ(close.reason, bytes({3, 9, 4}));
    EXPECT_EQ(local_created.listener->close(43, {}).status,
              TransportStatus::ConnectionClosed);
    for (int iteration = 0; iteration < 3; ++iteration) {
        EXPECT_TRUE(local_created.listener->poll(8).empty());
    }
}

TEST(NativeQuicLive, ReportsIdleTimeout) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.idle_timeout = std::chrono::milliseconds{20};
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    bool timed_out = false;
    std::size_t terminal_count = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{500};
    while (!timed_out && std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : created.listener->poll(8)) {
            terminal_count += terminal_event(event) ? 1u : 0u;
            timed_out |= std::holds_alternative<IdleTimeoutEvent>(event);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(terminal_count, 1u);
    EXPECT_EQ(created.listener->close(44, {}).status,
              TransportStatus::ConnectionClosed);
    for (int iteration = 0; iteration < 3; ++iteration) {
        EXPECT_TRUE(created.listener->poll(8).empty());
    }
}

TEST(NativeQuicLive, ReportsPeerResetAndStopSending) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    ASSERT_TRUE(client->send_stream(0, bytes({1}), false));
    ASSERT_TRUE(client->reset_stream(0, 51));
    bool reset = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            if (const auto* peer_reset = std::get_if<PeerResetEvent>(&event)) {
                reset = peer_reset->stream_id == 0 &&
                        peer_reset->application_error == 51;
            }
        }
        return reset;
    }));

    const auto opened = created.listener->open_bidi();
    ASSERT_EQ(opened.status, TransportStatus::Success);
    ASSERT_EQ(created.listener->write(opened.stream_id, bytes({2}), false).status,
              TransportStatus::Success);
    ASSERT_TRUE(client->pump());
    ASSERT_TRUE(client->stop_stream(opened.stream_id, 52));
    auto stop_events = created.listener->poll(8);
    const auto stopped =
        created.listener->write(opened.stream_id, bytes({3}), false);
    ASSERT_EQ(stopped.status, TransportStatus::PeerStopped);
    ASSERT_EQ(stopped.application_error, 52u);
    auto remaining = created.listener->poll(8);
    stop_events.insert(stop_events.end(), std::make_move_iterator(remaining.begin()),
                       std::make_move_iterator(remaining.end()));
    const auto stop = std::ranges::find_if(stop_events, [](const auto& event) {
        return std::holds_alternative<PeerStopSendingEvent>(event);
    });
    ASSERT_NE(stop, stop_events.end());
    EXPECT_EQ(std::get<PeerStopSendingEvent>(*stop).stream_id,
              opened.stream_id);
    EXPECT_EQ(std::get<PeerStopSendingEvent>(*stop).application_error, 52u);
}

TEST(NativeQuicLive, MalformedAndUnknownCidPacketsDoNotDamageSession) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    ASSERT_TRUE(send_raw(created.listener->bound_endpoint().port, bytes({0})));
    std::vector<std::byte> unknown(17, std::byte{0});
    unknown[0] = std::byte{0x40};
    ASSERT_TRUE(send_raw(created.listener->bound_endpoint().port, unknown));
    EXPECT_TRUE(created.listener->poll(8).empty());

    ASSERT_TRUE(client->send_datagram(bytes({7, 0, 8})));
    bool received = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            if (const auto* datagram = std::get_if<DatagramEvent>(&event)) {
                received = datagram->data == bytes({7, 0, 8});
            }
        }
        return received;
    }));
}

TEST(NativeQuicLive, ASecondPublisherCannotReplaceTheActiveSession) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto first = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(first, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*first, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    auto second = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(second, nullptr);
    for (std::size_t iteration = 0; iteration < 20; ++iteration) {
        ASSERT_TRUE(second->pump());
        EXPECT_TRUE(created.listener->poll(8).empty());
    }
    EXPECT_FALSE(second->established());

    ASSERT_TRUE(first->send_datagram(bytes({9, 0, 1})));
    bool received = false;
    ASSERT_TRUE(pump_until(*first, [&] {
        for (const auto& event : created.listener->poll(8)) {
            if (const auto* datagram = std::get_if<DatagramEvent>(&event)) {
                received = datagram->data == bytes({9, 0, 1});
            }
        }
        return received;
    }));
}

TEST(NativeQuicLive, MigratesToAdditionalCidAndRetiresInitialSequence) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established && client->established() && client->available_destination_ids() > 0;
    }));

    const auto migrated_sequence = client->migrate_source();
    ASSERT_TRUE(migrated_sequence.has_value());
    EXPECT_NE(*migrated_sequence, 0u);

    ASSERT_TRUE(client->send_datagram(bytes({8, 0, 9})));
    bool received = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            if (const auto* datagram = std::get_if<DatagramEvent>(&event)) {
                received = datagram->data == bytes({8, 0, 9});
            }
        }
        return received;
    }));
    ASSERT_TRUE(client->retire_destination_id(0));
    ASSERT_TRUE(client->send_datagram(bytes({10, 0, 11})));
    received = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            if (const auto* datagram = std::get_if<DatagramEvent>(&event)) {
                received = datagram->data == bytes({10, 0, 11});
            }
        }
        return received;
    }));
}

TEST(NativeQuicLive, ClientStreamsPreserveBytesAndFinAcrossSends) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    ASSERT_TRUE(client->send_stream(0, bytes({1, 0}), false));
    ASSERT_TRUE(client->send_stream(0, bytes({2, 3}), false));
    ASSERT_TRUE(client->send_stream(0, {}, true));
    ASSERT_TRUE(client->send_stream(2, bytes({4}), false));
    ASSERT_TRUE(client->send_stream(2, bytes({0, 5}), true));
    std::vector<std::byte> bidi;
    std::vector<std::byte> uni;
    std::size_t bidi_fin = 0;
    std::size_t uni_fin = 0;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(32)) {
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) {
                auto& target = stream->stream_id == 0 ? bidi : uni;
                target.insert(target.end(), stream->data.begin(),
                              stream->data.end());
                if (stream->stream_id == 0 && stream->fin) ++bidi_fin;
                if (stream->stream_id == 2 && stream->fin) ++uni_fin;
            }
        }
        return bidi_fin == 1 && uni_fin == 1;
    }));
    EXPECT_EQ(bidi, bytes({1, 0, 2, 3}));
    EXPECT_EQ(uni, bytes({4, 0, 5}));
    EXPECT_EQ(bidi_fin, 1u);
    EXPECT_EQ(uni_fin, 1u);
}

TEST(NativeQuicLive, ServerStreamsPreserveBytesAndFinAcrossSends) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    const auto bidi = created.listener->open_bidi();
    const auto uni = created.listener->open_uni();
    ASSERT_EQ(bidi.status, TransportStatus::Success);
    ASSERT_EQ(uni.status, TransportStatus::Success);
    ASSERT_EQ(created.listener->write(bidi.stream_id, bytes({1, 0}), false).status,
              TransportStatus::Success);
    ASSERT_EQ(created.listener->write(bidi.stream_id, bytes({2}), false).status,
              TransportStatus::Success);
    ASSERT_EQ(created.listener->write(bidi.stream_id, {}, true).status,
              TransportStatus::Success);
    ASSERT_EQ(created.listener->write(uni.stream_id, bytes({3}), false).status,
              TransportStatus::Success);
    ASSERT_EQ(created.listener->write(uni.stream_id, bytes({0, 4}), true).status,
              TransportStatus::Success);
    ASSERT_TRUE(pump_until(*client, [&] {
        created.listener->poll(16);
        const auto bidi_observation = client->stream(bidi.stream_id);
        const auto uni_observation = client->stream(uni.stream_id);
        return bidi_observation && bidi_observation->fin && uni_observation &&
               uni_observation->fin;
    }));
    const auto bidi_observation = client->stream(bidi.stream_id);
    const auto uni_observation = client->stream(uni.stream_id);
    ASSERT_TRUE(bidi_observation.has_value());
    ASSERT_TRUE(uni_observation.has_value());
    EXPECT_EQ(bidi_observation->data, bytes({1, 0, 2}));
    EXPECT_EQ(uni_observation->data, bytes({3, 0, 4}));
    EXPECT_EQ(bidi_observation->fin_count, 1u);
    EXPECT_EQ(uni_observation->fin_count, 1u);
}

TEST(NativeQuicLive, ServerResetAndStopSendingReachClientWithExactCodes) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    const auto server_stream = created.listener->open_bidi();
    ASSERT_EQ(server_stream.status, TransportStatus::Success);
    ASSERT_EQ(created.listener->write(server_stream.stream_id, bytes({1}), false)
                  .status,
              TransportStatus::Success);
    ASSERT_EQ(created.listener->reset(server_stream.stream_id, 71).status,
              TransportStatus::Success);
    ASSERT_TRUE(client->send_stream(0, bytes({2}), false));
    bool received_client_stream = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) {
                received_client_stream = stream->stream_id == 0 &&
                                         stream->data == bytes({2});
            }
        }
        return received_client_stream;
    }));
    ASSERT_EQ(created.listener->stop_sending(0, 72).status,
              TransportStatus::Success);
    ASSERT_TRUE(pump_until(*client, [&] {
        created.listener->poll(16);
        const auto reset = client->stream(server_stream.stream_id);
        const auto stopped = client->try_send_stream(0, bytes({3}), false);
        return reset && reset->reset_error == 71 &&
               stopped.status == test::ClientStreamSendStatus::PeerStopped &&
               stopped.application_error == 72;
    }));
}

TEST(NativeQuicLive, DatagramsAreBidirectionalEmptySafeAndBounded) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    ASSERT_TRUE(client->send_datagram({}));
    ASSERT_TRUE(client->send_datagram(bytes({1, 0, 2})));
    std::vector<std::vector<std::byte>> server_datagrams;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            if (const auto* datagram = std::get_if<DatagramEvent>(&event)) {
                server_datagrams.push_back(datagram->data);
            }
        }
        return server_datagrams.size() == 2;
    }));
    ASSERT_EQ(server_datagrams[0], std::vector<std::byte>{});
    ASSERT_EQ(server_datagrams[1], bytes({1, 0, 2}));

    ASSERT_EQ(created.listener->send_datagram({}).status,
              TransportStatus::Success);
    ASSERT_EQ(created.listener->send_datagram(bytes({3, 0, 4})).status,
              TransportStatus::Success);
    std::vector<std::vector<std::byte>> client_datagrams;
    ASSERT_TRUE(pump_until(*client, [&] {
        created.listener->poll(16);
        auto next = client->take_datagrams();
        client_datagrams.insert(client_datagrams.end(),
                                std::make_move_iterator(next.begin()),
                                std::make_move_iterator(next.end()));
        return client_datagrams.size() == 2;
    }));
    EXPECT_EQ(client_datagrams[0], std::vector<std::byte>{});
    EXPECT_EQ(client_datagrams[1], bytes({3, 0, 4}));
    EXPECT_EQ(created.listener->send_datagram(
                  std::vector<std::byte>(65'536, std::byte{1})).status,
              TransportStatus::DatagramTooLarge);
}

TEST(NativeQuicLive, EventOverflowPreservesEvidenceAndClosesTransport) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.max_events = 2;
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    ASSERT_TRUE(client->send_datagram(bytes({1})));
    ASSERT_TRUE(client->send_datagram(bytes({2})));
    ASSERT_TRUE(client->send_datagram(bytes({3})));
    const auto events = created.listener->poll(16);
    ASSERT_EQ(events.size(), 3u);
    ASSERT_TRUE(std::holds_alternative<DatagramEvent>(events[0]));
    ASSERT_TRUE(std::holds_alternative<DatagramEvent>(events[1]));
    EXPECT_EQ(std::get<DatagramEvent>(events[0]).data, bytes({1}));
    EXPECT_EQ(std::get<DatagramEvent>(events[1]).data, bytes({2}));
    EXPECT_TRUE(std::holds_alternative<EventQueueOverflowEvent>(events[2]));
    EXPECT_TRUE(created.listener->poll(16).empty());
    EXPECT_EQ(created.listener->open_bidi().status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->send_datagram(bytes({4})).status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->close(1, {}).status,
              TransportStatus::ConnectionClosed);

    ASSERT_TRUE(pump_until(*client, [&] {
        created.listener->poll(16);
        return client->peer_close().has_value();
    }));
    const auto close = client->peer_close();
    ASSERT_TRUE(close.has_value());
    EXPECT_FALSE(close->application);
    EXPECT_EQ(close->error_code, 1u);
    EXPECT_EQ(close->reason,
              bytes({'e', 'v', 'e', 'n', 't', ' ', 'q', 'u', 'e', 'u', 'e',
                     ' ', 'o', 'v', 'e', 'r', 'f', 'l', 'o', 'w'}));
}

TEST(NativeQuicLive, EventPayloadOverflowPreservesEvidenceAndClosesTransport) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.max_events = 8;
    config.max_event_payload_bytes = 64;
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));

    const std::vector<std::byte> preserved(64, std::byte{1});
    ASSERT_TRUE(client->send_datagram(preserved));
    ASSERT_TRUE(client->send_datagram(bytes({2})));
    const auto events = created.listener->poll(16);
    ASSERT_EQ(events.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<DatagramEvent>(events[0]));
    EXPECT_EQ(std::get<DatagramEvent>(events[0]).data, preserved);
    EXPECT_TRUE(std::holds_alternative<EventQueueOverflowEvent>(events[1]));
    EXPECT_TRUE(created.listener->poll(16).empty());
    EXPECT_EQ(created.listener->open_uni().status,
              TransportStatus::ConnectionClosed);
    EXPECT_EQ(created.listener->close(1, {}).status,
              TransportStatus::ConnectionClosed);

    ASSERT_TRUE(pump_until(*client, [&] {
        created.listener->poll(16);
        return client->peer_close().has_value();
    }));
    const auto close = client->peer_close();
    ASSERT_TRUE(close.has_value());
    EXPECT_FALSE(close->application);
    EXPECT_EQ(close->error_code, 1u);
    EXPECT_EQ(close->reason,
              bytes({'e', 'v', 'e', 'n', 't', ' ', 'q', 'u', 'e', 'u', 'e',
                     ' ', 'o', 'v', 'e', 'r', 'f', 'l', 'o', 'w'}));
}

TEST(NativeQuicLive, EstablishmentPayloadOverflowClosesWithoutEstablishing) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.max_event_payload_bytes = 1;
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);

    std::vector<TransportEvent> events;
    bool overflow = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        auto next = created.listener->poll(16);
        overflow |= std::ranges::any_of(next, [](const auto& event) {
            return std::holds_alternative<EventQueueOverflowEvent>(event);
        });
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        return overflow;
    }));
    ASSERT_TRUE(client->pump());
    EXPECT_EQ(std::ranges::count_if(events, [](const auto& event) {
                  return std::holds_alternative<ConnectionEstablishedEvent>(
                      event);
              }),
              0);
    EXPECT_EQ(std::ranges::count_if(events, [](const auto& event) {
                  return std::holds_alternative<EventQueueOverflowEvent>(event);
              }),
              1);
    EXPECT_EQ(created.listener->open_bidi().status,
              TransportStatus::ConnectionClosed);
    EXPECT_TRUE(created.listener->poll(16).empty());
    const auto close = client->peer_close();
    ASSERT_TRUE(close.has_value());
    EXPECT_FALSE(close->application);
    EXPECT_EQ(close->error_code, 1u);
    EXPECT_EQ(close->reason,
              bytes({'e', 'v', 'e', 'n', 't', ' ', 'q', 'u', 'e', 'u', 'e',
                     ' ', 'o', 'v', 'e', 'r', 'f', 'l', 'o', 'w'}));
}

TEST(NativeQuicLive, SimultaneousCloseClassifiesExactlyOnce) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    ASSERT_TRUE(client->close(81, bytes({1, 9, 2})));
    ASSERT_EQ(created.listener->close(82, bytes({3, 9, 4})).status,
              TransportStatus::Success);
    std::vector<TransportEvent> events;
    ASSERT_TRUE(pump_until(*client, [&] {
        auto next = created.listener->poll(16);
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        return std::ranges::any_of(events, terminal_event);
    }));
    const auto terminal_count = static_cast<std::size_t>(std::ranges::count_if(
        events, terminal_event));
    ASSERT_EQ(terminal_count, 1u);
    const auto server_close = std::ranges::find_if(events, terminal_event);
    ASSERT_TRUE(std::holds_alternative<LocalCloseEvent>(*server_close));
    const auto& local = std::get<LocalCloseEvent>(*server_close);
    EXPECT_EQ(local.error_space, CloseErrorSpace::Application);
    EXPECT_EQ(local.error_code, 82u);
    EXPECT_EQ(local.reason, bytes({3, 9, 4}));
    EXPECT_EQ(created.listener->close(83, {}).status,
              TransportStatus::ConnectionClosed);
    EXPECT_TRUE(created.listener->poll(16).empty());
}

TEST(NativeQuicLive, CloseAfterFinAndResetPreservesEvidenceBeforeTerminal) {
    TestPemFiles pem;
    for (const bool reset_case : {false, true}) {
        auto created = NativeQuicListener::create(live_config(pem));
        ASSERT_NE(created.listener, nullptr);
        auto client = test::PicoquicTestClient::create(
            {.port = created.listener->bound_endpoint().port,
             .alpn = expected_alpn()});
        ASSERT_NE(client, nullptr);
        bool established = false;
        ASSERT_TRUE(pump_until(*client, [&] {
            for (const auto& event : created.listener->poll(16)) {
                established |=
                    std::holds_alternative<ConnectionEstablishedEvent>(event);
            }
            return established;
        }));
        ASSERT_TRUE(client->send_stream(0, bytes({5, 0, 6}), !reset_case));
        if (reset_case) {
            ASSERT_TRUE(client->reset_stream(0, 91));
        }
        ASSERT_TRUE(client->close(reset_case ? 93 : 92,
                                  reset_case ? bytes({9, 3}) : bytes({9, 2})));

        std::vector<TransportEvent> events;
        ASSERT_TRUE(pump_until(*client, [&] {
            auto next = created.listener->poll(16);
            events.insert(events.end(), std::make_move_iterator(next.begin()),
                          std::make_move_iterator(next.end()));
            return std::ranges::any_of(events, terminal_event);
        }));
        EXPECT_EQ(std::ranges::count_if(events, terminal_event), 1);
        const auto close = std::ranges::find_if(events, terminal_event);
        ASSERT_TRUE(std::holds_alternative<PeerCloseEvent>(*close));
        const auto& peer_close = std::get<PeerCloseEvent>(*close);
        EXPECT_EQ(peer_close.error_space, CloseErrorSpace::Application);
        EXPECT_EQ(peer_close.error_code, reset_case ? 93u : 92u);
        EXPECT_EQ(peer_close.reason,
                  reset_case ? bytes({9, 3}) : bytes({9, 2}));
        if (reset_case) {
            const auto reset = std::ranges::find_if(events, [](const auto& event) {
                return std::holds_alternative<PeerResetEvent>(event);
            });
            ASSERT_NE(reset, events.end());
            EXPECT_EQ(std::get<PeerResetEvent>(*reset).stream_id, 0u);
            EXPECT_EQ(std::get<PeerResetEvent>(*reset).application_error, 91u);
        } else {
            const auto stream = std::ranges::find_if(events, [](const auto& event) {
                const auto* data = std::get_if<StreamDataEvent>(&event);
                return data != nullptr && data->stream_id == 0 && data->fin;
            });
            ASSERT_NE(stream, events.end());
            EXPECT_EQ(std::get<StreamDataEvent>(*stream).data,
                      bytes({5, 0, 6}));
        }
        EXPECT_TRUE(created.listener->poll(16).empty());
    }
}

TEST(NativeQuicLive, ServerStreamReservationsUsePeerStreamCounts) {
    TestPemFiles pem;
    for (const std::uint64_t credit : {0U, 1U, 64U}) {
        auto created = NativeQuicListener::create(live_config(pem));
        ASSERT_NE(created.listener, nullptr);
        auto client = test::PicoquicTestClient::create({
            .port = created.listener->bound_endpoint().port, .alpn = expected_alpn(),
            .initial_max_streams_bidi = credit, .initial_max_streams_uni = credit});
        ASSERT_NE(client, nullptr);
        bool established = false;
        ASSERT_TRUE(pump_until(*client, [&] {
            for (const auto& event : created.listener->poll(8)) {
                established |= std::holds_alternative<ConnectionEstablishedEvent>(event);
            }
            return established && client->established();
        }));
        for (std::uint64_t rank = 0; rank < credit; ++rank) {
            const auto bidi = created.listener->open_bidi();
            const auto uni = created.listener->open_uni();
            ASSERT_EQ(bidi.status, TransportStatus::Success);
            ASSERT_EQ(uni.status, TransportStatus::Success);
            EXPECT_EQ(bidi.stream_id, 1 + 4 * rank);
            EXPECT_EQ(uni.stream_id, 3 + 4 * rank);
        }
        EXPECT_EQ(created.listener->open_bidi().status, TransportStatus::StreamLimit);
        EXPECT_EQ(created.listener->open_uni().status, TransportStatus::StreamLimit);
    }
}

TEST(NativeQuicLive, InvalidOperationInputsDoNotMutateEstablishedSession) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create({
        .port = created.listener->bound_endpoint().port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |= std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established && client->established();
    }));
    const auto uni = created.listener->open_uni();
    ASSERT_EQ(uni.status, TransportStatus::Success);
    ASSERT_EQ(created.listener->write(uni.stream_id, bytes({1}), false).status,
              TransportStatus::Success);
    EXPECT_EQ(created.listener->stop_sending(uni.stream_id, 9).status,
              TransportStatus::InvalidState);
    const auto bidi = created.listener->open_bidi();
    ASSERT_EQ(bidi.status, TransportStatus::Success);
    const auto invalid_code = std::uint64_t{1} << 62u;
    EXPECT_EQ(created.listener->reset(bidi.stream_id, invalid_code).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->stop_sending(bidi.stream_id, invalid_code).status,
              TransportStatus::InvalidState);
    EXPECT_EQ(created.listener->close(invalid_code, {}).status, TransportStatus::InvalidState);
    ASSERT_EQ(created.listener->write(bidi.stream_id, bytes({2}), true).status,
              TransportStatus::Success);
    ASSERT_TRUE(pump_until(*client, [&] {
        created.listener->poll(8);
        const auto observed = client->stream(bidi.stream_id);
        return observed && observed->fin;
    }));
    EXPECT_EQ(client->stream(bidi.stream_id)->data, bytes({2}));
    EXPECT_FALSE(client->peer_close());
}

TEST(NativeQuicLive, LocalTransportFailureNeverInventsPeerCloseEvidence) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create({
        .port = created.listener->bound_endpoint().port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |= std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established && client->established();
    }));
    ASSERT_TRUE(client->send_invalid_transport_frame());
    std::vector<TransportEvent> events;
    bool terminal = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        auto next = created.listener->poll(8);
        terminal |= std::ranges::any_of(next, terminal_event);
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        return terminal;
    }));
    EXPECT_EQ(std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<TransportErrorEvent>(event);
    }), 1);
    EXPECT_EQ(std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<PeerCloseEvent>(event);
    }), 0);
    EXPECT_EQ(created.listener->open_bidi().status, TransportStatus::ConnectionClosed);
}

TEST(NativeQuicLive, ZeroCodeTransportCloseRemainsActualPeerEvidence) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create({
        .port = created.listener->bound_endpoint().port, .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |= std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established && client->established();
    }));
    ASSERT_TRUE(client->send_transport_close_frame());
    std::vector<TransportEvent> events;
    bool terminal = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        auto next = created.listener->poll(8);
        terminal |= std::ranges::any_of(next, terminal_event);
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        return terminal;
    }));
    EXPECT_EQ(std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<TransportErrorEvent>(event);
    }), 0);
    EXPECT_EQ(std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<PeerCloseEvent>(event);
    }), 1);
    const auto found = std::ranges::find_if(events, [](const auto& event) {
        return std::holds_alternative<PeerCloseEvent>(event);
    });
    ASSERT_NE(found, events.end());
    const auto& close = std::get<PeerCloseEvent>(*found);
    EXPECT_EQ(close.error_space, CloseErrorSpace::Transport);
    EXPECT_EQ(close.error_code, 0U);
    EXPECT_TRUE(close.reason.empty());
    EXPECT_EQ(created.listener->open_bidi().status, TransportStatus::ConnectionClosed);
}

TEST(NativeQuicLive, ServerWriteBackpressurePreservesEveryByteAndSingleFin) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.max_queued_send_bytes = 64 * 1024;
    auto created = NativeQuicListener::create(config);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::PicoquicTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(16)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    const auto opened = created.listener->open_bidi();
    ASSERT_EQ(opened.status, TransportStatus::Success);
    std::vector<std::byte> payload(400'000);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>(index % 251u);
    }
    std::size_t offset = 0;
    bool saw_backpressure = false;
    for (std::size_t iteration = 0; iteration < 2000 && offset < payload.size();
         ++iteration) {
        const auto result = created.listener->write(
            opened.stream_id,
            std::span<const std::byte>(payload).subspan(offset), false);
        ASSERT_TRUE(result.status == TransportStatus::Success ||
                    result.status == TransportStatus::Partial ||
                    result.status == TransportStatus::WouldBlock);
        ASSERT_LE(result.accepted, payload.size() - offset);
        offset += result.accepted;
        saw_backpressure |= result.status == TransportStatus::Partial ||
                            result.status == TransportStatus::WouldBlock;
        ASSERT_TRUE(client->pump());
        created.listener->poll(16);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(offset, payload.size());
    ASSERT_TRUE(saw_backpressure);
    ASSERT_EQ(created.listener->write(opened.stream_id, {}, true).status,
              TransportStatus::Success);
    ASSERT_TRUE(pump_until(*client, [&] {
        created.listener->poll(16);
        const auto stream = client->stream(opened.stream_id);
        return stream && stream->fin;
    }));
    const auto stream = client->stream(opened.stream_id);
    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(stream->data, payload);
    EXPECT_EQ(stream->fin_count, 1u);
}

TEST(NativeQuicLifecycle, EstablishedAndClosingDestructionReleasePort) {
    TestPemFiles pem;
    for (const bool close_first : {false, true}) {
        SCOPED_TRACE(close_first);
        auto config = live_config(pem);
        std::vector<std::vector<std::byte>> connection_ids;
        std::uint16_t port = 0;
        for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
            config.bind_port = port;
            auto created = NativeQuicListener::create(config);
            ASSERT_NE(created.listener, nullptr);
            if (ordinal == 0) port = created.listener->bound_endpoint().port;
            EXPECT_EQ(created.listener->bound_endpoint().port, port);
            auto client = test::PicoquicTestClient::create({.port = port, .alpn = expected_alpn()});
            ASSERT_NE(client, nullptr);
            bool established = false;
            ASSERT_TRUE(pump_until(*client, [&] {
                for (const auto& event : created.listener->poll(16)) {
                    if (const auto* ready = std::get_if<ConnectionEstablishedEvent>(&event)) {
                        connection_ids.push_back(ready->local_connection_id);
                        established = true;
                    }
                }
                return established;
            }));
            // New engine/session stream numbering restarts on the same UDP port.
            const auto opened = created.listener->open_bidi();
            ASSERT_EQ(opened.status, TransportStatus::Success);
            EXPECT_EQ(opened.stream_id, 1u);
            const auto payload = bytes({ordinal + 1, 42, 43});
            ASSERT_EQ(created.listener->write(opened.stream_id, payload, true).accepted, payload.size());
            ASSERT_TRUE(pump_until(*client, [&] {
                created.listener->poll(16);
                const auto received = client->stream(1);
                return received && received->fin && received->data == payload;
            }));
            ASSERT_TRUE(client->send_stream(0, payload, true));
            std::vector<std::byte> actual_peer_bytes;
            bool peer_fin = false;
            ASSERT_TRUE(pump_until(*client, [&] {
                for (const auto& event : created.listener->poll(16)) {
                    if (const auto* received = std::get_if<StreamDataEvent>(&event);
                        received && received->stream_id == 0) {
                        actual_peer_bytes.insert(actual_peer_bytes.end(), received->data.begin(), received->data.end());
                        peer_fin |= received->fin;
                    }
                }
                return peer_fin && actual_peer_bytes == payload;
            }));
            if (close_first) {
                ASSERT_TRUE(client->close(101, {}));
                bool peer_closed = false;
                ASSERT_TRUE(pump_until(*client, [&] {
                    for (const auto& event : created.listener->poll(16))
                        peer_closed |= std::holds_alternative<PeerCloseEvent>(event);
                    return peer_closed;
                }));
            }
            created.listener.reset();
        }
        ASSERT_EQ(connection_ids.size(), 2u);
        EXPECT_FALSE(connection_ids[0].empty());
        EXPECT_FALSE(connection_ids[1].empty());
        EXPECT_NE(connection_ids[0], connection_ids[1]);
    }
}

TEST(NativeQuicLive, SubscriberNotifyDirectionUsesBothRealSubscriptionOrigins) {
    TestPemFiles pem;
    const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source=requirements::load_draft_source(21,root / "docs",root / "requirements/draft-digests.json");
    const auto d21=std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(source,root / "requirements/draft21.json"));
    const auto d18=std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{18,"unused",true,{}});
    for (const auto& profile : scenarios::draft21_close_probes(std::chrono::milliseconds(1500),{bytes({'n'})},bytes({'t'}))) {
        if (profile.requirement_id!="D21-9-10-MUST-370") continue;
        const bool publish=profile.definition.writes.front().channel==scenarios::RawProbeChannel::PeerBidi;
        for (const unsigned close_code : {3u,9u}) {
            SCOPED_TRACE(profile.definition.id);
            SCOPED_TRACE(close_code);
            auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
            app::NativeRunManager manager(d18,d21,store,{.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
                .port_start=0,.port_end=0,.maximum_active_runs=1,.certificate_path=pem.certificate(),.private_key_path=pem.key()});
            const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
                app::RunMode::Observed,{profile.definition.id},std::chrono::milliseconds(1500),app::TrackFixture{{"n"},"t"}});
            ASSERT_EQ(started.status,app::RunStartStatus::Started);
            auto client=test::PicoquicTestClient::create({.port=started.endpoint.port,.alpn=bytes({'m','o','q','t','-','2','1'})});
            ASSERT_NE(client,nullptr);
            ASSERT_TRUE(pump_until(*client,[&] { const auto setup=client->stream(3); return setup && setup->data.size()==4; }));
            ASSERT_TRUE(client->send_stream(2,bytes({0xaf,0,0,0}),false));
            const auto peer_open=bytes({0x1d,0,10,0,1,1,'n',1,'t',0,0,4,1});
            const auto ack=bytes({4,0,4,0,0,4,1});
            const auto stream=publish ? 0u : 1u;
            if (publish) {
                ASSERT_TRUE(client->send_stream(stream,std::span(peer_open).first(5),false));
            } else {
                ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(stream); return request && request->data.size()==profile.definition.writes.front().bytes.size(); }));
                EXPECT_EQ(client->stream(stream)->data,profile.definition.writes.front().bytes);
                EXPECT_FALSE(client->stream(stream)->fin);
                ASSERT_TRUE(client->send_stream(stream,std::span(ack).first(2),false));
            }
            const auto partial_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now()<partial_deadline) {
                ASSERT_TRUE(client->pump()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (publish) {
                const auto response=client->stream(stream);
                EXPECT_TRUE(!response || response->data.empty());
                ASSERT_TRUE(client->send_stream(stream,std::span(peer_open).subspan(5),false));
            } else {
                EXPECT_EQ(client->stream(stream)->data,profile.definition.writes.front().bytes);
                ASSERT_TRUE(client->send_stream(stream,std::span(ack).subspan(2),false));
            }
            auto expected=profile.definition.writes.front().bytes;
            expected.insert(expected.end(),profile.definition.writes.back().bytes.begin(),profile.definition.writes.back().bytes.end());
            ASSERT_TRUE(pump_until(*client,[&] { const auto response=client->stream(stream); return response && response->data.size()==expected.size(); }));
            EXPECT_EQ(client->stream(stream)->data,expected);
            EXPECT_FALSE(client->stream(stream)->fin);
            ASSERT_TRUE(client->close(close_code,{}));
            ASSERT_TRUE(pump_until(*client,[&] { return store->load(started.id).state==storage::RunState::Finalized; }));
            const auto run=store->load(started.id);
            const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[](const auto& outcome) { return outcome.requirement_id=="D21-9-10-MUST-370"; });
            ASSERT_NE(row,run.outcomes.end());
            EXPECT_EQ(row->state,close_code==3 ? requirements::OutcomeState::NotRun : requirements::OutcomeState::Fail);
            const auto stimulus=std::find_if(run.events.begin(),run.events.end(),[](const auto& event) { return event.kind=="raw_probe_stimulus"; });
            ASSERT_NE(stimulus,run.events.end());
            EXPECT_NE(stimulus->detail.find(" stream="+std::to_string(stream)+" channel="+(publish ? "4" : "1")),std::string::npos);
            EXPECT_NE(stimulus->detail.find("accepted=4 fin=false bytes=22000100 accepted_event_count="),std::string::npos);
            EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
                return event.kind=="peer_close" && event.detail.find("application close code="+std::to_string(close_code)+" transport_event_index=")==0;
            }));
        }
    }
}

TEST(NativeQuicLive, RequestGoawayContextsUseAcknowledgedStreamsAndFreshBarrier) {
    TestPemFiles pem;
    const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto load=[&](unsigned draft) {
        const auto source=requirements::load_draft_source(draft,root / "docs",root / "requirements/draft-digests.json");
        return requirements::RequirementCatalog::load(source,root / (draft==18 ? "requirements/draft18.json" : "requirements/draft21.json"));
    };
    const auto full18=load(18), full21=load(21);
    for (const unsigned draft : {18u,21u}) {
        const auto profiles=draft==18 ? scenarios::draft18_request_goaway_probes(std::chrono::milliseconds(1500))
                                      : scenarios::draft21_request_goaway_probes(std::chrono::milliseconds(1500));
        for (const auto& profile : profiles) for (const bool alternate : {false,true}) {
            SCOPED_TRACE(profile.definition.id);
            SCOPED_TRACE(alternate);
            auto scoped18=full18, scoped21=full21;
            // Verify executor completion for each context; the protocol test
            // separately requires both contexts against the unchanged full catalog.
            auto& scoped=draft==18 ? scoped18 : scoped21;
            for (auto& row : scoped.requirements) if (row.id==profile.requirement_id) row.scenarios={profile.definition.id};
            auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
            app::NativeRunManager manager(std::make_shared<const requirements::RequirementCatalog>(std::move(scoped18)),
                std::make_shared<const requirements::RequirementCatalog>(std::move(scoped21)),store,
                {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",.port_start=0,.port_end=0,.maximum_active_runs=1,
                 .certificate_path=pem.certificate(),.private_key_path=pem.key()});
            const auto started=manager.start({static_cast<app::DraftVersion>(draft),app::TransportKind::NativeQuic,
                app::RunMode::Observed,{profile.definition.id},std::chrono::milliseconds(1500),std::nullopt});
            ASSERT_EQ(started.status,app::RunStartStatus::Started);
            auto client=test::PicoquicTestClient::create({.port=started.endpoint.port,
                .alpn=draft==18 ? expected_alpn() : bytes({'m','o','q','t','-','2','1'})});
            ASSERT_NE(client,nullptr);
            ASSERT_TRUE(pump_until(*client,[&] { const auto setup=client->stream(3); return setup && setup->data.size()==4; }));
            ASSERT_TRUE(client->send_stream(2,bytes({0xaf,0,0,0}),false));
            const auto& opening=profile.definition.writes.front().bytes;
            ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(1); return request && request->data.size()==opening.size(); }));
            EXPECT_EQ(client->stream(1)->data,opening);
            if (!profile.duplicate) {
                ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(5); return request && request->data.size()==profile.definition.writes[1].bytes.size(); }));
                EXPECT_EQ(client->stream(5)->data,profile.definition.writes[1].bytes);
                ASSERT_TRUE(client->send_stream(5,bytes({7,0,1,0}),false));
            }
            ASSERT_TRUE(client->send_stream(1,bytes({7,0}),false));
            const auto partial_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now()<partial_deadline) {
                ASSERT_TRUE(client->pump()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_EQ(client->stream(1)->data,opening);
            ASSERT_TRUE(client->send_stream(1,bytes({1,0}),false));
            const auto goaway=bytes({0x10,0,3,0,0xa7,0x10});
            auto expected=opening;
            expected.insert(expected.end(),goaway.begin(),goaway.end());
            if (profile.duplicate) expected.insert(expected.end(),goaway.begin(),goaway.end());
            ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(1); return request && request->data.size()==expected.size(); }));
            EXPECT_EQ(client->stream(1)->data,expected);
            EXPECT_FALSE(client->stream(1)->fin);
            if (profile.duplicate) {
                ASSERT_TRUE(client->close(alternate ? 9 : 3,{}));
            } else {
                auto second=profile.definition.writes[1].bytes;
                second.insert(second.end(),goaway.begin(),goaway.end());
                ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(5); return request && request->data.size()==second.size(); }));
                EXPECT_EQ(client->stream(5)->data,second);
                ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(9); return request && request->data.size()==profile.definition.writes.back().bytes.size(); }));
                EXPECT_EQ(client->stream(9)->data,profile.definition.writes.back().bytes);
                if (alternate) { ASSERT_TRUE(client->reset_stream(1,4)); }
                ASSERT_TRUE(client->send_stream(9,alternate ? bytes({5,0,3,4,0,0}) : bytes({7,0,1,0}),alternate));
            }
            ASSERT_TRUE(pump_until(*client,[&] { return store->load(started.id).state==storage::RunState::Finalized; }));
            const auto run=store->load(started.id);
            const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[&](const auto& outcome) { return outcome.requirement_id==profile.requirement_id; });
            ASSERT_NE(row,run.outcomes.end());
            EXPECT_EQ(row->state,profile.duplicate && alternate ? requirements::OutcomeState::Fail : requirements::OutcomeState::Pass);
            const auto stimulus=std::find_if(run.events.begin(),run.events.end(),[](const auto& event) { return event.kind=="raw_probe_stimulus"; });
            ASSERT_NE(stimulus,run.events.end());
            EXPECT_NE(stimulus->detail.find("accepted=6 fin=false bytes=10000300a710 accepted_event_count="),std::string::npos);
            if (!profile.duplicate) {
                EXPECT_NE(stimulus->detail.find(" stream=9 channel=1 operation=write"),std::string::npos);
                EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) { return event.kind=="raw_probe_transport_event" && event.stream_id=="9"; }));
            }
        }
    }
}

TEST(NativeQuicLive, NotifyOnUnsupportedRequestUsesAcknowledgedStreamAndPersistsClose) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(21,root / "docs",root / "requirements/draft-digests.json");
    const auto d21 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(source,root / "requirements/draft21.json"));
    const auto d18 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{18,"unused",true,{}});
    for (const auto& profile : scenarios::draft21_close_probes(std::chrono::milliseconds(1500),{bytes({'n'})},bytes({'t'}))) {
        if (profile.requirement_id != "D21-9-10-MUST-369") continue;
        for (const unsigned close_code : {3u,4u}) {
            SCOPED_TRACE(profile.definition.id);
            SCOPED_TRACE(close_code);
            auto store = std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
            app::NativeRunManager manager(d18,d21,store,{.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
                .port_start=0,.port_end=0,.maximum_active_runs=1,.certificate_path=pem.certificate(),.private_key_path=pem.key()});
            const auto started = manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
                app::RunMode::Observed,{profile.definition.id},std::chrono::milliseconds(1500),app::TrackFixture{{"n"},"t"}});
            ASSERT_EQ(started.status,app::RunStartStatus::Started);
            auto client = test::PicoquicTestClient::create({.port=started.endpoint.port,.alpn=bytes({'m','o','q','t','-','2','1'})});
            ASSERT_NE(client,nullptr);
            ASSERT_TRUE(pump_until(*client,[&] { const auto setup=client->stream(3); return setup && setup->data.size()==4; }));
            ASSERT_TRUE(client->send_stream(2,bytes({0xaf,0,0,0}),false));
            const auto& opening = profile.definition.writes.front().bytes;
            ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(1); return request && request->data.size()==opening.size(); }));
            EXPECT_EQ(client->stream(1)->data,opening);
            EXPECT_FALSE(client->stream(1)->fin);
            const auto ack = profile.definition.id == "d21-publish-state-notify-on-fetch"
                ? bytes({0x18,0,4,0,7,9,0}) : bytes({7,0,1,0});
            ASSERT_TRUE(client->send_stream(1,std::span(ack).first(2),false));
            const auto partial_deadline = std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now()<partial_deadline) {
                ASSERT_TRUE(client->pump());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_EQ(client->stream(1)->data,opening);
            ASSERT_TRUE(client->send_stream(1,std::span(ack).subspan(2),false));
            auto expected = opening;
            const auto& notify = profile.definition.writes.back().bytes;
            EXPECT_EQ(notify,bytes({0x22,0,1,0}));
            expected.insert(expected.end(),notify.begin(),notify.end());
            ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(1); return request && request->data.size()==expected.size(); }));
            EXPECT_EQ(client->stream(1)->data,expected);
            EXPECT_FALSE(client->stream(1)->fin);
            ASSERT_TRUE(client->close(close_code,{}));
            ASSERT_TRUE(pump_until(*client,[&] { return store->load(started.id).state==storage::RunState::Finalized; }));
            const auto run = store->load(started.id);
            const auto outcome = std::find_if(run.outcomes.begin(),run.outcomes.end(),[](const auto& row) { return row.requirement_id=="D21-9-10-MUST-369"; });
            ASSERT_NE(outcome,run.outcomes.end());
            // A correct reply to one context cannot pass the two-context catalog row.
            EXPECT_EQ(outcome->state,close_code==3 ? requirements::OutcomeState::NotRun : requirements::OutcomeState::Fail);
            const auto stimulus = std::find_if(run.events.begin(),run.events.end(),[](const auto& event) { return event.kind=="raw_probe_stimulus"; });
            ASSERT_NE(stimulus,run.events.end());
            EXPECT_NE(stimulus->detail.find("accepted=4 fin=false bytes=22000100 accepted_event_count="),std::string::npos);
            const auto close = std::find_if(run.events.begin(),run.events.end(),[&](const auto& event) {
                return event.kind=="peer_close" && event.detail.find(
                    "application close code="+std::to_string(close_code)+" transport_event_index=")==0;
            });
            EXPECT_NE(close,run.events.end());
        }
    }
}

TEST(NativeQuicLive, FetchGroupOrderingUsesAllRequestedStreamsAndConfiguredTrack) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto load = [&](unsigned draft) {
        const auto source = requirements::load_draft_source(draft,root / "docs",root / "requirements/draft-digests.json");
        return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source,root / (draft == 18 ? "requirements/draft18.json" : "requirements/draft21.json")));
    };
    const auto d18 = load(18), d21 = load(21);
    for (const unsigned draft : {18u,21u}) {
        for (const auto& fixture : std::vector<app::TrackFixture>{{{"n"},"t"},{{"a","b"},"v"}}) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : fixture.namespace_fields)
                fields.emplace_back(reinterpret_cast<const std::byte*>(field.data()),reinterpret_cast<const std::byte*>(field.data()+field.size()));
            const std::vector<std::byte> name(reinterpret_cast<const std::byte*>(fixture.track_name.data()),reinterpret_cast<const std::byte*>(fixture.track_name.data()+fixture.track_name.size()));
            const auto profiles = draft == 18 ? scenarios::draft18_fetch_group_order_probes(std::chrono::milliseconds(1500),fields,name)
                                              : scenarios::draft21_fetch_group_order_probes(std::chrono::milliseconds(1500),fields,name);
            for (const auto& profile : profiles) for (const bool reversed : {false,true}) {
                SCOPED_TRACE(profile.definition.id);
                SCOPED_TRACE(reversed);
                auto store = std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
                app::NativeRunManager manager(d18,d21,store,{.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
                    .port_start=0,.port_end=0,.maximum_active_runs=1,.certificate_path=pem.certificate(),.private_key_path=pem.key()});
                const auto started = manager.start({static_cast<app::DraftVersion>(draft),app::TransportKind::NativeQuic,
                    app::RunMode::Observed,{profile.definition.id},std::chrono::milliseconds(1500),fixture});
                ASSERT_EQ(started.status,app::RunStartStatus::Started);
                auto client = test::PicoquicTestClient::create({.port=started.endpoint.port,
                    .alpn=draft == 18 ? expected_alpn() : bytes({'m','o','q','t','-','2','1'})});
                ASSERT_NE(client,nullptr);
                ASSERT_TRUE(pump_until(*client,[&] { const auto setup=client->stream(3); return setup && setup->data.size()==4; }));
                ASSERT_TRUE(client->send_stream(2,bytes({0xaf,0,0,0}),false));
                for (std::size_t index=0;index<profile.definition.writes.size();++index) {
                    const auto stream = 1+4*index;
                    const auto& opening = profile.definition.writes[index].bytes;
                    ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(stream); return request && request->data.size()==opening.size() && request->fin; }));
                    EXPECT_EQ(client->stream(stream)->data,opening);
                    ASSERT_TRUE(client->send_stream(stream,bytes({0x18,0,4,0,9,9,0}),true));
                }
                for (std::size_t index=0;index<profile.definition.writes.size();++index) {
                    const bool descending = profile.order==scenarios::FetchGroupOrderRequest::Descending ||
                        (profile.order==scenarios::FetchGroupOrderRequest::BothExplicit && index==1);
                    const unsigned request_id=static_cast<unsigned>(1+2*index);
                    const auto object = reversed && index==0
                        ? bytes({5,request_id,0x1c,descending ? 7u : 9u,0,99,1,42,
                                 0x81,0x0c,descending ? 9u : 7u,0,0x04,1,1,43})
                        : bytes({5,request_id,0x1c,descending ? 9u : 7u,0,99,1,42,0x0c,0,0,1,43});
                    ASSERT_TRUE(client->send_stream(6+4*index,std::span(object).first(1),false));
                    ASSERT_TRUE(client->send_stream(6+4*index,std::span(object).subspan(1),true));
                }
                ASSERT_TRUE(pump_until(*client,[&] { return store->load(started.id).state==storage::RunState::Finalized; }));
                const auto run = store->load(started.id);
                const auto outcome = std::find_if(run.outcomes.begin(),run.outcomes.end(),[&](const auto& row) { return row.requirement_id==profile.requirement_id; });
                ASSERT_NE(outcome,run.outcomes.end());
                EXPECT_EQ(outcome->state,reversed ? requirements::OutcomeState::Fail : draft==18 ? requirements::OutcomeState::Pass : requirements::OutcomeState::NotRun);
                const auto stimulus = std::find_if(run.events.begin(),run.events.end(),[](const auto& event) { return event.kind=="raw_probe_stimulus"; });
                ASSERT_NE(stimulus,run.events.end());
                for (std::size_t index=0;index<profile.definition.writes.size();++index) {
                    std::string hex;
                    constexpr char digits[]="0123456789abcdef";
                    for (const auto byte : profile.definition.writes[index].bytes) {
                        const auto value=std::to_integer<unsigned>(byte);
                        hex+=digits[value>>4]; hex+=digits[value&15];
                    }
                    const auto prefix=" stream="+std::to_string(1+4*index)+" channel=1 operation=write application_error=0 operation_accepted=false accepted="+
                        std::to_string(profile.definition.writes[index].bytes.size())+" fin=true bytes="+hex+" accepted_event_count=";
                    const auto position=stimulus->detail.find(prefix);
                    ASSERT_NE(position,std::string::npos);
                    const auto marker=position+prefix.size();
                    ASSERT_LT(marker,stimulus->detail.size());
                    EXPECT_GE(stimulus->detail[marker],'0'); EXPECT_LE(stimulus->detail[marker],'9');
                }
            }
        }
    }
}

TEST(NativeQuicLive, FirstFetchObjectFieldsUseRealCatalogAndActualConfiguredTrack) {
    TestPemFiles pem;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto load = [&](unsigned draft) {
        const auto source = requirements::load_draft_source(draft,root / "docs",root / "requirements/draft-digests.json");
        return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
            source,root / (draft == 18 ? "requirements/draft18.json" : "requirements/draft21.json")));
    };
    const auto d18 = load(18), d21 = load(21);
    for (const unsigned draft : {18u,21u}) {
        for (const auto& fixture : std::vector<app::TrackFixture>{{{"n"},"t"},{{"a","b"},"v"}}) {
            std::vector<std::vector<std::byte>> fields;
            for (const auto& field : fixture.namespace_fields)
                fields.emplace_back(reinterpret_cast<const std::byte*>(field.data()),reinterpret_cast<const std::byte*>(field.data() + field.size()));
            const std::vector<std::byte> name(reinterpret_cast<const std::byte*>(fixture.track_name.data()),
                                              reinterpret_cast<const std::byte*>(fixture.track_name.data() + fixture.track_name.size()));
            const auto profiles = draft == 18 ? scenarios::draft18_fetch_first_object_probes(std::chrono::milliseconds(1500),fields,name)
                                              : scenarios::draft21_fetch_first_object_probes(std::chrono::milliseconds(1500),fields,name);
            ASSERT_EQ(profiles.size(),2u);
            for (const unsigned missing : {0u,8u,4u}) {
                SCOPED_TRACE(draft);
                SCOPED_TRACE(missing);
                auto store = std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
                app::NativeRunManager manager(d18,d21,store,{.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
                    .port_start=0,.port_end=0,.maximum_active_runs=1,.certificate_path=pem.certificate(),.private_key_path=pem.key()});
                const auto started = manager.start({static_cast<app::DraftVersion>(draft),app::TransportKind::NativeQuic,
                    app::RunMode::Observed,{profiles.front().definition.id},std::chrono::milliseconds(1500),fixture});
                ASSERT_EQ(started.status,app::RunStartStatus::Started);
                auto client = test::PicoquicTestClient::create({.port=started.endpoint.port,
                    .alpn=draft == 18 ? expected_alpn() : bytes({'m','o','q','t','-','2','1'})});
                ASSERT_NE(client,nullptr);
                ASSERT_TRUE(pump_until(*client,[&] { const auto setup=client->stream(3); return setup && setup->data.size()==4; }));
                ASSERT_TRUE(client->send_stream(2,bytes({0xaf,0,0,0}),false));
                const auto& opening = profiles.front().definition.writes.front().bytes;
                ASSERT_TRUE(pump_until(*client,[&] { const auto request=client->stream(1); return request && request->data.size()==opening.size() && request->fin; }));
                EXPECT_EQ(client->stream(1)->data,opening);
                const auto ack = bytes({0x18,0,4,0,7,draft == 18 ? 10u : 9u,0});
                const auto object = missing == 0 ? bytes({1,0x1c,7,9,77,1,'v'}) : bytes({1,0x1c & ~missing});
                // Alternate the permitted ACK/object ordering using actual stream bytes.
                if (fixture.namespace_fields.size()==2) {
                    ASSERT_TRUE(client->send_stream(1,ack,true));
                }
                ASSERT_TRUE(client->send_stream(6,bytes({5}),false));
                ASSERT_TRUE(client->send_stream(6,object,missing==0));
                if (fixture.namespace_fields.size()==1) {
                    ASSERT_TRUE(client->send_stream(1,ack,true));
                }
                ASSERT_TRUE(pump_until(*client,[&] { return store->load(started.id).state==storage::RunState::Finalized; }));
                const auto run = store->load(started.id);
                for (const auto& profile : profiles) {
                    const auto outcome = std::find_if(run.outcomes.begin(),run.outcomes.end(),[&](const auto& row) { return row.requirement_id==profile.requirement_id; });
                    ASSERT_NE(outcome,run.outcomes.end());
                    const unsigned bit = profile.field==scenarios::FetchFirstObjectField::Group ? 8u : 4u;
                    EXPECT_EQ(outcome->state,missing==0 ? requirements::OutcomeState::Pass : missing==bit ? requirements::OutcomeState::Fail : requirements::OutcomeState::NotRun);
                }
                const auto stimulus = std::find_if(run.events.begin(),run.events.end(),[](const auto& event) { return event.kind=="raw_probe_stimulus"; });
                ASSERT_NE(stimulus,run.events.end());
                std::string opening_hex;
                constexpr char hex_digits[] = "0123456789abcdef";
                for (const auto byte : opening) {
                    const auto value = std::to_integer<unsigned>(byte);
                    opening_hex += hex_digits[value >> 4];
                    opening_hex += hex_digits[value & 15];
                }
                const auto write_prefix = " stream=1 channel=" +
                    std::to_string(static_cast<unsigned>(scenarios::RawProbeChannel::NewBidi)) +
                    " operation=write application_error=0 operation_accepted=false accepted=" +
                    std::to_string(opening.size()) + " fin=true bytes=" + opening_hex + " accepted_event_count=";
                const auto write_position = stimulus->detail.find(write_prefix);
                ASSERT_NE(write_position,std::string::npos);
                const auto marker_position = write_position + write_prefix.size();
                ASSERT_LT(marker_position,stimulus->detail.size());
                EXPECT_GE(stimulus->detail[marker_position],'0');
                EXPECT_LE(stimulus->detail[marker_position],'9');
            }
        }
    }
}

TEST(NativeQuicConfiguration, FirstFetchRejectsReservedFixtureBeforeStartingListener) {
    auto d18 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{18,"test",true,{}});
    auto d21 = std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{21,"test",true,{}});
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    app::NativeRunManager manager(d18,d21,store,{});
    for (const unsigned draft : {18u,21u}) {
        for (const auto& fixture : std::vector<app::TrackFixture>{{{"."},"x"},{{".session"},""}}) {
            const auto id = draft == 18 ? "fetch-known-first-object-with-nonzero-group-and-object-ids" : "d21-fetch-first-object-flags";
            EXPECT_EQ(manager.start({static_cast<app::DraftVersion>(draft),app::TransportKind::NativeQuic,
                app::RunMode::Observed,{id},std::chrono::milliseconds(1000),fixture}).status,app::RunStartStatus::InvalidConfig);
        }
    }
}

}  // namespace
}  // namespace moq::interop::transport

namespace moq::interop::transport {
namespace {
TEST(NativeQuicLive, DiscoveryOverlapNamedContextsUseActiveRequestsAndActualPrefixUpdates) {
    TestPemFiles pem;
    for(unsigned draft:{18u,21u})for(const auto& fields:std::vector<std::vector<std::vector<std::byte>>>{{bytes({'a'})},{bytes({'m'}),bytes({'r'})}}) {
        auto profiles=draft==18?scenarios::draft18_discovery_overlap_probes(std::chrono::milliseconds(1500),fields):
            scenarios::draft21_discovery_overlap_probes(std::chrono::milliseconds(1500),fields);
        std::set<std::string> seen;
        for(const auto& profile:profiles) {
            if(!seen.insert(profile.definition.id).second)continue;
            for(unsigned reply:{0x30u,0x10u,7u}) {
                SCOPED_TRACE(profile.definition.id);
                SCOPED_TRACE(reply);
                SCOPED_TRACE(fields.size());
                requirements::Requirement row{profile.requirement_id,requirements::Strength::Must,{"test",1,1,1,1},
                    "publisher","reject discovery overlap",requirements::Applicability::Applicable,requirements::Testability::Testable,
                    {profile.definition.id},{profile.evaluator_id},""};
                auto d18=std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{18,"test",true,draft==18?std::vector{row}:std::vector<requirements::Requirement>{}});
                auto d21=std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{21,"test",true,draft==21?std::vector{row}:std::vector<requirements::Requirement>{}});
                auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
                app::NativeRunManager manager(d18,d21,store,{.bind_address="127.0.0.1",.advertised_address="127.0.0.1",.port_start=0,.port_end=0,
                    .maximum_active_runs=1,.certificate_path=pem.certificate(),.private_key_path=pem.key()});
                app::TrackFixture fixture;
                for(const auto& field:fields)fixture.namespace_fields.emplace_back(reinterpret_cast<const char*>(field.data()),field.size());
                fixture.track_name="unused";
                const auto started=manager.start({static_cast<app::DraftVersion>(draft),app::TransportKind::NativeQuic,
                    app::RunMode::Observed,{profile.definition.id},std::chrono::milliseconds(1500),fixture});
                ASSERT_EQ(started.status,app::RunStartStatus::Started);
                auto client=test::PicoquicTestClient::create({.port=started.endpoint.port,.alpn=draft==18?expected_alpn():bytes({'m','o','q','t','-','2','1'})});
                ASSERT_NE(client,nullptr);
                ASSERT_TRUE(pump_until(*client,[&]{auto setup=client->stream(3);return setup&&setup->data.size()==4;}));
                ASSERT_TRUE(client->send_stream(2,bytes({0xaf,0,0,0}),false));
                std::uint64_t next_stream=1;
                std::vector<std::uint64_t> streams;
                std::map<std::uint64_t,std::vector<std::byte>> expected;
                const auto first_challenge=std::find_if(profile.definition.writes.begin(),profile.definition.writes.end(),[](const auto& w){return bool(w.evidence_ready);});
                const auto initial=static_cast<std::size_t>(first_challenge-profile.definition.writes.begin());
                for(std::size_t i=0;i<profile.definition.writes.size();++i) {
                    const auto& write=profile.definition.writes[i];
                    const auto stream=write.reuse_write_stream?streams[*write.reuse_write_stream]:next_stream;
                    if(!write.reuse_write_stream)next_stream+=4;
                    streams.push_back(stream);
                    expected[stream].insert(expected[stream].end(),write.bytes.begin(),write.bytes.end());
                    ASSERT_TRUE(pump_until(*client,[&]{auto request=client->stream(stream);return request&&request->data.size()==expected[stream].size();}));
                    EXPECT_EQ(client->stream(stream)->data,expected[stream]);
                    EXPECT_FALSE(client->stream(stream)->fin);
                    // All initial requests stay active. An incomplete OK cannot open a challenge.
                    if(i<initial) {
                        ASSERT_TRUE(client->send_stream(stream,bytes({7,0}),false));
                        if(i+1==initial) {
                            const auto wait_end=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
                            while(std::chrono::steady_clock::now()<wait_end) { ASSERT_TRUE(client->pump()); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
                            const auto& challenge=profile.definition.writes[initial];
                            if(challenge.reuse_write_stream) {
                                const auto target=streams[*challenge.reuse_write_stream];
                                ASSERT_TRUE(client->stream(target));
                                EXPECT_EQ(client->stream(target)->data,expected[target]);
                            } else EXPECT_FALSE(client->stream(next_stream).has_value());
                        } else ASSERT_TRUE(client->pump());
                        EXPECT_EQ(store->load(started.id).state,storage::RunState::Active);
                        ASSERT_TRUE(client->send_stream(stream,bytes({1,0}),false));
                    } else {
                        const auto response=reply==7?bytes({7,0,1,0}):bytes({5,0,3,reply,0,0});
                        ASSERT_TRUE(client->send_stream(stream,response,reply!=7));
                    }
                }
                ASSERT_TRUE(pump_until(*client,[&]{return store->load(started.id).state==storage::RunState::Finalized;}));
                const auto run=store->load(started.id);
                ASSERT_EQ(run.outcomes.size(),1u);
                EXPECT_EQ(run.outcomes.front().state,reply==0x30?requirements::OutcomeState::Pass:requirements::OutcomeState::Fail);
                const auto count=std::count_if(run.events.begin(),run.events.end(),[](const auto& e){return e.kind=="raw_probe_stimulus";});
                EXPECT_EQ(count,1);
                const auto stimulus=std::find_if(run.events.begin(),run.events.end(),[](const auto& e){return e.kind=="raw_probe_stimulus";});
                ASSERT_NE(stimulus,run.events.end());
                std::size_t write_markers=0;
                for(std::size_t pos=0;(pos=stimulus->detail.find(" accepted_event_count=",pos))!=std::string::npos;++pos)++write_markers;
                EXPECT_EQ(write_markers,profile.definition.writes.size()+1);
            }
        }
    }
}

TEST(NativeQuicLive, DiscoveryOverlapRejectsInvalidFixtureBeforeAllocatingListener) {
    auto d18=std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{18,"test",true,{}});
    auto d21=std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog{21,"test",true,{}});
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    app::NativeRunManager manager(d18,d21,store,{});
    for(unsigned draft:{18u,21u})for(const auto& fields:std::vector<std::vector<std::string>>{std::vector<std::string>(32,"a"),{std::string(4095,'a')},{"."}}) {
        const auto scenario=draft==18?"receive-overlapping-subscribe-namespace-in-same-session":"d21-discovery-update-independent-overlap-spaces";
        const auto result=manager.start({static_cast<app::DraftVersion>(draft),app::TransportKind::NativeQuic,app::RunMode::Observed,
            {scenario},std::chrono::milliseconds(1000),app::TrackFixture{fields,""}});
        EXPECT_EQ(result.status,app::RunStartStatus::InvalidConfig);
    }
}
}  // namespace
}  // namespace moq::interop::transport

namespace moq::interop::transport {
namespace {

struct RawFamilyCatalogs {
    std::shared_ptr<const requirements::RequirementCatalog> draft18;
    std::shared_ptr<const requirements::RequirementCatalog> draft21;

    RawFamilyCatalogs() {
        const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
        const auto load = [&](unsigned draft) {
            const auto source = requirements::load_draft_source(
                draft, root / "docs", root / "requirements/draft-digests.json");
            return std::make_shared<const requirements::RequirementCatalog>(
                requirements::RequirementCatalog::load(source, root /
                    (draft == 18 ? "requirements/draft18.json" : "requirements/draft21.json")));
        };
        draft18 = load(18);
        draft21 = load(21);
    }
};

template <typename Predicate>
bool raw_family_wait(Predicate predicate,
                     std::chrono::milliseconds limit = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool raw_family_ready(const storage::RunRecord& run, std::string_view scenario,
                      unsigned ordinal) {
    return std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
        return event.kind == "context_ready" && event.scenario_id == scenario &&
            event.detail.find("ordinal=" + std::to_string(ordinal)) != std::string::npos;
    });
}

void expect_raw_family_outcome(const storage::RunRecord& run,
                               const requirements::RequirementCatalog& catalog,
                               std::string_view requirement,
                               requirements::OutcomeState expected) {
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(run.outcomes.size(), catalog.requirements.size());
    const auto row = std::find_if(run.outcomes.begin(), run.outcomes.end(),
        [&](const auto& outcome) { return outcome.requirement_id == requirement; });
    ASSERT_NE(row, run.outcomes.end());
    EXPECT_EQ(row->state, expected);
}

void expect_raw_family_evidence(const storage::RunRecord& run,
                                const std::vector<std::string>& scenarios) {
    std::vector<std::string> actual_connection_ids;
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        const auto& scenario = scenarios[index];
        SCOPED_TRACE(scenario);
        EXPECT_TRUE(raw_family_ready(run, scenario, static_cast<unsigned>(index + 1)));
        const auto established = std::find_if(run.events.begin(), run.events.end(),
            [&](const auto& event) {
                return event.kind == "transport_established" && event.scenario_id == scenario;
            });
        ASSERT_NE(established, run.events.end());
        ASSERT_TRUE(established->connection_id.has_value());
        EXPECT_FALSE(established->connection_id->empty());
        EXPECT_NE(established->detail.find("local_connection_id=" + *established->connection_id),
                  std::string::npos);
        EXPECT_NE(established->detail.find("transport_event_index=0"), std::string::npos);
        actual_connection_ids.push_back(*established->connection_id);
        const auto stimulus = std::find_if(run.events.begin(), run.events.end(),
            [&](const auto& event) {
                return event.kind == "raw_probe_stimulus" && event.scenario_id == scenario;
            });
        ASSERT_NE(stimulus, run.events.end());
        EXPECT_EQ(stimulus->connection_id, established->connection_id);
        EXPECT_NE(stimulus->detail.find(" stream=3 channel=0 operation=write"), std::string::npos);
        EXPECT_NE(stimulus->detail.find("accepted=4 fin=false bytes=af000000 accepted_event_count="),
                  std::string::npos);
        EXPECT_EQ(std::count_if(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "context_complete" && event.scenario_id == scenario;
        }), 1);
        EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "raw_probe_transport_event" && event.scenario_id == scenario &&
                event.stream_id == "2" && event.detail.find("bytes=af000000") != std::string::npos;
        }));
    }
    ASSERT_EQ(actual_connection_ids.size(), 2u);
    EXPECT_NE(actual_connection_ids[0], actual_connection_ids[1]);
    for (std::size_t index = 1; index < run.events.size(); ++index) {
        EXPECT_GT(run.events[index].sequence, run.events[index - 1].sequence);
        EXPECT_GE(run.events[index].monotonic_time_ns, run.events[index - 1].monotonic_time_ns);
    }
}

void drive_raw_family_goaway(test::PicoquicTestClient& client,
                             const scenarios::RequestGoawayProbe& profile,
                             unsigned duplicate_close = 3) {
    ASSERT_TRUE(pump_until(client, [&] {
        const auto setup = client.stream(3);
        return setup && setup->data.size() == 4;
    }));
    ASSERT_TRUE(client.send_stream(2, bytes({0xaf, 0, 0, 0}), false));
    const auto& opening = profile.definition.writes.front().bytes;
    ASSERT_TRUE(pump_until(client, [&] {
        const auto request = client.stream(1);
        return request && request->data.size() == opening.size();
    }));
    EXPECT_EQ(client.stream(1)->data, opening);
    if (!profile.duplicate) {
        const auto& second = profile.definition.writes[1].bytes;
        ASSERT_TRUE(pump_until(client, [&] {
            const auto request = client.stream(5);
            return request && request->data.size() == second.size();
        }));
        EXPECT_EQ(client.stream(5)->data, second);
        ASSERT_TRUE(client.send_stream(5, bytes({7, 0, 1, 0}), false));
    }
    ASSERT_TRUE(client.send_stream(1, bytes({7, 0}), false));
    const auto partial_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (std::chrono::steady_clock::now() < partial_deadline) {
        ASSERT_TRUE(client.pump());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(client.stream(1)->data, opening);
    ASSERT_TRUE(client.send_stream(1, bytes({1, 0}), false));
    const auto goaway = bytes({0x10, 0, 3, 0, 0xa7, 0x10});
    auto expected = opening;
    expected.insert(expected.end(), goaway.begin(), goaway.end());
    if (profile.duplicate) expected.insert(expected.end(), goaway.begin(), goaway.end());
    ASSERT_TRUE(pump_until(client, [&] {
        const auto request = client.stream(1);
        return request && request->data.size() == expected.size();
    }));
    EXPECT_EQ(client.stream(1)->data, expected);
    EXPECT_FALSE(client.stream(1)->fin);
    if (profile.duplicate) {
        ASSERT_TRUE(client.close(duplicate_close, {}));
    } else {
        auto second = profile.definition.writes[1].bytes;
        second.insert(second.end(), goaway.begin(), goaway.end());
        ASSERT_TRUE(pump_until(client, [&] {
            const auto request = client.stream(5);
            return request && request->data.size() == second.size();
        }));
        EXPECT_EQ(client.stream(5)->data, second);
        ASSERT_TRUE(pump_until(client, [&] {
            const auto request = client.stream(9);
            return request && request->data.size() == profile.definition.writes.back().bytes.size();
        }));
        EXPECT_EQ(client.stream(9)->data, profile.definition.writes.back().bytes);
        ASSERT_TRUE(client.send_stream(9, bytes({7, 0, 1, 0}), false));
    }
}

TEST(NativeQuicLive, RawFamilyGoawayUsesIndependentSessionsAndFullCatalog) {
    TestPemFiles pem;
    RawFamilyCatalogs catalogs;
    const auto profiles = scenarios::draft21_request_goaway_probes(std::chrono::milliseconds(1500));
    ASSERT_EQ(profiles.size(), 2u);
    // Exercise cleanup of both an application-closed session and a still-open
    // positive control before rebinding the identical public endpoint.
    for (const bool reverse : {false, true}) {
        SCOPED_TRACE(reverse);
        const std::vector<std::size_t> order = reverse ? std::vector<std::size_t>{1, 0}
                                                     : std::vector<std::size_t>{0, 1};
        const std::vector<std::string> ids{profiles[order[0]].definition.id, profiles[order[1]].definition.id};
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(catalogs.draft18, catalogs.draft21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
             .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
             .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
            app::RunMode::Observed, ids, std::chrono::milliseconds(1500), std::nullopt});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        for (std::size_t index = 0; index < order.size(); ++index) {
            ASSERT_TRUE(raw_family_wait([&] {
                return raw_family_ready(store->load(started.id), ids[index], static_cast<unsigned>(index + 1));
            }));
            auto client = test::PicoquicTestClient::create(
                {.port = started.endpoint.port, .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
            ASSERT_NE(client, nullptr);
            ASSERT_NO_FATAL_FAILURE(drive_raw_family_goaway(*client, profiles[order[index]]));
            ASSERT_TRUE(pump_until(*client, [&] {
                const auto run = store->load(started.id);
                return index == 0 ? raw_family_ready(run, ids[1], 2)
                                  : run.state == storage::RunState::Finalized;
            }));
            if (index == 0) {
                const auto run = store->load(started.id);
                EXPECT_EQ(run.state, storage::RunState::Active);
                EXPECT_TRUE(run.outcomes.empty());
                EXPECT_FALSE(run.finalized_at_unix_ns.has_value());
            }
        }
        const auto run = store->load(started.id);
        EXPECT_EQ(run.config.scenario_ids, ids);
        EXPECT_EQ(store->list({}).total, 1u);
        ASSERT_NO_FATAL_FAILURE(expect_raw_family_outcome(run, *catalogs.draft21,
            "D21-9-2-MUST-328", requirements::OutcomeState::Pass));
        ASSERT_NO_FATAL_FAILURE(expect_raw_family_evidence(run, ids));
        for (const auto& id : ids) {
            const auto stimulus = std::find_if(run.events.begin(), run.events.end(), [&](const auto& event) {
                return event.kind == "raw_probe_stimulus" && event.scenario_id == id;
            });
            ASSERT_NE(stimulus, run.events.end());
            EXPECT_NE(stimulus->detail.find("accepted=6 fin=false bytes=10000300a710 accepted_event_count="),
                      std::string::npos);
            if (id == profiles[1].definition.id) {
                EXPECT_NE(stimulus->detail.find(" stream=9 channel=1 operation=write"), std::string::npos);
            }
        }
    }
}

}  // namespace
}  // namespace moq::interop::transport

namespace moq::interop::transport {
namespace {

void drive_raw_family_subscriber_notify(test::PicoquicTestClient& client,
                                        const scenarios::Draft21CloseProbe& profile,
                                        unsigned close_code) {
    const bool publish = profile.definition.writes.front().channel == scenarios::RawProbeChannel::PeerBidi;
    ASSERT_TRUE(pump_until(client, [&] {
        const auto setup = client.stream(3);
        return setup && setup->data.size() == 4;
    }));
    ASSERT_TRUE(client.send_stream(2, bytes({0xaf, 0, 0, 0}), false));
    const auto peer_open = bytes({0x1d, 0, 10, 0, 1, 1, 'n', 1, 't', 0, 0, 4, 1});
    const auto ack = bytes({4, 0, 4, 0, 0, 4, 1});
    const auto stream = publish ? 0u : 1u;
    if (publish) {
        ASSERT_TRUE(client.send_stream(stream, std::span(peer_open).first(5), false));
    } else {
        ASSERT_TRUE(pump_until(client, [&] {
            const auto request = client.stream(stream);
            return request && request->data.size() == profile.definition.writes.front().bytes.size();
        }));
        EXPECT_EQ(client.stream(stream)->data, profile.definition.writes.front().bytes);
        ASSERT_TRUE(client.send_stream(stream, std::span(ack).first(2), false));
    }
    const auto partial_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (std::chrono::steady_clock::now() < partial_deadline) {
        ASSERT_TRUE(client.pump());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (publish) {
        const auto response = client.stream(stream);
        EXPECT_TRUE(!response || response->data.empty());
        ASSERT_TRUE(client.send_stream(stream, std::span(peer_open).subspan(5), false));
    } else {
        EXPECT_EQ(client.stream(stream)->data, profile.definition.writes.front().bytes);
        ASSERT_TRUE(client.send_stream(stream, std::span(ack).subspan(2), false));
    }
    EXPECT_EQ(profile.definition.writes.back().bytes, bytes({0x22, 0, 1, 0}));
    auto expected = profile.definition.writes.front().bytes;
    expected.insert(expected.end(), profile.definition.writes.back().bytes.begin(),
                    profile.definition.writes.back().bytes.end());
    ASSERT_TRUE(pump_until(client, [&] {
        const auto response = client.stream(stream);
        return response && response->data.size() == expected.size();
    }));
    EXPECT_EQ(client.stream(stream)->data, expected);
    EXPECT_FALSE(client.stream(stream)->fin);
    ASSERT_TRUE(client.close(close_code, {}));
}

TEST(NativeQuicLive, RawFamilySubscriberNotifyUsesIndependentSubscriptionOrigins) {
    TestPemFiles pem;
    RawFamilyCatalogs catalogs;
    std::vector<scenarios::Draft21CloseProbe> profiles;
    for (const auto& profile : scenarios::draft21_close_probes(
             std::chrono::milliseconds(1500), {bytes({'n'})}, bytes({'t'}))) {
        if (profile.requirement_id == "D21-9-10-MUST-370") profiles.push_back(profile);
    }
    ASSERT_EQ(profiles.size(), 2u);
    for (const bool reverse : {false, true}) {
        for (const auto close_codes : {std::array<unsigned, 2>{3, 3}, {3, 9}, {9, 3}}) {
            SCOPED_TRACE(reverse);
            SCOPED_TRACE(close_codes[0]);
            SCOPED_TRACE(close_codes[1]);
            const std::vector<std::size_t> order = reverse ? std::vector<std::size_t>{1, 0}
                                                         : std::vector<std::size_t>{0, 1};
            const std::vector<std::string> ids{profiles[order[0]].definition.id, profiles[order[1]].definition.id};
            auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
            app::NativeRunManager manager(catalogs.draft18, catalogs.draft21, store,
                {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
                 .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
                 .certificate_path = pem.certificate(), .private_key_path = pem.key()});
            const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
                app::RunMode::Observed, ids, std::chrono::milliseconds(1500), app::TrackFixture{{"n"}, "t"}});
            ASSERT_EQ(started.status, app::RunStartStatus::Started);
            for (std::size_t index = 0; index < order.size(); ++index) {
                ASSERT_TRUE(raw_family_wait([&] {
                    return raw_family_ready(store->load(started.id), ids[index], static_cast<unsigned>(index + 1));
                }));
                auto client = test::PicoquicTestClient::create(
                    {.port = started.endpoint.port, .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
                ASSERT_NE(client, nullptr);
                ASSERT_NO_FATAL_FAILURE(drive_raw_family_subscriber_notify(
                    *client, profiles[order[index]], close_codes[order[index]]));
                ASSERT_TRUE(pump_until(*client, [&] {
                    const auto run = store->load(started.id);
                    return index == 0 ? raw_family_ready(run, ids[1], 2)
                                      : run.state == storage::RunState::Finalized;
                }));
                if (index == 0) {
                    const auto run = store->load(started.id);
                    EXPECT_EQ(run.state, storage::RunState::Active);
                    EXPECT_TRUE(run.outcomes.empty());
                }
            }
            const auto run = store->load(started.id);
            ASSERT_NO_FATAL_FAILURE(expect_raw_family_outcome(run, *catalogs.draft21,
                "D21-9-10-MUST-370", close_codes[0] == 3 && close_codes[1] == 3
                    ? requirements::OutcomeState::Pass : requirements::OutcomeState::Fail));
            ASSERT_NO_FATAL_FAILURE(expect_raw_family_evidence(run, ids));
            EXPECT_EQ(store->list({}).total, 1u);
            for (std::size_t index = 0; index < profiles.size(); ++index) {
                const auto& profile = profiles[index];
                const auto stream = profile.definition.writes.front().channel == scenarios::RawProbeChannel::PeerBidi ? "0" : "1";
                const auto stimulus = std::find_if(run.events.begin(), run.events.end(), [&](const auto& event) {
                    return event.kind == "raw_probe_stimulus" && event.scenario_id == profile.definition.id;
                });
                ASSERT_NE(stimulus, run.events.end());
                EXPECT_NE(stimulus->detail.find(std::string(" stream=") + stream + " channel="), std::string::npos);
                EXPECT_NE(stimulus->detail.find("accepted=4 fin=false bytes=22000100 accepted_event_count="),
                          std::string::npos);
                EXPECT_TRUE(std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
                    return event.kind == "peer_close" && event.scenario_id == profile.definition.id &&
                        event.detail.find("application close code=" + std::to_string(close_codes[index]) +
                                          " transport_event_index=") == 0;
                }));
            }
        }
    }
}

TEST(NativeQuicLive, RawFamilyMissingOrCancelledSecondContextCannotPassAndReleasesPort) {
    TestPemFiles pem;
    RawFamilyCatalogs catalogs;
    const auto profiles = scenarios::draft21_request_goaway_probes(std::chrono::milliseconds(700));
    ASSERT_EQ(profiles.size(), 2u);
    const std::vector<std::string> ids{profiles[0].definition.id, profiles[1].definition.id};
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel);
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(catalogs.draft18, catalogs.draft21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
             .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
             .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
            app::RunMode::Observed, ids, std::chrono::milliseconds(700), std::nullopt});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create(
            {.port = started.endpoint.port, .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(client, nullptr);
        ASSERT_NO_FATAL_FAILURE(drive_raw_family_goaway(*client, profiles[0]));
        ASSERT_TRUE(pump_until(*client, [&] { return raw_family_ready(store->load(started.id), ids[1], 2); }));
        EXPECT_EQ(store->load(started.id).state, storage::RunState::Active);
        if (!cancel) {
            ASSERT_TRUE(raw_family_wait([&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        }
        const auto stop_started = std::chrono::steady_clock::now();
        EXPECT_TRUE(manager.stop(started.id));
        EXPECT_LT(std::chrono::steady_clock::now() - stop_started, std::chrono::seconds(1));
        const auto run = store->load(started.id);
        ASSERT_NO_FATAL_FAILURE(expect_raw_family_outcome(run, *catalogs.draft21,
            "D21-9-2-MUST-328", requirements::OutcomeState::NotRun));
        ASSERT_TRUE(run.score.has_value());
        EXPECT_NE(run.score->verdict, requirements::RunVerdict::Pass);
        EXPECT_EQ(std::count_if(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "context_complete" && event.scenario_id == ids[0];
        }), 1);
        EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.scenario_id == ids[1] && (event.kind == "context_complete" ||
                event.kind == "transport_established" || event.kind == "peer_close" || event.kind == "raw_probe_stimulus");
        }));
        auto config = live_config(pem);
        config.bind_port = started.endpoint.port;
        const auto rebound = NativeQuicListener::create(config);
        ASSERT_NE(rebound.listener, nullptr);
        EXPECT_EQ(rebound.listener->bound_endpoint().port, started.endpoint.port);
        EXPECT_FALSE(manager.stop(started.id));
    }
}

TEST(NativeQuicConfiguration, RawFamilyRejectsInvalidSelectionsBeforeCreatingRun) {
    RawFamilyCatalogs catalogs;
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    // Deliberately absent certificate: validation must finish before listener creation.
    app::NativeRunManager manager(catalogs.draft18, catalogs.draft21, store, {});
    const std::string valid = "d21-duplicate-request-goaway";
    const std::vector<std::pair<std::vector<std::string>, app::RunStartStatus>> rejected{
        {{valid, valid}, app::RunStartStatus::InvalidConfig},
        {{valid, ""}, app::RunStartStatus::InvalidConfig},
        {{}, app::RunStartStatus::InvalidConfig},
        {{valid, "does-not-exist"}, app::RunStartStatus::Unsupported},
        {{valid, "receive-two-goaways-on-same-request-stream"}, app::RunStartStatus::Unsupported},
        {{valid, "d21-setup-unknown-options"}, app::RunStartStatus::Unsupported},
    };
    for (const auto& [ids, status] : rejected) {
        SCOPED_TRACE(::testing::PrintToString(ids));
        EXPECT_EQ(manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
            app::RunMode::Observed, ids, std::chrono::milliseconds(1000), std::nullopt}).status, status);
        EXPECT_EQ(store->list({}).total, 0u);
    }
    std::vector<std::string> excessive;
    for (unsigned index = 0; index < 101; ++index) excessive.push_back("unknown-" + std::to_string(index));
    EXPECT_EQ(manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
        app::RunMode::Observed, excessive, std::chrono::milliseconds(1000), std::nullopt}).status,
        app::RunStartStatus::InvalidConfig);
    EXPECT_EQ(manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {valid}, std::chrono::milliseconds(3600001), std::nullopt}).status,
        app::RunStartStatus::InvalidConfig);
    EXPECT_EQ(store->list({}).total, 0u);
}

}  // namespace
}  // namespace moq::interop::transport

namespace moq::interop::transport {
namespace {

TEST(NativeQuicLive, RawFamilyDraft18UsesRealGoawayAndIndependentControlContext) {
    TestPemFiles pem;
    RawFamilyCatalogs catalogs;
    const auto profiles = scenarios::draft18_request_goaway_probes(std::chrono::milliseconds(1500));
    ASSERT_EQ(profiles.size(), 1u);
    const std::vector<std::string> ids{profiles[0].definition.id, "receive-two-goaways-on-control-stream"};
    const auto control = scenarios::draft18_close_probe(ids[1], std::chrono::milliseconds(1500));
    ASSERT_EQ(control.writes.size(), 1u);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(catalogs.draft18, catalogs.draft21, store,
        {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
         .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
         .certificate_path = pem.certificate(), .private_key_path = pem.key()});
    const auto started = manager.start({app::DraftVersion::Draft18, app::TransportKind::NativeQuic,
        app::RunMode::Observed, ids, std::chrono::milliseconds(1500), std::nullopt});
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        ASSERT_TRUE(raw_family_wait([&] {
            return raw_family_ready(store->load(started.id), ids[index], static_cast<unsigned>(index + 1));
        }));
        auto client = test::PicoquicTestClient::create({.port = started.endpoint.port, .alpn = expected_alpn()});
        ASSERT_NE(client, nullptr);
        if (index == 0) {
            ASSERT_NO_FATAL_FAILURE(drive_raw_family_goaway(*client, profiles[0]));
        } else {
            ASSERT_TRUE(pump_until(*client, [&] {
                const auto setup = client->stream(3);
                return setup && setup->data == control.setup_bytes;
            }));
            ASSERT_TRUE(client->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
            auto expected = control.setup_bytes;
            expected.insert(expected.end(), control.writes[0].bytes.begin(), control.writes[0].bytes.end());
            ASSERT_TRUE(pump_until(*client, [&] {
                const auto request = client->stream(3);
                return request && request->data.size() == expected.size();
            }));
            EXPECT_EQ(client->stream(3)->data, expected);
            ASSERT_TRUE(client->close(3, {}));
        }
        ASSERT_TRUE(pump_until(*client, [&] {
            const auto run = store->load(started.id);
            return index == 0 ? raw_family_ready(run, ids[1], 2)
                              : run.state == storage::RunState::Finalized;
        }));
        if (index == 0) {
            EXPECT_EQ(store->load(started.id).state, storage::RunState::Active);
        }
    }
    const auto run = store->load(started.id);
    ASSERT_NO_FATAL_FAILURE(expect_raw_family_outcome(run, *catalogs.draft18,
        "D18-10-4-MUST-003", requirements::OutcomeState::Pass));
    ASSERT_NO_FATAL_FAILURE(expect_raw_family_outcome(run, *catalogs.draft18,
        "D18-10-4-MUST-002", requirements::OutcomeState::Pass));
    ASSERT_NO_FATAL_FAILURE(expect_raw_family_evidence(run, ids));
    EXPECT_EQ(run.config.scenario_ids, ids);
    EXPECT_EQ(store->list({}).total, 1u);
}

TEST(NativeQuicLive, RawFamilyIncompleteSecondAcknowledgmentDoesNotSendNotificationOrPass) {
    TestPemFiles pem;
    RawFamilyCatalogs catalogs;
    std::vector<scenarios::Draft21CloseProbe> profiles;
    for (const auto& profile : scenarios::draft21_close_probes(
             std::chrono::milliseconds(700), {bytes({'n'})}, bytes({'t'}))) {
        if (profile.requirement_id == "D21-9-10-MUST-370") profiles.push_back(profile);
    }
    ASSERT_EQ(profiles.size(), 2u);
    for (const bool missing : {false, true}) {
        SCOPED_TRACE(missing);
        // Establish the PUBLISH origin first; the unfinished second SUBSCRIBE
        // origin must remain missing from the full catalog proof.
        const auto subscribe = std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
            return profile.definition.writes.front().channel == scenarios::RawProbeChannel::NewBidi;
        });
        const auto publish = std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
            return profile.definition.writes.front().channel == scenarios::RawProbeChannel::PeerBidi;
        });
        ASSERT_NE(subscribe, profiles.end());
        ASSERT_NE(publish, profiles.end());
        const std::vector<std::string> ids{publish->definition.id, subscribe->definition.id};
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
        app::NativeRunManager manager(catalogs.draft18, catalogs.draft21, store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
             .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
             .certificate_path = pem.certificate(), .private_key_path = pem.key()});
        const auto started = manager.start({app::DraftVersion::Draft21, app::TransportKind::NativeQuic,
            app::RunMode::Observed, ids, std::chrono::milliseconds(700), app::TrackFixture{{"n"}, "t"}});
        ASSERT_EQ(started.status, app::RunStartStatus::Started);
        auto first = test::PicoquicTestClient::create(
            {.port = started.endpoint.port, .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(first, nullptr);
        ASSERT_NO_FATAL_FAILURE(drive_raw_family_subscriber_notify(*first, *publish, 3));
        ASSERT_TRUE(pump_until(*first, [&] { return raw_family_ready(store->load(started.id), ids[1], 2); }));
        first.reset();
        auto second = test::PicoquicTestClient::create(
            {.port = started.endpoint.port, .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(pump_until(*second, [&] {
            const auto setup = second->stream(3);
            return setup && setup->data.size() == 4;
        }));
        ASSERT_TRUE(second->send_stream(2, bytes({0xaf, 0, 0, 0}), false));
        const auto& opening = subscribe->definition.writes.front().bytes;
        ASSERT_TRUE(pump_until(*second, [&] {
            const auto request = second->stream(1);
            return request && request->data == opening;
        }));
        if (!missing) {
            ASSERT_TRUE(second->send_stream(1, bytes({4, 0}), false));
        }
        ASSERT_TRUE(pump_until(*second, [&] { return store->load(started.id).state == storage::RunState::Finalized; }));
        EXPECT_EQ(second->stream(1)->data, opening);
        const auto run = store->load(started.id);
        ASSERT_NO_FATAL_FAILURE(expect_raw_family_outcome(run, *catalogs.draft21,
            "D21-9-10-MUST-370", requirements::OutcomeState::NotRun));
        EXPECT_FALSE(std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.scenario_id == ids[1] && (event.kind == "raw_probe_stimulus" ||
                event.kind == "context_complete" || event.kind == "peer_close");
        }));
    }
}

}  // namespace
}  // namespace moq::interop::transport

namespace moq::interop::transport {
namespace {
TEST(NativeQuicLive, RepeatedPropertiesUseActualGatedFetchesAndCatalogScoring) {
    TestPemFiles pem;
    RawFamilyCatalogs catalogs;
    for (unsigned draft : {18u,21u}) for (bool changed : {false,true}) {
        SCOPED_TRACE(draft);
        SCOPED_TRACE(changed);
        const auto profiles = draft == 18
            ? scenarios::draft18_immutable_repeat_probes(std::chrono::milliseconds(1500),{bytes({'n'})},bytes({'t'}))
            : scenarios::draft21_immutable_repeat_probes(std::chrono::milliseconds(1500),{bytes({'n'})},bytes({'t'}));
        auto store = std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
        app::NativeRunManager manager(catalogs.draft18,catalogs.draft21,store,
            {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",.port_start=0,.port_end=0,
             .maximum_active_runs=1,.certificate_path=pem.certificate(),.private_key_path=pem.key()});
        const auto started = manager.start({static_cast<app::DraftVersion>(draft),app::TransportKind::NativeQuic,
            app::RunMode::Observed,{profiles.front().definition.id},std::chrono::milliseconds(1500),app::TrackFixture{{"n"},"t"}});
        ASSERT_EQ(started.status,app::RunStartStatus::Started);
        auto client = test::PicoquicTestClient::create({.port=started.endpoint.port,
            .alpn=draft == 18 ? expected_alpn() : bytes({'m','o','q','t','-','2','1'})});
        ASSERT_NE(client,nullptr);
        ASSERT_TRUE(pump_until(*client,[&] { return client->stream(3).has_value(); }));
        ASSERT_TRUE(client->send_stream(2,bytes({0xaf,0,0,0}),false));
        ASSERT_TRUE(pump_until(*client,[&] { auto stream=client->stream(1); return stream && stream->fin; }));
        EXPECT_EQ(client->stream(1)->data,profiles.front().definition.writes.front().bytes);
        const auto ack=bytes({0x18,0,8,0,7,draft == 18 ? 10u : 9u,0,0xb,2,0x40,1});
        ASSERT_TRUE(client->send_stream(1,ack,true));
        ASSERT_TRUE(client->send_stream(6,bytes({5,1,0x3c,7,9,99,4,0xb,2,0x40,1,1,42}),false));
        for (unsigned i=0;i<20;++i) client->pump();
        EXPECT_FALSE(client->stream(5).has_value());
        ASSERT_TRUE(client->send_stream(6,{},true));
        ASSERT_TRUE(pump_until(*client,[&] { auto stream=client->stream(5); return stream && stream->fin; }));
        EXPECT_EQ(client->stream(5)->data,profiles.front().definition.writes.at(1).bytes);
        ASSERT_TRUE(client->send_stream(10,bytes({5,3,0x3c,7,9,99,4,0xb,2,0x40,changed ? 2u : 1u,1,42}),true));
        ASSERT_TRUE(client->send_stream(5,ack,true));
        ASSERT_TRUE(pump_until(*client,[&] { return store->load(started.id).state==storage::RunState::Finalized; }));
        const auto run=store->load(started.id);
        for (const auto& profile : profiles) if (profile.definition.id==profiles.front().definition.id) {
            const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[&](const auto& value) { return value.requirement_id==profile.requirement_id; });
            ASSERT_NE(row,run.outcomes.end());
            EXPECT_EQ(row->state, changed && profile.aspect!=scenarios::ImmutableRepeatAspect::Presence
                ? requirements::OutcomeState::Fail : requirements::OutcomeState::Pass);
        }
    }
}
} // namespace
} // namespace moq::interop::transport
