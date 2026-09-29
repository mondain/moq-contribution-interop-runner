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
            if (std::string_view{argv[3]} != "burst") return 0;
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
