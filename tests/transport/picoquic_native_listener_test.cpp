#include "moq/interop/transport/native_quic_listener.h"
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace moq::interop::transport {
namespace {

std::vector<std::byte> alpn(std::string_view value) {
    std::vector<std::byte> bytes;
    for (const char character : value) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

NativeQuicListenerConfig config_for(std::string_view protocol) {
    NativeQuicListenerConfig config;
    config.certificate_path =
        std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem";
    config.private_key_path =
        std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem";
    config.expected_alpn = alpn(protocol);
    return config;
}

class PeerProcess {
public:
    PeerProcess(std::uint16_t port, std::string_view protocol,
                std::string_view action = {})
        : port_(std::to_string(port)), protocol_(protocol), action_(action) {
        pid_ = ::fork();
        if (pid_ == 0) {
            ::execl(QUICHE_TEST_PEER_PATH, QUICHE_TEST_PEER_PATH,
                    port_.c_str(), protocol_.c_str(), action_.c_str(),
                    nullptr);
            ::_exit(127);
        }
    }
    ~PeerProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    bool valid() const { return pid_ > 0; }
    bool exited_successfully() {
        if (pid_ <= 0) return false;
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) != pid_) return false;
        pid_ = -1;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

private:
    pid_t pid_ = -1;
    std::string port_;
    std::string protocol_;
    std::string action_;
};

bool pump_until_established(NativeQuicListener& listener, PeerProcess& peer,
                            std::vector<TransportEvent>& received) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& event : listener.poll(32)) {
            received.push_back(std::move(event));
        }
        if (peer.exited_successfully()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

TEST(PicoquicNativeListener, AcceptsExactDraftAlpn) {
    for (const std::string_view protocol : {"moqt-18", "moqt-21"}) {
        auto result = NativeQuicListener::create(config_for(protocol));
        ASSERT_NE(result.listener, nullptr);
        EXPECT_EQ(result.listener->bound_endpoint().address, "127.0.0.1");
        ASSERT_NE(result.listener->bound_endpoint().port, 0);

        PeerProcess peer(result.listener->bound_endpoint().port, protocol);
        ASSERT_TRUE(peer.valid());

        std::vector<TransportEvent> events;
        ASSERT_TRUE(pump_until_established(*result.listener, peer, events));
        std::size_t established = 0;
        for (const auto& event : events) {
            if (const auto* ready =
                    std::get_if<ConnectionEstablishedEvent>(&event)) {
                ++established;
                EXPECT_EQ(ready->alpn, alpn(protocol));
                EXPECT_FALSE(ready->local_connection_id.empty());
                EXPECT_FALSE(ready->peer_connection_id.empty());
                EXPECT_GT(ready->max_datagram_payload, 0U);
            }
        }
        EXPECT_EQ(established, 1U);
    }
}

TEST(PicoquicNativeListener, RejectsWrongDraftAlpn) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-16");
    ASSERT_TRUE(peer.valid());

    std::vector<TransportEvent> events;
    EXPECT_FALSE(pump_until_established(*result.listener, peer, events));
    for (const auto& event : events) {
        EXPECT_FALSE(std::holds_alternative<ConnectionEstablishedEvent>(event));
    }
}

TEST(PicoquicNativeListener, ReleasesBoundPortOnDestruction) {
    auto config = config_for("moqt-18");
    auto first = NativeQuicListener::create(config);
    ASSERT_NE(first.listener, nullptr);
    config.bind_port = first.listener->bound_endpoint().port;
    first.listener.reset();
    auto second = NativeQuicListener::create(config);
    ASSERT_NE(second.listener, nullptr);
    EXPECT_EQ(second.listener->bound_endpoint().port, config.bind_port);
}

TEST(PicoquicNativeListener, RejectsZeroRetryTokenLifetime) {
    auto config = config_for("moqt-18");
    config.retry_token_lifetime = std::chrono::seconds{0};
    auto result = NativeQuicListener::create(config);
    EXPECT_EQ(result.listener, nullptr);
    EXPECT_EQ(result.error, NativeQuicListenerError::InvalidConfiguration);
}

TEST(PicoquicNativeListener, BoundsQueuedPeerEvents) {
    auto config = config_for("moqt-18");
    config.max_events = 1;
    auto result = NativeQuicListener::create(config);
    ASSERT_NE(result.listener, nullptr)
        << "listener error "
        << static_cast<int>(result.error.value_or(
               NativeQuicListenerError::InvalidConfiguration));
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-18",
                     "burst");
    ASSERT_TRUE(peer.valid());

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline &&
           !peer.exited_successfully()) {
        result.listener->poll(0);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto events = result.listener->poll(8);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<EventQueueOverflowEvent>(events[0]));
}

}  // namespace
}  // namespace moq::interop::transport
