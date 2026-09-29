#include "moq/interop/transport/native_quic_listener.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/http/server.h"
#include "moq/interop/scenarios/draft18.h"
#include "moq/interop/scenarios/run_controller.h"
#include "transport/quiche_native_listener_internal.h"
#include "support/quiche_client.h"

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
bool pump_until(test::QuicheTestClient& client, Predicate predicate,
                std::chrono::milliseconds limit = std::chrono::seconds{2}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

detail::TokenAddress ipv4_address(std::uint16_t port = 4443) {
    detail::TokenAddress address;
    address.family = 4;
    address.length = 4;
    address.bytes[0] = std::byte{127};
    address.bytes[1] = std::byte{0};
    address.bytes[2] = std::byte{0};
    address.bytes[3] = std::byte{1};
    address.port = port;
    return address;
}

detail::RetryTokenCodec token_codec(unsigned seed = 0) {
    std::array<std::byte, 32> secret{};
    for (std::size_t index = 0; index < secret.size(); ++index) {
        secret[index] = static_cast<std::byte>(seed + index);
    }
    return detail::RetryTokenCodec(secret);
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
        Case{[](auto& value) { value.bind_address = "::"; }},
        Case{[](auto& value) { value.idle_timeout = std::chrono::milliseconds{0}; }},
        Case{[](auto& value) { value.retry_token_lifetime = std::chrono::seconds{0}; }},
        Case{[](auto& value) { value.max_udp_payload = 1199; }},
        Case{[](auto& value) { value.max_datagrams_per_poll = 0; }},
        Case{[](auto& value) { value.max_egress_datagrams_per_call = 0; }},
        Case{[](auto& value) { value.max_events = 0; }},
        Case{[](auto& value) { value.max_event_payload_bytes = 0; }},
        Case{[](auto& value) { value.initial_max_data = 0; }},
        Case{[](auto& value) { value.initial_max_streams_bidi = 0; }},
        Case{[](auto& value) { value.initial_max_streams_uni = 0; }},
        Case{[](auto& value) { value.datagram_receive_queue = 0; }},
        Case{[](auto& value) { value.datagram_send_queue = 0; }},
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
        Case{[](auto& value) { value.max_additional_connection_ids = 2; }},
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

TEST(NativeQuicSocketFailures,
     FatalReceiveBeforeConnectionIsTerminalAndReportedExactlyOnce) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    std::size_t receive_calls = 0;
    dependencies.socket.receive_message = [&](int, msghdr*, int) -> ssize_t {
        ++receive_calls;
        errno = EIO;
        return -1;
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);

    auto first = created.listener->poll(8);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<TransportErrorEvent>(first.front()));
    EXPECT_EQ(std::get<TransportErrorEvent>(first.front()).error,
              TransportError::InternalFailure);
    EXPECT_TRUE(created.listener->poll(8).empty());
    EXPECT_EQ(receive_calls, 1u);
}

TEST(NativeQuicSocketFailures, TruncatedDatagramIsDroppedWithoutTermination) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    std::size_t receive_calls = 0;
    dependencies.socket.receive_message = [&](int, msghdr* message,
                                               int) -> ssize_t {
        ++receive_calls;
        if (receive_calls == 1) {
            message->msg_flags = MSG_TRUNC;
            return 1200;
        }
        errno = EAGAIN;
        return -1;
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    EXPECT_TRUE(created.listener->poll(8).empty());
    EXPECT_EQ(receive_calls, 2u);
}

TEST(NativeQuicSocketFailures,
     RetryPacketSurvivesWouldBlockAndIsRetriedBeforeNewEgress) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    const auto real_send = dependencies.socket.send_datagram;
    std::size_t send_calls = 0;
    dependencies.socket.send_datagram =
        [&](int fd, const void* data, std::size_t size, int flags,
            const sockaddr* address, socklen_t address_size) -> ssize_t {
        ++send_calls;
        if (send_calls == 1) {
            errno = EAGAIN;
            return -1;
        }
        return real_send(fd, data, size, flags, address, address_size);
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
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
    EXPECT_GT(send_calls, 1u);
}

