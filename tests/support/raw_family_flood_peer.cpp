// A driven publisher stand-in that behaves like a real-bitrate publisher: after the SETUP
// exchange it streams media-sized chunks on publisher-opened unidirectional streams as fast
// as the transport accepts them. Argument "flood-first" floods only the first scenario of a
// run (any scenario other than d21-duplicate-request-goaway stays quiet after SETUP).
#include "support/picoquic_client.h"

#include <nlohmann/json.hpp>

#include <span>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Client = moq::interop::transport::test::PicoquicTestClient;
using Status = moq::interop::transport::test::ClientStreamSendStatus;

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

int execute(const std::string& mode) {
    const auto* path = std::getenv("MOQ_INTEROP_DRIVER_REQUEST_FILE");
    if (!path) return 2;
    std::ifstream input(path);
    const auto request = nlohmann::json::parse(input);
    const auto scenario = request.at("scenario_id").get<std::string>();
    const bool flood = mode != "flood-first" || scenario == "d21-duplicate-request-goaway";
    const auto endpoint = request.at("endpoint").get<std::string>();
    const std::string prefix = "moqt://127.0.0.1:";
    if (!endpoint.starts_with(prefix) || !endpoint.ends_with("/moq")) return 5;
    const auto port = std::stoul(endpoint.substr(prefix.size(), endpoint.size() - prefix.size() - 4));
    auto client = Client::create({.port = static_cast<std::uint16_t>(port),
        .alpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'})});
    if (!client) return 8;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    const auto wait = [&](const auto& ready) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (!client->pump()) return false;
            if (ready()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    };
    if (!wait([&] { const auto setup = client->stream(3); return setup && setup->data == bytes({0xaf, 0, 0, 0}); }) ||
        !client->send_stream(2, bytes({0xaf, 0, 0, 0}), false)) return 9;
    if (!flood) {
        (void)wait([] { return false; });
        return 0;
    }
    // About 12 KiB per "object", one uni stream per object, like a 3 Mbps / 30 fps publisher.
    const std::vector<std::byte> object(12 * 1024, std::byte{0x5a});
    std::uint64_t stream = 6;
    std::size_t offset = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client->pump()) return 0;  // the runner closed the session: it had enough
        const auto sent = client->try_send_stream(stream, std::span(object).subspan(offset), true);
        if (sent.status == Status::Success) { stream += 4; offset = 0; }
        else if (sent.status == Status::Partial) offset += sent.accepted;
        else if (sent.status != Status::WouldBlock) return 0;
        std::this_thread::sleep_for(100us);
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        return execute(argc == 2 ? argv[1] : "");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 17;
    }
}
