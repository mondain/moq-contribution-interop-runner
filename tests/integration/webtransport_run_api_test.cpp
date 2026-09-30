#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/http/server.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pico_webtransport.h>
#include <picoquic_internal.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace moq::interop {
namespace {

using Json = nlohmann::json;

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root = std::filesystem::path{MOQ_INTEROP_PROJECT_SOURCE_DIR};
    const auto source = requirements::load_draft_source(
        draft, root / "docs", root / "requirements" / "draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog::load(source,
            root / "requirements" / ("draft" + std::to_string(draft) + ".json")));
}

struct PublisherState {
    bool accepted = false;
    std::vector<std::uint8_t> request_bytes;
    h3zero_stream_ctx_t* request_stream = nullptr;
};

int publisher_callback(picoquic_cnx_t*, std::uint8_t* bytes, std::size_t length,
                       picohttp_call_back_event_t event, h3zero_stream_ctx_t* stream,
                       void* context) {
    auto* state = static_cast<PublisherState*>(context);
    if (event == picohttp_callback_connect_accepted) state->accepted = true;
    if (event == picohttp_callback_post_data && stream != nullptr &&
        (stream->stream_id & 3u) == 1u && bytes != nullptr) {
        state->request_stream = stream;
        state->request_bytes.insert(state->request_bytes.end(), bytes, bytes + length);
    }
    return 0;
}