TEST(NativeQuicSocketFailures,
     PartialRetrySendIsTerminalAndPreventsLaterAcceptance) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    dependencies.socket.send_datagram =
        [](int, const void*, std::size_t size, int, const sockaddr*,
           socklen_t) -> ssize_t { return static_cast<ssize_t>(size - 1); };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    client->pump();
    auto events = created.listener->poll(8);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<TransportErrorEvent>(events.front()));
    EXPECT_TRUE(created.listener->poll(8).empty());
}

TEST(NativeQuicSocketFailures,
     FatalRetrySendIsTerminalAndPreventsLaterAcceptance) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    dependencies.socket.send_datagram =
        [](int, const void*, std::size_t, int, const sockaddr*,
           socklen_t) -> ssize_t {
        errno = EIO;
        return -1;
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->pump());
    auto events = created.listener->poll(8);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<TransportErrorEvent>(events.front()));
    EXPECT_TRUE(created.listener->poll(8).empty());
}

TEST(NativeQuicSocketFailures, ReceivePumpIsBoundedPerPoll) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.max_datagrams_per_poll = 3;
    auto dependencies = detail::default_native_listener_dependencies();
    std::size_t receive_calls = 0;
    dependencies.socket.receive_message = [&](int, msghdr* message,
                                               int) -> ssize_t {
        ++receive_calls;
        message->msg_flags = MSG_TRUNC;
        return 1200;
    };
    auto created = detail::create_native_quic_listener(config, dependencies);
    ASSERT_NE(created.listener, nullptr);
    EXPECT_TRUE(created.listener->poll(8).empty());
    EXPECT_EQ(receive_calls, 3u);
}

TEST(NativeQuicSocketFailures, EgressBudgetAppliesToTheEntirePollCall) {
    TestPemFiles pem;
    auto config = live_config(pem);
    config.max_egress_datagrams_per_call = 1;
    auto dependencies = detail::default_native_listener_dependencies();
    const auto real_send = dependencies.socket.send_datagram;
    std::size_t send_calls = 0;
    dependencies.socket.send_datagram =
        [&](int fd, const void* data, std::size_t size, int flags,
            const sockaddr* address, socklen_t address_size) -> ssize_t {
        ++send_calls;
        return real_send(fd, data, size, flags, address, address_size);
    };
    auto created = detail::create_native_quic_listener(config, dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);

    bool established = false;
    for (std::size_t iteration = 0; iteration < 1000 && !established;
         ++iteration) {
        ASSERT_TRUE(client->pump());
        const auto before = send_calls;
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        EXPECT_LE(send_calls - before, 1u);
    }
    EXPECT_TRUE(established);
}

TEST(NativeQuicSocketFailures, PacedPacketWaitsForInjectedMonotonicDeadline) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    std::uint64_t now = 0;
    dependencies.monotonic_nanoseconds = [&] { return now; };
    const auto real_send = dependencies.socket.send_datagram;
    std::size_t send_calls = 0;
    dependencies.socket.send_datagram =
        [&](int fd, const void* data, std::size_t size, int flags,
            const sockaddr* address, socklen_t address_size) -> ssize_t {
        ++send_calls;
        return real_send(fd, data, size, flags, address, address_size);
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);

    EXPECT_TRUE(created.listener->poll(8).empty());
    ASSERT_EQ(send_calls, 1u);
    ASSERT_TRUE(client->pump());
    EXPECT_TRUE(created.listener->poll(8).empty());
    EXPECT_EQ(send_calls, 1u);

    now = std::numeric_limits<std::uint64_t>::max();
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established;
    }));
    EXPECT_GT(send_calls, 1u);
}

