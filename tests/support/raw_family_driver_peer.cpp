#include "support/picoquic_client.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
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

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

int execute(const std::string& mode) {
    const auto* contract = std::getenv("MOQ_INTEROP_DRIVER_CONTRACT_VERSION");
    const auto* path = std::getenv("MOQ_INTEROP_DRIVER_REQUEST_FILE");
    if (!contract || std::string(contract) != "1" || !path) return 2;
    std::ifstream input(path);
    const auto request = nlohmann::json::parse(input);
    const auto scenario = request.at("scenario_id").get<std::string>();
    // A draft 21 run, or a draft 22 run of the same shared scenarios: the request carries the run's
    // draft and the scenario id selected for that draft (a d22- id for draft 22), never a mix.
    const auto draft = request.at("draft").get<unsigned>();
    if (draft != 21 && draft != 22) return 4;
    const std::string family = draft == 22 ? "d22-" : "d21-";
    const bool duplicate = scenario == family + "duplicate-request-goaway";
    if (!duplicate && scenario != family + "goaway-on-distinct-request-streams") return 3;
    if (request.at("transport") != "native_quic" ||
        request.at("namespace_hex") != nlohmann::json::array({"6e"}) ||
        request.at("track_name_hex") != "74") return 4;
    const auto endpoint = request.at("endpoint").get<std::string>();
    const std::string prefix = "moqt://127.0.0.1:";
    if (!endpoint.starts_with(prefix) || !endpoint.ends_with("/moq")) return 5;
    const auto port = std::stoul(endpoint.substr(prefix.size(),endpoint.size()-prefix.size()-4));
    if (port == 0 || port > 65535) return 6;
    std::cout << "scenario=" << scenario << " endpoint=" << endpoint << std::endl;
    if (mode == "--fail-control" && !duplicate) return 7;
    if (mode == "--ignore-term-control" && !duplicate) std::signal(SIGTERM,SIG_IGN);
    auto client = Client::create({.port=static_cast<std::uint16_t>(port),
        .alpn=bytes({'m','o','q','t','-','2',draft == 22 ? unsigned{'2'} : unsigned{'1'}})});
    if (!client) return 8;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const auto wait = [&](const auto& ready) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (!client->pump()) return false;
            if (ready()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    };
    if (!wait([&] { const auto setup=client->stream(3); return setup && setup->data==bytes({0xaf,0,0,0}); }) ||
        !client->send_stream(2,bytes({0xaf,0,0,0}),false)) return 9;
    const auto opening = [](unsigned id, unsigned field) { return bytes({0x50,0,5,id,1,1,field,0}); };
    const auto a = opening(1,'a');
    const auto b = opening(3,'b');
    if (!wait([&] { const auto stream=client->stream(1); return stream && stream->data==a; })) return 10;
    if (mode == "--stall-control" && !duplicate) {
        (void)wait([] { return false; });
        return 18;
    }
    if (!duplicate && (!wait([&] { const auto stream=client->stream(5); return stream && stream->data==b; }) ||
                       !client->send_stream(5,bytes({7,0,1,0}),false))) return 11;
    if (!client->send_stream(1,bytes({7,0,1,0}),false)) return 12;
    const auto goaway = bytes({0x10,0,3,0,0xa7,0x10});
    auto expected = a;
    expected.insert(expected.end(),goaway.begin(),goaway.end());
    if (duplicate) expected.insert(expected.end(),goaway.begin(),goaway.end());
    if (!wait([&] { const auto stream=client->stream(1); return stream && stream->data==expected && !stream->fin; })) return 13;
    if (duplicate) {
        if (!client->close(3,{})) return 14;
    } else {
        auto expected_b = b;
        expected_b.insert(expected_b.end(),goaway.begin(),goaway.end());
        if (!wait([&] { const auto stream=client->stream(5); return stream && stream->data==expected_b && !stream->fin; }) ||
            !wait([&] { const auto stream=client->stream(9); return stream && stream->data==opening(5,'c'); }) ||
            !client->send_stream(9,bytes({7,0,1,0}),false)) return 15;
    }
    std::cout << "actual GOAWAY bytes and acknowledged stream proof complete" << std::endl;
    if (mode == "--ignore-term-control" && !duplicate) {
        for (;;) {
            (void)client->pump();
            std::this_thread::sleep_for(1ms);
        }
    }
    const auto flush_deadline = std::chrono::steady_clock::now() + 100ms;
    while (std::chrono::steady_clock::now() < flush_deadline) {
        if (!client->pump()) return 16;
        std::this_thread::sleep_for(1ms);
    }
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        return execute(argc == 2 ? argv[1] : "");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 17;
    }
}