bool publish_setup(unsigned port, unsigned draft,
                   const std::shared_ptr<storage::SqliteRunStore>& store,
                   const app::RunId& id) {
    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) return false;
    const int flags = ::fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socket_fd);
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        ::close(socket_fd);
        return false;
    }
    socklen_t local_size = sizeof(local);
    (void)::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&local), &local_size);
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    remote.sin_port = htons(static_cast<std::uint16_t>(port));
    auto* quic = picoquic_create(1, nullptr, nullptr, nullptr, "h3", nullptr,
                                nullptr, nullptr, nullptr, nullptr,
                                picoquic_current_time(), nullptr, nullptr,
                                nullptr, 0);
    if (quic == nullptr) { ::close(socket_fd); return false; }
    picoquic_set_null_verifier(quic);
    picoquic_cnx_t* cnx = nullptr;
    h3zero_callback_ctx_t* h3 = nullptr;
    h3zero_stream_ctx_t* control = nullptr;
    bool success = false;
    PublisherState state;
    if (picowt_prepare_client_cnx(quic, reinterpret_cast<sockaddr*>(&remote),
            &cnx, &h3, &control, picoquic_current_time(), "runner.test") == 0 &&
        h3zero_declare_stream_prefix(h3, control->stream_id,
            publisher_callback, &state) == 0 &&
        picoquic_start_client_cnx(cnx) == 0) {
        bool connect_sent = false;
        bool setup_sent = false;
        bool response_sent = false;
        std::array<std::uint8_t, 2048> outgoing{};
        std::array<std::uint8_t, 2048> incoming{};
        for (int step = 0; step < 3000 && !success; ++step) {
            for (int packet = 0; packet < 8; ++packet) {
                sockaddr_storage destination{};
                sockaddr_storage source{};
                picoquic_connection_id_t log_id{};
                picoquic_cnx_t* last = nullptr;
                std::size_t length = 0;
                int interface_index = 0;
                if (picoquic_prepare_next_packet(quic, picoquic_current_time(),
                    outgoing.data(), outgoing.size(), &length, &destination,
                    &source, &interface_index, &log_id, &last) != 0 || length == 0)
                    break;
                (void)::sendto(socket_fd, outgoing.data(), length, 0,
                    reinterpret_cast<sockaddr*>(&destination), sizeof(remote));
            }
            for (int packet = 0; packet < 8; ++packet) {
                sockaddr_in peer{};
                socklen_t peer_size = sizeof(peer);
                const auto length = ::recvfrom(socket_fd, incoming.data(), incoming.size(),
                    0, reinterpret_cast<sockaddr*>(&peer), &peer_size);
                if (length < 0) break;
                auto received_at = local;
                (void)picoquic_incoming_packet(quic, incoming.data(),
                    static_cast<std::size_t>(length),
                    reinterpret_cast<sockaddr*>(&peer),
                    reinterpret_cast<sockaddr*>(&received_at), 0, 0,
                    picoquic_current_time());
            }
            if (!connect_sent && h3->settings.settings_received) {
                const std::string authority = "127.0.0.1:" + std::to_string(port);
                const std::string offered = "\"moqt-" + std::to_string(draft) + "\"";
                std::array<std::uint8_t, 512> qpack{};
                auto* end = h3zero_create_connect_header_frame(qpack.data(),
                    qpack.data() + qpack.size(), authority.c_str(),
                    reinterpret_cast<const std::uint8_t*>("/moq"), 4,
                    "webtransport-h3", nullptr, nullptr, offered.c_str());
                if (end == nullptr) break;
                std::array<std::uint8_t, 1024> frame{};
                auto* cursor = picoquic_frames_varint_encode(frame.data(),
                    frame.data() + frame.size(), h3zero_frame_header);
                cursor = picoquic_frames_varint_encode(cursor,
                    frame.data() + frame.size(), end - qpack.data());
                if (cursor == nullptr) break;
                std::memcpy(cursor, qpack.data(), end - qpack.data());
                cursor += end - qpack.data();
                control->path_callback = publisher_callback;
                control->path_callback_ctx = &state;
                control->is_open = 1;
                control->ps.stream_state.is_upgrade_requested = 1;
                if (picoquic_add_to_stream_with_ctx(cnx, control->stream_id,
                    frame.data(), cursor - frame.data(), 0, control) != 0) break;
                connect_sent = true;
            }
            if (state.accepted && !setup_sent) {
                auto* stream = picowt_create_local_stream(cnx, 0, h3,
                                                           control->stream_id);
                if (stream == nullptr) break;
                static constexpr std::array<std::uint8_t, 4> setup{0xaf, 0, 0, 0};
                if (picoquic_add_to_stream_with_ctx(cnx, stream->stream_id,
                    setup.data(), setup.size(), 0, stream) != 0) break;
                setup_sent = true;
            }
            if (draft == 18 && state.request_bytes.size() >= 10 &&
                state.request_stream != nullptr && !response_sent) {
                static constexpr std::array<std::uint8_t, 7> response{
                    0x04, 0x00, 0x04, 0x05, 0x00, 0x02, 0x09};
                if (picoquic_add_to_stream_with_ctx(cnx,
                    state.request_stream->stream_id, response.data(), response.size(),
                    0, state.request_stream) != 0) break;
                response_sent = true;
            }
            if (setup_sent) {
                const auto run = store->load(id);
                for (const auto& event : run.events) {
                    if (event.kind == "peer_setup_received" &&
                        (draft == 21 || response_sent))
                        success = true;
                }
                if (draft == 18) {
                    success = false;
                    for (const auto& outcome : run.outcomes) {
                        if (outcome.requirement_id == "D18-5-1-MUST-001" &&
                            outcome.state == requirements::OutcomeState::Pass)
                            success = true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    if (h3 != nullptr) h3zero_callback_delete_context(cnx, h3);
    picoquic_free(quic);
    ::close(socket_fd);
    if (!success) {
        const auto run = store->load(id);
        std::cerr << "draft=" << draft << " accepted=" << state.accepted
                  << " request_bytes=" << state.request_bytes.size()
                  << " state=" << static_cast<int>(run.state)
                  << " outcomes=" << run.outcomes.size() << '\n';
        for (const auto& outcome : run.outcomes) {
            if (outcome.requirement_id == "D18-5-1-MUST-001")
                std::cerr << "response outcome=" << static_cast<int>(outcome.state) << '\n';
        }
        for (const auto& event : run.events)
            std::cerr << "event=" << event.kind << " " << event.detail << '\n';
    }
    return success;
}

TEST(WebTransportRunApi, AllocatesExactPublisherUrlForBothDrafts) {
    const app::BuildInfo build{"test", "test", {}};
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", build);
    auto draft18 = catalog(18);
    auto draft21 = catalog(21);
    auto runs = std::make_shared<app::NativeRunManager>(draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
            .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
            .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"});
    http::HttpServer server(draft18, draft21, store, build, {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    for (const auto draft : {18, 21}) {
        const std::string scenario = draft == 18
            ? "subscribe-to-publisher-track"
            : "d21-publisher-request-stream-placement";
        const Json request{{"draft", draft}, {"transport", "webtransport"},
                           {"mode", "observed"},
                           {"scenarios", Json::array({scenario})},
                           {"timeout_ms", 1000},
                           {"track", {{"namespace_hex", Json::array({"6e"})},
                                      {"name_hex", "78"}}}};
        const auto response = api.Post("/api/v1/runs", request.dump(),
                                       "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 201) << response->body;
        const auto body = Json::parse(response->body);
        const auto& endpoint = body.at("publisher_endpoint");
        EXPECT_EQ(endpoint.at("alpn"), "h3");
        EXPECT_EQ(endpoint.at("protocol"), "moqt-" + std::to_string(draft));
        EXPECT_EQ(endpoint.at("path"), "/moq");
        const auto port = endpoint.at("port").get<unsigned>();
        EXPECT_GT(port, 0u);
        EXPECT_EQ(endpoint.at("url"),
                  "https://127.0.0.1:" + std::to_string(port) + "/moq");
        EXPECT_EQ(body.at("run").at("config").at("transport"), "webtransport");
        const auto id = body.at("run").at("id").get<std::string>();
        EXPECT_TRUE(publish_setup(port, draft, store, id));
        const auto stopped = api.Post("/api/v1/runs/" + id + "/stop", "",
                                      "application/json");
        ASSERT_TRUE(stopped);
        if (stopped->status == 409) {
            EXPECT_EQ(store->load(id).state, storage::RunState::Finalized);
        } else {
            EXPECT_EQ(stopped->status, 200) << stopped->body;
        }
    }
}

TEST(WebTransportRunApi, OccupiedPortDoesNotCreateRunAndStopReleasesIt) {
    int occupied = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(occupied, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(occupied, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(occupied, reinterpret_cast<sockaddr*>(&address),
                            &length), 0);
    const auto port = ntohs(address.sin_port);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:",
        app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager(catalog(18), store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = port, .port_end = port, .maximum_active_runs = 1,
            .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
            .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"});
    const app::RunConfig config{app::DraftVersion::Draft18,
        app::TransportKind::WebTransport, app::RunMode::Observed,
        {"subscribe-to-publisher-track"}, std::chrono::seconds{1},
        app::TrackFixture{{"n"}, "x"}};
    EXPECT_EQ(manager.start(config).status, app::RunStartStatus::PortExhausted);
    EXPECT_EQ(store->list({1, 0}).total, 0u);
    ::close(occupied);
    const auto started = manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    EXPECT_EQ(started.endpoint.port, port);
    EXPECT_TRUE(manager.stop(started.id));
    int rebound = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(rebound, 0);
    EXPECT_EQ(::bind(rebound, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)), 0);
    ::close(rebound);
}

}  // namespace
}  // namespace moq::interop