TEST(NativeQuicSocketFailures,
     RetainedWouldBlockEgressDoesNotBlockEstablishedIngress) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    const auto real_send = dependencies.socket.send_datagram;
    bool block_egress = false;
    std::size_t blocked_sends = 0;
    dependencies.socket.send_datagram =
        [&](int fd, const void* data, std::size_t size, int flags,
            const sockaddr* address, socklen_t address_size) -> ssize_t {
        if (block_egress) {
            ++blocked_sends;
            errno = EAGAIN;
            return -1;
        }
        return real_send(fd, data, size, flags, address, address_size);
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
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

    block_egress = true;
    ASSERT_EQ(created.listener->send_datagram(bytes({1})).status,
              TransportStatus::Success);
    ASSERT_GT(blocked_sends, 0u);
    ASSERT_TRUE(client->send_datagram(bytes({2, 0, 3})));
    const auto events = created.listener->poll(8);
    const auto inbound = std::ranges::find_if(events, [](const auto& event) {
        const auto* datagram = std::get_if<DatagramEvent>(&event);
        return datagram != nullptr && datagram->data == bytes({2, 0, 3});
    });
    EXPECT_NE(inbound, events.end());
}

TEST(NativeQuicSocketFailures,
     FuturePacedEgressDoesNotBlockEstablishedIngress) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    std::uint64_t now = std::numeric_limits<std::uint64_t>::max();
    dependencies.monotonic_nanoseconds = [&] { return now; };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
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

    now = 0;
    ASSERT_EQ(created.listener->send_datagram(bytes({4})).status,
              TransportStatus::Success);
    ASSERT_TRUE(client->send_datagram(bytes({5, 0, 6})));
    const auto events = created.listener->poll(8);
    const auto inbound = std::ranges::find_if(events, [](const auto& event) {
        const auto* datagram = std::get_if<DatagramEvent>(&event);
        return datagram != nullptr && datagram->data == bytes({5, 0, 6});
    });
    EXPECT_NE(inbound, events.end());
}

TEST(NativeQuicConfiguration, ReportsCertificateFailurePrecisely) {
    auto result = NativeQuicListener::create(base_config());
    EXPECT_EQ(result.listener, nullptr);
    EXPECT_EQ(result.error, NativeQuicListenerError::CertificateLoadFailed);
}

TEST(RetryTokenCodec, RoundTripsBinaryConnectionIdsAndAddress) {
    const auto original = bytes({0, 1, 0, 2, 3});
    const auto retry = bytes({9, 0, 8, 0, 7});
    const auto encoded = token_codec().encode(
        100, ipv4_address(), original, retry);
    ASSERT_TRUE(encoded.has_value());

    const auto decoded = token_codec().validate(
        *encoded, 105, 10, ipv4_address(), retry);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->original_dcid, original);
    EXPECT_EQ(decoded->retry_scid, retry);
}

TEST(RetryTokenCodec, RejectsEveryTruncationAndSingleByteBitFlip) {
    const auto original = bytes({1, 2, 3, 4});
    const auto retry = bytes({5, 6, 7, 8});
    const auto encoded = token_codec().encode(
        100, ipv4_address(), original, retry);
    ASSERT_TRUE(encoded.has_value());

    for (std::size_t size = 0; size < encoded->size(); ++size) {
        EXPECT_FALSE(token_codec().validate(
            std::span<const std::byte>(*encoded).first(size), 101, 10,
            ipv4_address(), retry));
    }
    for (std::size_t index = 0; index < encoded->size(); ++index) {
        auto changed = *encoded;
        changed[index] ^= std::byte{1};
        EXPECT_FALSE(token_codec().validate(
            changed, 101, 10, ipv4_address(), retry)) << index;
    }
}

TEST(RetryTokenCodec, RejectsWrongAddressPortCidTimeAndSecret) {
    const auto original = bytes({1, 2, 3});
    const auto retry = bytes({4, 5, 6});
    const auto encoded = token_codec().encode(
        100, ipv4_address(), original, retry);
    ASSERT_TRUE(encoded.has_value());

    auto wrong_address = ipv4_address();
    wrong_address.bytes[3] = std::byte{2};
    EXPECT_FALSE(token_codec().validate(
        *encoded, 101, 10, wrong_address, retry));
    EXPECT_FALSE(token_codec().validate(
        *encoded, 101, 10, ipv4_address(4444), retry));
    EXPECT_FALSE(token_codec().validate(
        *encoded, 101, 10, ipv4_address(), bytes({4, 5, 7})));
    EXPECT_FALSE(token_codec().validate(
        *encoded, 111, 10, ipv4_address(), retry));
    EXPECT_FALSE(token_codec().validate(
        *encoded, 99, 10, ipv4_address(), retry));
    EXPECT_FALSE(token_codec(1).validate(
        *encoded, 101, 10, ipv4_address(), retry));
}

