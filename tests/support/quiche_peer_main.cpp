#include "quiche_client.h"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const auto port = std::strtoul(argv[1], nullptr, 10);
    if (port == 0 || port > 65535) return 2;

    moq::interop::transport::test::QuicheTestClient::Config config;
    config.port = static_cast<std::uint16_t>(port);
    for (const char character : std::string_view{argv[2]}) {
        config.alpn.push_back(static_cast<std::byte>(character));
    }
    auto client = moq::interop::transport::test::QuicheTestClient::create(config);
    if (!client) return 3;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client->pump()) return 4;
        if (client->established()) {
            const std::string_view action{argv[3]};
            if (action == "expect-datagram") {
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!client->pump()) return 11;
                    const auto received = client->take_datagrams();
                    if (!received.empty()) {
                        return received.size() == 1 &&
                                       received[0] ==
                                           std::vector<std::byte>(
                                               16, std::byte{0x42})
                                   ? 0
                                   : 12;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return 13;
            }
            if (action == "expect-reset") {
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!client->pump()) return 14;
                    const auto observed = client->stream(1);
                    if (observed && observed->reset_error) {
                        return *observed->reset_error == 0x33 ? 0 : 15;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return 16;
            }
            if (action == "expect-stop") {
                const std::array<std::byte, 1> payload{std::byte{0x41}};
                if (!client->send_stream(2, payload, false)) return 17;
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!client->pump()) return 18;
                    const auto attempted =
                        client->try_send_stream(2, payload, false);
                    if (attempted.status ==
                        moq::interop::transport::test::ClientStreamSendStatus::PeerStopped) {
                        return attempted.application_error == 0x44 ? 0 : 19;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return 20;
            }
            if (action == "expect-close") {
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!client->pump()) return 21;
                    const auto observed = client->peer_close();
                    if (observed) {
                        return observed->application &&
                                       observed->error_code == 0x45 &&
                                       observed->reason ==
                                           std::vector<std::byte>{
                                               std::byte{'d'}, std::byte{'o'},
                                               std::byte{'n'}, std::byte{'e'}}
                                   ? 0
                                   : 22;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return 23;
            }
            if (action == "send-close") {
                const std::vector<std::byte> reason{
                    std::byte{'b'}, std::byte{'y'}, std::byte{'e'}};
                return client->close(0x66, reason) ? 0 : 24;
            }
            if (action == "send-stop") {
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!client->pump()) return 25;
                    const auto observed = client->stream(1);
                    if (observed && !observed->data.empty()) {
                        return client->stop_stream(1, 0x77) ? 0 : 26;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return 27;
            }
            if (action == "send-datagram") {
                const std::array<std::byte, 2> payload{
                    std::byte{0x31}, std::byte{0x32}};
                return client->send_datagram(payload) ? 0 : 28;
            }
            if (action == "send-reset") {
                const std::array<std::byte, 1> payload{std::byte{0x41}};
                if (!client->send_stream(2, payload, false)) return 29;
                return client->reset_stream(2, 0x78) ? 0 : 30;
            }
            if (action == "expect-bidi-response") {
                const std::array<std::byte, 1> request{std::byte{0x41}};
                if (!client->send_stream(0, request, true)) return 31;
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!client->pump()) return 32;
                    const auto observed = client->stream(0);
                    if (observed && observed->fin) {
                        return observed->data ==
                                       std::vector<std::byte>{std::byte{0x42}}
                                   ? 0
                                   : 33;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return 34;
            }
            if (action == "hold-idle") {
                std::this_thread::sleep_for(std::chrono::seconds{2});
                return 0;
            }
            if (action == "expect-stream") {
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!client->pump()) return 8;
                    const auto observed = client->stream(1);
                    if (observed && observed->fin) {
                        return observed->data ==
                                       std::vector<std::byte>{std::byte{0x41},
                                                              std::byte{0x42}}
                                   ? 0
                                   : 9;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return 10;
            }
            if (action != "burst") return 0;
            const std::array<std::byte, 1> payload{std::byte{0x45}};
            for (std::uint64_t stream_id : {2U, 6U, 10U, 14U}) {
                if (!client->send_stream(stream_id, payload, true)) return 6;
            }
            const auto flush_deadline = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds{100};
            while (std::chrono::steady_clock::now() < flush_deadline) {
                if (!client->pump()) return 7;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return 5;
}
