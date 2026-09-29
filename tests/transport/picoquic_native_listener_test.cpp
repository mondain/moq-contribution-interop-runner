#include "moq/interop/transport/native_quic_listener.h"
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
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
    config.retry_token_lifetime = std::chrono::seconds{120};
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

bool pump_until_ready(NativeQuicListener& listener) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : listener.poll(32)) {
            if (std::holds_alternative<ConnectionEstablishedEvent>(event)) {
                return true;
            }
        }
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

TEST(PicoquicNativeListener, RejectsMissingDatagramAsLocalProtocolClose) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "expect-missing-datagram-close");
    ASSERT_TRUE(peer.valid());

    std::optional<LocalCloseEvent> observed;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !observed) {
        for (const auto& event : result.listener->poll(32)) {
            if (const auto* closed = std::get_if<LocalCloseEvent>(&event)) {
                observed = *closed;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->error_space, CloseErrorSpace::Application);
    EXPECT_EQ(observed->error_code, 3U);
    EXPECT_EQ(observed->reason,
              (std::vector<std::byte>{
                  std::byte{'Q'}, std::byte{'U'}, std::byte{'I'}, std::byte{'C'},
                  std::byte{' '}, std::byte{'D'}, std::byte{'A'}, std::byte{'T'},
                  std::byte{'A'}, std::byte{'G'}, std::byte{'R'}, std::byte{'A'},
                  std::byte{'M'}, std::byte{' '}, std::byte{'n'}, std::byte{'o'},
                  std::byte{'t'}, std::byte{' '}, std::byte{'n'}, std::byte{'e'},
                  std::byte{'g'}, std::byte{'o'}, std::byte{'t'}, std::byte{'i'},
                  std::byte{'a'}, std::byte{'t'}, std::byte{'e'}, std::byte{'d'}}));
    bool delivered = false;
    while (std::chrono::steady_clock::now() < deadline && !delivered) {
        result.listener->poll(32);
        delivered = peer.exited_successfully();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(delivered);
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

TEST(PicoquicNativeListener, RejectsUnsupportedRetryTokenLifetime) {
    auto config = config_for("moqt-18");
    config.retry_token_lifetime = std::chrono::seconds{10};
    auto result = NativeQuicListener::create(config);
    EXPECT_EQ(result.listener, nullptr);
    EXPECT_EQ(result.error, NativeQuicListenerError::InvalidConfiguration);
}

TEST(PicoquicNativeListener, RejectsZeroSendQueueBound) {
    auto config = config_for("moqt-21");
    config.max_queued_send_bytes = 0;
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

TEST(PicoquicNativeListener, RepeatedPollOnePreservesAllStreamEvents) {
    auto result = NativeQuicListener::create(config_for("moqt-18"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-18",
                     "burst");
    ASSERT_TRUE(peer.valid());

    std::set<StreamId> received;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline &&
           received.size() < 4) {
        const auto events = result.listener->poll(1);
        ASSERT_LE(events.size(), 1U);
        for (const auto& event : events) {
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) {
                if (stream->fin) received.insert(stream->stream_id);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(received, (std::set<StreamId>{2, 6, 10, 14}));
}

TEST(PicoquicNativeListener, ReservesServerStreamIdsAfterHandshake) {
    auto result = NativeQuicListener::create(config_for("moqt-18"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-18");
    ASSERT_TRUE(peer.valid());
    std::vector<TransportEvent> events;
    ASSERT_TRUE(pump_until_established(*result.listener, peer, events));

    const auto first_bidi = result.listener->open_bidi();
    const auto first_uni = result.listener->open_uni();
    const auto second_bidi = result.listener->open_bidi();
    const auto second_uni = result.listener->open_uni();
    EXPECT_EQ(first_bidi.status, TransportStatus::Success);
    EXPECT_EQ(first_bidi.stream_id, 1U);
    EXPECT_EQ(first_uni.status, TransportStatus::Success);
    EXPECT_EQ(first_uni.stream_id, 3U);
    EXPECT_EQ(second_bidi.status, TransportStatus::Success);
    EXPECT_EQ(second_bidi.stream_id, 5U);
    EXPECT_EQ(second_uni.status, TransportStatus::Success);
    EXPECT_EQ(second_uni.stream_id, 7U);
}

TEST(PicoquicNativeListener, EnforcesAdvertisedDatagramLimit) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21");
    ASSERT_TRUE(peer.valid());
    std::vector<TransportEvent> events;
    ASSERT_TRUE(pump_until_established(*result.listener, peer, events));

    std::size_t maximum = 0;
    for (const auto& event : events) {
        if (const auto* ready =
                std::get_if<ConnectionEstablishedEvent>(&event)) {
            maximum = ready->max_datagram_payload;
        }
    }
    ASSERT_GT(maximum, 0U);
    const std::vector<std::byte> exact(maximum, std::byte{0x42});
    const std::vector<std::byte> oversized(maximum + 1, std::byte{0x42});
    const auto accepted = result.listener->send_datagram(exact);
    const auto rejected = result.listener->send_datagram(oversized);
    EXPECT_EQ(accepted.status, TransportStatus::Success);
    EXPECT_EQ(accepted.accepted, maximum);
    EXPECT_EQ(rejected.status, TransportStatus::DatagramTooLarge);
    EXPECT_EQ(rejected.accepted, 0U);
}

TEST(PicoquicNativeListener, SendsFinWithServerStreamPayload) {
    auto result = NativeQuicListener::create(config_for("moqt-18"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-18",
                     "expect-stream");
    ASSERT_TRUE(peer.valid());
    ASSERT_TRUE(pump_until_ready(*result.listener));

    const auto opened = result.listener->open_bidi();
    ASSERT_EQ(opened.status, TransportStatus::Success);
    const std::vector<std::byte> payload{std::byte{0x41}, std::byte{0x42}};
    const auto written = result.listener->write(opened.stream_id, payload, true);
    EXPECT_EQ(written.status, TransportStatus::Success);
    EXPECT_EQ(written.accepted, payload.size());

    bool delivered = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !delivered) {
        result.listener->poll(32);
        delivered = peer.exited_successfully();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(delivered);
}

TEST(PicoquicNativeListener, DeliversDatagramToIndependentPeer) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "expect-datagram");
    ASSERT_TRUE(peer.valid());
    ASSERT_TRUE(pump_until_ready(*result.listener));

    const std::vector<std::byte> payload(16, std::byte{0x42});
    const auto sent = result.listener->send_datagram(payload);
    ASSERT_EQ(sent.status, TransportStatus::Success);
    ASSERT_EQ(sent.accepted, payload.size());

    bool delivered = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !delivered) {
        result.listener->poll(32);
        delivered = peer.exited_successfully();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(delivered);
}

TEST(PicoquicNativeListener, ResetsServerStreamWithApplicationCode) {
    auto result = NativeQuicListener::create(config_for("moqt-18"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-18",
                     "expect-reset");
    ASSERT_TRUE(peer.valid());
    ASSERT_TRUE(pump_until_ready(*result.listener));

    const auto opened = result.listener->open_bidi();
    ASSERT_EQ(opened.status, TransportStatus::Success);
    const std::vector<std::byte> payload{std::byte{0x41}};
    ASSERT_EQ(result.listener->write(opened.stream_id, payload, false).status,
              TransportStatus::Success);
    const auto reset = result.listener->reset(opened.stream_id, 0x33);
    EXPECT_EQ(reset.status, TransportStatus::Success);

    bool delivered = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !delivered) {
        result.listener->poll(32);
        delivered = peer.exited_successfully();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(delivered);
}

TEST(PicoquicNativeListener, StopsPeerUnidirectionalStream) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "expect-stop");
    ASSERT_TRUE(peer.valid());

    bool saw_stream = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !saw_stream) {
        for (const auto& event : result.listener->poll(32)) {
            if (const auto* data = std::get_if<StreamDataEvent>(&event)) {
                saw_stream = data->stream_id == 2;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(saw_stream);
    const auto stopped = result.listener->stop_sending(2, 0x44);
    EXPECT_EQ(stopped.status, TransportStatus::Success);

    bool delivered = false;
    while (std::chrono::steady_clock::now() < deadline && !delivered) {
        result.listener->poll(32);
        delivered = peer.exited_successfully();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(delivered);
}

TEST(PicoquicNativeListener, SendsApplicationCloseReason) {
    auto result = NativeQuicListener::create(config_for("moqt-18"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-18",
                     "expect-close");
    ASSERT_TRUE(peer.valid());
    ASSERT_TRUE(pump_until_ready(*result.listener));

    const std::vector<std::byte> reason{
        std::byte{'d'}, std::byte{'o'}, std::byte{'n'}, std::byte{'e'}};
    const auto closed = result.listener->close(0x45, reason);
    EXPECT_EQ(closed.status, TransportStatus::Success);
    bool saw_local_close = false;
    for (const auto& event : result.listener->poll(32)) {
        if (const auto* local = std::get_if<LocalCloseEvent>(&event)) {
            saw_local_close = local->error_code == 0x45 &&
                              local->reason == reason;
        }
    }
    EXPECT_TRUE(saw_local_close);

    bool delivered = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !delivered) {
        result.listener->poll(32);
        delivered = peer.exited_successfully();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(delivered);
    EXPECT_EQ(result.listener->close(0x45, reason).status,
              TransportStatus::ConnectionClosed);
}

TEST(PicoquicNativeListener, RecordsPeerApplicationCloseReason) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "send-close");
    ASSERT_TRUE(peer.valid());

    std::optional<PeerCloseEvent> observed;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !observed) {
        for (const auto& event : result.listener->poll(32)) {
            if (const auto* closed = std::get_if<PeerCloseEvent>(&event)) {
                observed = *closed;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->error_space, CloseErrorSpace::Application);
    EXPECT_EQ(observed->error_code, 0x66U);
    EXPECT_EQ(observed->reason,
              (std::vector<std::byte>{std::byte{'b'}, std::byte{'y'},
                                      std::byte{'e'}}));
}

TEST(PicoquicNativeListener, RecordsPeerStopSendingCode) {
    auto result = NativeQuicListener::create(config_for("moqt-18"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-18",
                     "send-stop");
    ASSERT_TRUE(peer.valid());
    ASSERT_TRUE(pump_until_ready(*result.listener));
    const auto opened = result.listener->open_bidi();
    ASSERT_EQ(opened.status, TransportStatus::Success);
    const std::vector<std::byte> payload{std::byte{0x41}};
    ASSERT_EQ(result.listener->write(opened.stream_id, payload, false).status,
              TransportStatus::Success);

    std::optional<PeerStopSendingEvent> observed;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !observed) {
        for (const auto& event : result.listener->poll(32)) {
            if (const auto* stopped =
                    std::get_if<PeerStopSendingEvent>(&event)) {
                observed = *stopped;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->stream_id, opened.stream_id);
    EXPECT_EQ(observed->application_error, 0x77U);
}

TEST(PicoquicNativeListener, ReceivesPeerDatagramPayload) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "send-datagram");
    ASSERT_TRUE(peer.valid());

    std::optional<DatagramEvent> observed;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !observed) {
        for (const auto& event : result.listener->poll(32)) {
            if (const auto* datagram = std::get_if<DatagramEvent>(&event)) {
                observed = *datagram;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->data,
              (std::vector<std::byte>{std::byte{0x31}, std::byte{0x32}}));
}

TEST(PicoquicNativeListener, RecordsPeerResetCode) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "send-reset");
    ASSERT_TRUE(peer.valid());

    std::optional<PeerResetEvent> observed;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !observed) {
        for (const auto& event : result.listener->poll(32)) {
            if (const auto* reset = std::get_if<PeerResetEvent>(&event)) {
                observed = *reset;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->stream_id, 2U);
    EXPECT_EQ(observed->application_error, 0x78U);
}

TEST(PicoquicNativeListener, WritesPeerInitiatedBidirectionalStream) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "expect-bidi-response");
    ASSERT_TRUE(peer.valid());

    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !received) {
        for (const auto& event : result.listener->poll(32)) {
            if (const auto* stream = std::get_if<StreamDataEvent>(&event)) {
                received = stream->stream_id == 0 && stream->fin;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(received);
    const std::array<std::byte, 1> response{std::byte{0x42}};
    EXPECT_EQ(result.listener->write(0, response, true).status,
              TransportStatus::Success);
    bool delivered = false;
    while (std::chrono::steady_clock::now() < deadline && !delivered) {
        result.listener->poll(32);
        delivered = peer.exited_successfully();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(delivered);
}

TEST(PicoquicNativeListener, ReportsIdleTimeout) {
    auto config = config_for("moqt-21");
    config.idle_timeout = std::chrono::milliseconds{300};
    auto result = NativeQuicListener::create(std::move(config));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "hold-idle");
    ASSERT_TRUE(peer.valid());
    ASSERT_TRUE(pump_until_ready(*result.listener));

    bool closed = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline && !closed) {
        for (const auto& event : result.listener->poll(1)) {
            closed = std::holds_alternative<PeerCloseEvent>(event);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(closed);
}

TEST(PicoquicNativeListener, BoundsCumulativeQueuedStreamWrites) {
    auto result = NativeQuicListener::create(config_for("moqt-21"));
    ASSERT_NE(result.listener, nullptr);
    PeerProcess peer(result.listener->bound_endpoint().port, "moqt-21",
                     "hold-idle");
    ASSERT_TRUE(peer.valid());
    ASSERT_TRUE(pump_until_ready(*result.listener));
    const auto opened = result.listener->open_bidi();
    ASSERT_EQ(opened.status, TransportStatus::Success);

    const std::vector<std::byte> chunk(256 * 1024, std::byte{0x41});
    for (int index = 0; index < 4; ++index) {
        const auto accepted = result.listener->write(opened.stream_id, chunk,
                                                     false);
        ASSERT_EQ(accepted.status, TransportStatus::Success);
        ASSERT_EQ(accepted.accepted, chunk.size());
    }
    const auto blocked = result.listener->write(opened.stream_id, chunk,
                                                false);
    EXPECT_EQ(blocked.status, TransportStatus::WouldBlock);
    EXPECT_EQ(blocked.accepted, 0U);
}

}  // namespace
}  // namespace moq::interop::transport