TEST(RetryTokenCodec, RejectsStructuralAndLifetimeLimits) {
    auto invalid_address = ipv4_address();
    invalid_address.length = 5;
    EXPECT_FALSE(token_codec().encode(
        1, invalid_address, bytes({1}), bytes({2})));
    EXPECT_FALSE(token_codec().encode(
        1, ipv4_address(), std::vector<std::byte>(21), bytes({2})));
    EXPECT_FALSE(token_codec().encode(
        1, ipv4_address(), bytes({1}), std::vector<std::byte>(21)));

    const auto encoded = token_codec().encode(
        std::numeric_limits<std::uint64_t>::max() - 1, ipv4_address(),
        bytes({1}), bytes({2}));
    ASSERT_TRUE(encoded.has_value());
    EXPECT_FALSE(token_codec().validate(
        *encoded, 1, 10, ipv4_address(), bytes({2})));
}

TEST(ActiveCidRoutes, TracksBinaryZerosAndRemovesTheStaleAlias) {
    detail::ActiveCidRoutes routes;
    const std::array<std::uint8_t, 3> first{0, 1, 0};
    const std::array<std::uint8_t, 3> second{2, 0, 3};
    EXPECT_TRUE(routes.add(first));
    EXPECT_TRUE(routes.add(second));
    EXPECT_FALSE(routes.add(first));
    EXPECT_TRUE(routes.contains(first));
    EXPECT_TRUE(routes.contains(second));
    EXPECT_TRUE(routes.synchronize(second, 1));
    EXPECT_FALSE(routes.contains(first));
    EXPECT_TRUE(routes.contains(second));
    EXPECT_EQ(routes.size(), 1u);
}

TEST(ActiveCidRoutes, RejectsAmbiguousOrUnknownSynchronization) {
    detail::ActiveCidRoutes routes;
    const std::array<std::uint8_t, 2> known{1, 2};
    const std::array<std::uint8_t, 2> unknown{3, 4};
    ASSERT_TRUE(routes.add(known));
    EXPECT_FALSE(routes.synchronize(unknown, 1));
    EXPECT_FALSE(routes.synchronize(known, 2));
    EXPECT_TRUE(routes.contains(known));
}

TEST(NativeQuicLifecycle, ReleasesPortAfterInjectedFailureAndDestruction) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    BoundEndpoint failed_endpoint;
    dependencies.after_bind = [&](const BoundEndpoint& endpoint) {
        failed_endpoint = endpoint;
        return false;
    };
    auto failed = detail::create_native_quic_listener(live_config(pem),
                                                       dependencies);
    ASSERT_EQ(failed.listener, nullptr);
    ASSERT_EQ(failed.error, NativeQuicListenerError::AfterBindFailed);
    ASSERT_NE(failed_endpoint.port, 0);

    auto exact = live_config(pem);
    exact.bind_port = failed_endpoint.port;
    auto first = NativeQuicListener::create(exact);
    ASSERT_NE(first.listener, nullptr);
    const auto port = first.listener->bound_endpoint().port;
    first.listener.reset();
    exact.bind_port = port;
    auto second = NativeQuicListener::create(exact);
    EXPECT_NE(second.listener, nullptr);
}

TEST(NativeQuicLifecycle, UsesInjectedSocketOpenBindAddressAndClose) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    const auto real_open = dependencies.socket.open;
    const auto real_bind = dependencies.socket.bind;
    const auto real_address = dependencies.socket.local_address;
    const auto real_close = dependencies.socket.close;
    std::size_t open_calls = 0;
    std::size_t bind_calls = 0;
    std::size_t address_calls = 0;
    std::size_t close_calls = 0;
    dependencies.socket.open = [&](int domain, int type, int protocol) {
        ++open_calls;
        return real_open(domain, type, protocol);
    };
    dependencies.socket.bind = [&](int fd, const sockaddr* address,
                                   socklen_t size) {
        ++bind_calls;
        return real_bind(fd, address, size);
    };
    dependencies.socket.local_address = [&](int fd, sockaddr* address,
                                            socklen_t* size) {
        ++address_calls;
        return real_address(fd, address, size);
    };
    dependencies.socket.close = [&](int fd) {
        ++close_calls;
        return real_close(fd);
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    EXPECT_EQ(open_calls, 1u);
    EXPECT_EQ(bind_calls, 1u);
    EXPECT_EQ(address_calls, 1u);
    EXPECT_EQ(close_calls, 0u);
    created.listener.reset();
    EXPECT_EQ(close_calls, 1u);
}

TEST(NativeQuicLifecycle, ReportsInjectedSocketSetupFailuresAndClosesFd) {
    TestPemFiles pem;
    {
        auto dependencies = detail::default_native_listener_dependencies();
        dependencies.socket.open = [](int, int, int) {
            errno = EMFILE;
            return -1;
        };
        auto created = detail::create_native_quic_listener(live_config(pem),
                                                            dependencies);
        EXPECT_EQ(created.listener, nullptr);
        EXPECT_EQ(created.error, NativeQuicListenerError::SocketOpenFailed);
    }
    {
        auto dependencies = detail::default_native_listener_dependencies();
        const auto real_close = dependencies.socket.close;
        std::size_t close_calls = 0;
        dependencies.socket.bind = [](int, const sockaddr*, socklen_t) {
            errno = EACCES;
            return -1;
        };
        dependencies.socket.close = [&](int fd) {
            ++close_calls;
            return real_close(fd);
        };
        auto created = detail::create_native_quic_listener(live_config(pem),
                                                            dependencies);
        EXPECT_EQ(created.listener, nullptr);
        EXPECT_EQ(created.error, NativeQuicListenerError::BindFailed);
        EXPECT_EQ(close_calls, 1u);
    }
    {
        auto dependencies = detail::default_native_listener_dependencies();
        const auto real_close = dependencies.socket.close;
        std::size_t close_calls = 0;
        dependencies.socket.local_address = [](int, sockaddr*, socklen_t*) {
            errno = EIO;
            return -1;
        };
        dependencies.socket.close = [&](int fd) {
            ++close_calls;
            return real_close(fd);
        };
        auto created = detail::create_native_quic_listener(live_config(pem),
                                                            dependencies);
        EXPECT_EQ(created.listener, nullptr);
        EXPECT_EQ(created.error, NativeQuicListenerError::BoundEndpointFailed);
        EXPECT_EQ(close_calls, 1u);
    }
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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

TEST(NativeQuicLive, Draft18ControllerCompletesSubscribeResponseOverQuic) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
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

    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto peer_client = test::QuicheTestClient::create(
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
    ASSERT_TRUE(peer_client->close(41, bytes({1, 0, 2})));
    bool peer_close = false;
    std::size_t peer_terminal_count = 0;
    ASSERT_TRUE(pump_until(*peer_client, [&] {
        for (const auto& event : peer_created.listener->poll(8)) {
            peer_terminal_count += terminal_event(event) ? 1u : 0u;
            if (const auto* close = std::get_if<PeerCloseEvent>(&event)) {
                peer_close = close->error_space == CloseErrorSpace::Application &&
                             close->error_code == 41 &&
                             close->reason == bytes({1, 0, 2});
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
    auto local_client = test::QuicheTestClient::create(
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
    EXPECT_EQ(close.reason, bytes({3, 0, 4}));
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    created.listener->poll(8);
    const auto stopped =
        created.listener->write(opened.stream_id, bytes({3}), false);
    ASSERT_EQ(stopped.status, TransportStatus::PeerStopped);
    ASSERT_EQ(stopped.application_error, 52u);
    const auto stop_events = created.listener->poll(8);
    const auto stop = std::ranges::find_if(stop_events, [](const auto& event) {
        return std::holds_alternative<PeerStopSendingEvent>(event);
    });
    ASSERT_NE(stop, stop_events.end());
    EXPECT_EQ(std::get<PeerStopSendingEvent>(*stop).stream_id,
              opened.stream_id);
    EXPECT_EQ(std::get<PeerStopSendingEvent>(*stop).application_error, 52u);
}

TEST(NativeQuicLive, UnsupportedVersionGetsNegotiationWithoutAcceptance) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    const auto real_send = dependencies.socket.send_datagram;
    std::size_t send_calls = 0;
    dependencies.socket.send_datagram =
        [&](int fd, const void* data, std::size_t size, int flags,
            const sockaddr* address, socklen_t address_size) -> ssize_t {
        ++send_calls;
        return real_send(fd, data, size, flags, address, address_size);
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    const auto packet = bytes({
        0xc0, 0xfa, 0xce, 0xb0, 0x0c, 8,
        1, 2, 3, 4, 5, 6, 7, 8,
        8, 9, 10, 11, 12, 13, 14, 15, 16, 0});
    ASSERT_TRUE(send_raw(created.listener->bound_endpoint().port, packet));
    EXPECT_TRUE(created.listener->poll(8).empty());
    EXPECT_EQ(send_calls, 1u);
}

TEST(NativeQuicLive, MalformedAndUnknownCidPacketsDoNotDamageSession) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
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
    auto first = test::QuicheTestClient::create(
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
    auto second = test::QuicheTestClient::create(
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

TEST(NativeQuicLive, ZeroContainingConnectionIdsRemainBinarySafe) {
    TestPemFiles pem;
    auto dependencies = detail::default_native_listener_dependencies();
    std::size_t random_calls = 0;
    dependencies.random_bytes = [&](std::span<std::byte> output) {
        ++random_calls;
        const auto fill = random_calls == 1 ? 0xaau
                          : random_calls <= 4 ? 0u
                                              : random_calls - 4u;
        std::ranges::fill(output, static_cast<std::byte>(fill));
        return true;
    };
    auto created = detail::create_native_quic_listener(live_config(pem),
                                                        dependencies);
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
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
    EXPECT_GE(random_calls, 6u);
}

TEST(NativeQuicLive, MigratesToAdditionalCidAndRetiresInitialSequence) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
        {.port = created.listener->bound_endpoint().port,
         .alpn = expected_alpn()});
    ASSERT_NE(client, nullptr);
    bool established = false;
    ASSERT_TRUE(pump_until(*client, [&] {
        for (const auto& event : created.listener->poll(8)) {
            established |=
                std::holds_alternative<ConnectionEstablishedEvent>(event);
        }
        return established && client->available_destination_ids() > 0;
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    auto client = test::QuicheTestClient::create(
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
    ASSERT_TRUE(client->close(81, bytes({1, 0, 2})));
    ASSERT_EQ(created.listener->close(82, bytes({3, 0, 4})).status,
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
    EXPECT_EQ(local.reason, bytes({3, 0, 4}));
    EXPECT_EQ(created.listener->close(83, {}).status,
              TransportStatus::ConnectionClosed);
    EXPECT_TRUE(created.listener->poll(16).empty());
}

TEST(NativeQuicLive, CloseAfterFinAndResetPreservesEvidenceBeforeTerminal) {
    TestPemFiles pem;
    for (const bool reset_case : {false, true}) {
        auto created = NativeQuicListener::create(live_config(pem));
        ASSERT_NE(created.listener, nullptr);
        auto client = test::QuicheTestClient::create(
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

TEST(NativeQuicLive, ServerWriteBackpressurePreservesEveryByteAndSingleFin) {
    TestPemFiles pem;
    auto created = NativeQuicListener::create(live_config(pem));
    ASSERT_NE(created.listener, nullptr);
    auto client = test::QuicheTestClient::create(
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
        auto config = live_config(pem);
        auto created = NativeQuicListener::create(config);
        ASSERT_NE(created.listener, nullptr);
        auto client = test::QuicheTestClient::create(
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
        const auto port = created.listener->bound_endpoint().port;
        if (close_first) {
            ASSERT_EQ(created.listener->close(101, {}).status,
                      TransportStatus::Success);
        }
        created.listener.reset();
        config.bind_port = port;
        auto replacement = NativeQuicListener::create(config);
        EXPECT_NE(replacement.listener, nullptr);
    }
}

}  // namespace
}  // namespace moq::interop::transport
