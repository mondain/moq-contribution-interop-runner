#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/http/server.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_evaluators.h"
#include "moq/interop/requirements/execution_audit.h"
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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
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
    std::map<std::uint64_t, std::vector<std::uint8_t>> stream_bytes;
};

struct RawFamilyContext {
    std::string scenario_id;
    std::string next_scenario_id;
    bool publish_origin = false;
    unsigned close_code = 3;
    std::optional<std::uint64_t> request_stream_id;
    std::vector<std::uint8_t> received;
    bool connect_accepted = false;
    bool gate_held_for_fragment = false;
    bool peer_close_sent = false;
    std::map<std::uint64_t, std::vector<std::uint8_t>> observed_streams;
};

int publisher_callback(picoquic_cnx_t*, std::uint8_t* bytes, std::size_t length,
                       picohttp_call_back_event_t event, h3zero_stream_ctx_t* stream,
                       void* context) {
    auto* state = static_cast<PublisherState*>(context);
    if (event == picohttp_callback_connect_accepted) state->accepted = true;
    if (event == picohttp_callback_post_data && stream != nullptr && bytes != nullptr) {
        auto& received = state->stream_bytes[stream->stream_id];
        received.insert(received.end(), bytes, bytes + length);
    }
    if (event == picohttp_callback_post_data && stream != nullptr &&
        (stream->stream_id & 3u) == 1u && bytes != nullptr) {
        state->request_stream = stream;
        state->request_bytes.insert(state->request_bytes.end(), bytes, bytes + length);
    }
    return 0;
}

// Whether the runner accepted the last publish_setup's WebTransport CONNECT.
bool last_connect_accepted = false;

bool publish_setup(unsigned port, unsigned draft,
                   const std::shared_ptr<storage::SqliteRunStore>& store,
                   const app::RunId& id,
                   std::optional<std::uint64_t> reject_code = std::nullopt,
                   std::string expected_requirement_id = {},
                   std::vector<std::uint8_t> peer_setup = {0xaf, 0, 0, 0},
                   requirements::OutcomeState expected_state =
                       requirements::OutcomeState::Pass,
                   RawFamilyContext* family = nullptr) {
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
        bool close_sent = false;
        bool fragment_sent = false;
        bool remaining_sent = false;
        h3zero_stream_ctx_t* publish_stream = nullptr;
        std::optional<std::chrono::steady_clock::time_point> fragment_time;
        std::set<std::uint64_t> acknowledged_streams;
        bool barrier_response_sent = false;
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
                if (picoquic_add_to_stream_with_ctx(cnx, stream->stream_id,
                    peer_setup.data(), peer_setup.size(), 0, stream) != 0) break;
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
                if (family != nullptr) {
                    const bool goaway_duplicate = family->scenario_id == "d21-duplicate-request-goaway";
                    const bool goaway_control = family->scenario_id == "d21-goaway-on-distinct-request-streams";
                    if (goaway_duplicate || goaway_control) {
                        const auto opening = [](unsigned request_id, unsigned field) {
                            return std::vector<std::uint8_t>{0x50, 0, 5,
                                static_cast<std::uint8_t>(request_id), 1, 1,
                                static_cast<std::uint8_t>(field), 0};
                        };
                        const auto a = opening(1, 'a');
                        const auto b = opening(3, 'b');
                        const auto c = opening(5, 'c');
                        const auto stream_for = [&](const std::vector<std::uint8_t>& prefix)
                            -> std::optional<std::uint64_t> {
                            for (const auto& [stream_id, data] : state.stream_bytes) {
                                if (data.size() >= prefix.size() &&
                                    std::equal(prefix.begin(), prefix.end(), data.begin()))
                                    return stream_id;
                            }
                            return std::nullopt;
                        };
                        const auto first = stream_for(a);
                        const auto second = stream_for(b);
                        if (first && !fragment_sent) {
                            auto* stream = h3zero_find_stream(h3, *first);
                            const std::array<std::uint8_t, 2> partial{7, 0};
                            if (stream == nullptr || picoquic_add_to_stream_with_ctx(cnx,
                                    *first, partial.data(), partial.size(), 0, stream) != 0) break;
                            family->request_stream_id = first;
                            fragment_sent = true;
                            fragment_time = std::chrono::steady_clock::now();
                        }
                        if (goaway_control && second && !acknowledged_streams.contains(*second)) {
                            auto* stream = h3zero_find_stream(h3, *second);
                            const std::array<std::uint8_t, 4> response{7, 0, 1, 0};
                            if (stream == nullptr || picoquic_add_to_stream_with_ctx(cnx,
                                    *second, response.data(), response.size(), 0, stream) != 0) break;
                            acknowledged_streams.insert(*second);
                        }
                        if (fragment_time && !remaining_sent &&
                            std::chrono::steady_clock::now() - *fragment_time >=
                                std::chrono::milliseconds{20}) {
                            family->gate_held_for_fragment = state.stream_bytes[*first] == a;
                            auto* stream = h3zero_find_stream(h3, *first);
                            const std::array<std::uint8_t, 2> remainder{1, 0};
                            if (stream == nullptr || picoquic_add_to_stream_with_ctx(cnx,
                                    *first, remainder.data(), remainder.size(), 0, stream) != 0) break;
                            remaining_sent = true;
                        }
                        const std::vector<std::uint8_t> goaway{0x10, 0, 3, 0, 0xa7, 0x10};
                        auto expected = a;
                        expected.insert(expected.end(), goaway.begin(), goaway.end());
                        if (goaway_duplicate) expected.insert(expected.end(), goaway.begin(), goaway.end());
                        if (remaining_sent && first && state.stream_bytes[*first] == expected) {
                            family->received = expected;
                            if (goaway_duplicate && !close_sent) {
                                if (picoquic_close(cnx, family->close_code) != 0) break;
                                close_sent = true;
                                family->peer_close_sent = true;
                            } else if (goaway_control && second && !barrier_response_sent) {
                                auto expected_b = b;
                                expected_b.insert(expected_b.end(), goaway.begin(), goaway.end());
                                const auto barrier = stream_for(c);
                                if (state.stream_bytes[*second] == expected_b && barrier &&
                                    state.stream_bytes[*barrier] == c) {
                                    auto* stream = h3zero_find_stream(h3, *barrier);
                                    const std::array<std::uint8_t, 4> response{7, 0, 1, 0};
                                    if (stream == nullptr || picoquic_add_to_stream_with_ctx(cnx,
                                            *barrier, response.data(), response.size(), 0, stream) != 0) break;
                                    barrier_response_sent = true;
                                }
                            }
                        }
                        family->connect_accepted = state.accepted;
                        if (close_sent || barrier_response_sent) {
                            if (!family->next_scenario_id.empty()) {
                                success = run.state == storage::RunState::Active &&
                                    std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
                                        return event.kind == "context_ready" &&
                                               event.scenario_id == family->next_scenario_id;
                                    });
                            } else {
                                success = run.state == storage::RunState::Finalized &&
                                    std::any_of(run.outcomes.begin(), run.outcomes.end(), [&](const auto& outcome) {
                                        return outcome.requirement_id == expected_requirement_id &&
                                               outcome.state == expected_state;
                                    });
                            }
                        }
                        if (success) {
                            family->observed_streams = state.stream_bytes;
                            continue;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                        continue;
                    }
                    static const std::vector<std::uint8_t> subscribe{
                        3, 0, 7, 1, 1, 1, 'n', 1, 't', 0};
                    static const std::vector<std::uint8_t> publish{
                        0x1d, 0, 10, 0, 1, 1, 'n', 1, 't', 0, 0, 4, 1};
                    static const std::vector<std::uint8_t> subscribe_ok{
                        4, 0, 4, 0, 0, 4, 1};
                    static const std::vector<std::uint8_t> publish_ok{7, 0, 1, 0};
                    static const std::vector<std::uint8_t> notify{0x22, 0, 1, 0};
                    if (family->publish_origin && publish_stream == nullptr) {
                        publish_stream = picowt_create_local_stream(cnx, 1, h3,
                                                                   control->stream_id);
                        if (publish_stream == nullptr) break;
                        publish_stream->path_callback = publisher_callback;
                        publish_stream->path_callback_ctx = &state;
                        family->request_stream_id = publish_stream->stream_id;
                        if (picoquic_add_to_stream_with_ctx(cnx, publish_stream->stream_id,
                                publish.data(), 5, 0, publish_stream) != 0) break;
                        fragment_sent = true;
                        fragment_time = std::chrono::steady_clock::now();
                    } else if (!family->publish_origin && !fragment_sent &&
                               state.request_bytes == subscribe && state.request_stream != nullptr) {
                        family->request_stream_id = state.request_stream->stream_id;
                        if (picoquic_add_to_stream_with_ctx(cnx, state.request_stream->stream_id,
                                subscribe_ok.data(), 2, 0, state.request_stream) != 0) break;
                        fragment_sent = true;
                        fragment_time = std::chrono::steady_clock::now();
                    }
                    if (fragment_time && !remaining_sent &&
                        std::chrono::steady_clock::now() - *fragment_time >=
                            std::chrono::milliseconds{20}) {
                        const auto& received = state.stream_bytes[*family->request_stream_id];
                        family->gate_held_for_fragment = family->publish_origin
                            ? received.empty() : received == subscribe;
                        const auto& response = family->publish_origin ? publish : subscribe_ok;
                        const std::size_t split = family->publish_origin ? 5 : 2;
                        auto* stream = family->publish_origin ? publish_stream : state.request_stream;
                        if (picoquic_add_to_stream_with_ctx(cnx, stream->stream_id,
                                response.data() + split, response.size() - split, 0, stream) != 0)
                            break;
                        remaining_sent = true;
                    }
                    auto expected = family->publish_origin ? publish_ok : subscribe;
                    expected.insert(expected.end(), notify.begin(), notify.end());
                    if (remaining_sent && !close_sent &&
                        state.stream_bytes[*family->request_stream_id] == expected) {
                        family->received = expected;
                        if (picoquic_close(cnx, family->close_code) != 0) break;
                        close_sent = true;
                    }
                    family->connect_accepted = state.accepted;
                    if (close_sent) {
                        if (!family->next_scenario_id.empty()) {
                            success = run.state == storage::RunState::Active &&
                                std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
                                    return event.kind == "context_ready" &&
                                           event.scenario_id == family->next_scenario_id;
                                });
                        } else {
                            success = run.state == storage::RunState::Finalized &&
                                std::any_of(run.outcomes.begin(), run.outcomes.end(), [&](const auto& outcome) {
                                    return outcome.requirement_id == expected_requirement_id &&
                                           outcome.state == expected_state;
                                });
                        }
                    }
                    if (success) continue;
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    continue;
                }
                if (reject_code && !close_sent &&
                    std::any_of(run.events.begin(), run.events.end(),
                        [](const auto& event) {
                            return event.kind == "local_setup_sent";
                        })) {
                    if (picoquic_close(cnx, *reject_code) != 0) break;
                    close_sent = true;
                }
                if (reject_code || !expected_requirement_id.empty()) {
                    success = std::any_of(run.outcomes.begin(), run.outcomes.end(),
                        [&](const auto& outcome) {
                            return outcome.requirement_id == expected_requirement_id &&
                                   outcome.state == expected_state;
                        });
                    if (success) continue;
                }
                for (const auto& event : run.events) {
                    if (event.kind == "peer_setup_received" &&
                        (draft != 18 || response_sent) && !reject_code &&
                        expected_requirement_id.empty())
                        success = true;
                }
                if (draft == 18 && expected_requirement_id.empty()) {
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
    last_connect_accepted = state.accepted;
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

TEST(WebTransportRunApi, RawFamilyGoawayRecreatesStillOpenSessionOnExactEndpoint) {
    const app::BuildInfo build{"test", "test", {}};
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", build);
    const auto draft18 = catalog(18);
    const auto draft21 = catalog(21);
    auto runs = std::make_shared<app::NativeRunManager>(draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
            .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
            .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"});
    http::HttpServer server(draft18, draft21, store, build, {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    const std::vector<std::string> scenarios{
        "d21-goaway-on-distinct-request-streams", "d21-duplicate-request-goaway"};
    const Json request{{"draft", 21}, {"transport", "webtransport"}, {"mode", "observed"},
                       {"scenarios", scenarios}, {"timeout_ms", 2500}};
    const auto response = api.Post("/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 201) << response->body;
    const auto body = Json::parse(response->body);
    const auto id = body.at("run").at("id").get<std::string>();
    const auto port = body.at("publisher_endpoint").at("port").get<unsigned>();
    const auto endpoint = "https://127.0.0.1:" + std::to_string(port) + "/moq";
    EXPECT_EQ(body.at("publisher_endpoint").at("url"), endpoint);

    RawFamilyContext first{.scenario_id = scenarios[0], .next_scenario_id = scenarios[1]};
    ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt,
        "D21-9-2-MUST-328", {0xaf, 0, 0, 0}, requirements::OutcomeState::Pass, &first));
    EXPECT_TRUE(first.connect_accepted);
    EXPECT_TRUE(first.gate_held_for_fragment);
    EXPECT_FALSE(first.peer_close_sent);
    ASSERT_TRUE(first.request_stream_id);
    EXPECT_EQ(*first.request_stream_id & 3u, 1u);
    std::set<std::uint64_t> actual_request_streams;
    for (const auto& [stream_id, data] : first.observed_streams) {
        if (data.size() >= 3 && data[0] == 0x50 && data[1] == 0 && data[2] == 5) {
            actual_request_streams.insert(stream_id);
            EXPECT_EQ(stream_id & 3u, 1u);
        }
    }
    EXPECT_EQ(actual_request_streams.size(), 3u);
    const auto between = store->load(id);
    ASSERT_EQ(between.state, storage::RunState::Active);
    EXPECT_TRUE(between.outcomes.empty());
    EXPECT_FALSE(between.finalized_at_unix_ns);
    EXPECT_EQ(std::count_if(between.events.begin(), between.events.end(), [&](const auto& event) {
        return event.kind == "peer_close" && event.scenario_id == scenarios[0];
    }), 0);
    EXPECT_EQ(std::count_if(between.events.begin(), between.events.end(), [&](const auto& event) {
        return event.kind == "context_complete" && event.scenario_id == scenarios[0];
    }), 1);

    RawFamilyContext second{.scenario_id = scenarios[1]};
    ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt,
        "D21-9-2-MUST-328", {0xaf, 0, 0, 0}, requirements::OutcomeState::Pass, &second));
    EXPECT_TRUE(second.connect_accepted);
    EXPECT_TRUE(second.gate_held_for_fragment);
    EXPECT_TRUE(second.peer_close_sent);
    const auto run = store->load(id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(run.config.scenario_ids, scenarios);
    EXPECT_EQ(run.outcomes.size(), draft21->requirements.size());
    EXPECT_EQ(store->list({10, 0}).total, 1u);
    std::set<std::string> actual_connection_ids;
    for (const auto& scenario : scenarios) {
        const auto established = std::find_if(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "transport_established" && event.scenario_id == scenario;
        });
        ASSERT_NE(established, run.events.end());
        ASSERT_TRUE(established->connection_id);
        ASSERT_FALSE(established->connection_id->empty());
        actual_connection_ids.insert(*established->connection_id);
        EXPECT_NE(established->detail.find("transport_event_index=0"), std::string::npos);
        EXPECT_EQ(std::count_if(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "context_ready" && event.scenario_id == scenario &&
                   event.detail.find("endpoint=" + endpoint) != std::string::npos;
        }), 1);
        EXPECT_EQ(std::count_if(run.events.begin(), run.events.end(), [&](const auto& event) {
            return event.kind == "context_complete" && event.scenario_id == scenario;
        }), 1);
    }
    EXPECT_EQ(actual_connection_ids.size(), 2u);
    const auto result = std::find_if(run.outcomes.begin(), run.outcomes.end(), [](const auto& outcome) {
        return outcome.requirement_id == "D21-9-2-MUST-328";
    });
    ASSERT_NE(result, run.outcomes.end());
    EXPECT_EQ(result->state, requirements::OutcomeState::Pass);
    EXPECT_TRUE(runs->stop(id));
}

TEST(WebTransportRunApi, RawFamilySubscriberNotifyUsesIndependentSessionsAndFullCatalog) {
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
    const std::vector<std::string> scenarios{
        "d21-subscriber-sends-publish-state-notify",
        "d21-publish-established-subscriber-sends-publish-state-notify"};
    const Json request{{"draft", 21}, {"transport", "webtransport"},
                       {"mode", "observed"}, {"scenarios", scenarios},
                       {"timeout_ms", 2500},
                       {"track", {{"namespace_hex", Json::array({"6e"})},
                                  {"name_hex", "74"}}}};
    const auto response = api.Post("/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 201) << response->body;
    const auto body = Json::parse(response->body);
    const auto id = body.at("run").at("id").get<std::string>();
    const auto endpoint = body.at("publisher_endpoint");
    const auto port = endpoint.at("port").get<unsigned>();
    ASSERT_GT(port, 0u);
    EXPECT_EQ(endpoint.at("url"), "https://127.0.0.1:" + std::to_string(port) + "/moq");
    EXPECT_EQ(endpoint.at("path"), "/moq");
    EXPECT_EQ(endpoint.at("protocol"), "moqt-21");
    EXPECT_EQ(endpoint.at("alpn"), "h3");
    EXPECT_EQ(body.at("run").at("config").at("scenarios"), Json(scenarios));
    EXPECT_EQ(store->load(id).config.scenario_ids, scenarios);

    RawFamilyContext first{.scenario_id = scenarios[0], .next_scenario_id = scenarios[1]};
    ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt,
        "D21-9-10-MUST-370", {0xaf, 0, 0, 0}, requirements::OutcomeState::Pass, &first));
    EXPECT_TRUE(first.connect_accepted);
    EXPECT_TRUE(first.gate_held_for_fragment);
    ASSERT_TRUE(first.request_stream_id);
    EXPECT_EQ(*first.request_stream_id & 3u, 1u);
    EXPECT_EQ(first.received, (std::vector<std::uint8_t>{
        3, 0, 7, 1, 1, 1, 'n', 1, 't', 0, 0x22, 0, 1, 0}));
    const auto between = store->load(id);
    ASSERT_EQ(between.state, storage::RunState::Active);
    EXPECT_FALSE(between.finalized_at_unix_ns);
    EXPECT_TRUE(between.outcomes.empty());
    EXPECT_FALSE(between.score);

    RawFamilyContext second{.scenario_id = scenarios[1], .publish_origin = true};
    ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt,
        "D21-9-10-MUST-370", {0xaf, 0, 0, 0}, requirements::OutcomeState::Pass, &second));
    EXPECT_TRUE(second.connect_accepted);
    EXPECT_TRUE(second.gate_held_for_fragment);
    ASSERT_TRUE(second.request_stream_id);
    EXPECT_EQ(*second.request_stream_id & 3u, 0u);
    EXPECT_NE(*second.request_stream_id, 0u);  // HTTP CONNECT occupies stream 0.
    EXPECT_EQ(second.received, (std::vector<std::uint8_t>{7, 0, 1, 0, 0x22, 0, 1, 0}));
    const auto record = store->load(id);
    ASSERT_EQ(record.state, storage::RunState::Finalized);
    EXPECT_TRUE(record.finalized_at_unix_ns);
    ASSERT_TRUE(record.score);
    EXPECT_EQ(record.outcomes.size(), draft21->requirements.size());
    EXPECT_EQ(record.config.scenario_ids, scenarios);
    EXPECT_EQ(store->list({10, 0}).total, 1u);
    const auto row = std::find_if(record.outcomes.begin(), record.outcomes.end(), [](const auto& outcome) {
        return outcome.requirement_id == "D21-9-10-MUST-370";
    });
    ASSERT_NE(row, record.outcomes.end());
    EXPECT_EQ(row->state, requirements::OutcomeState::Pass);
    std::vector<std::string> connection_ids;
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        const auto ready = std::find_if(record.events.begin(), record.events.end(), [&](const auto& event) {
            return event.kind == "context_ready" && event.scenario_id == scenarios[i];
        });
        ASSERT_NE(ready, record.events.end());
        EXPECT_NE(ready->detail.find("ordinal=" + std::to_string(i + 1)), std::string::npos);
        const auto established = std::find_if(record.events.begin(), record.events.end(), [&](const auto& event) {
            return event.kind == "transport_established" && event.scenario_id == scenarios[i];
        });
        ASSERT_NE(established, record.events.end());
        ASSERT_TRUE(established->connection_id);
        EXPECT_FALSE(established->connection_id->empty());
        connection_ids.push_back(*established->connection_id);
        const auto stimulus = std::find_if(record.events.begin(), record.events.end(), [&](const auto& event) {
            return event.kind == "raw_probe_stimulus" && event.scenario_id == scenarios[i];
        });
        ASSERT_NE(stimulus, record.events.end());
        EXPECT_NE(stimulus->detail.find("accepted=4 fin=false bytes=22000100 accepted_event_count="),
                  std::string::npos);
        EXPECT_TRUE(std::any_of(record.events.begin(), record.events.end(), [&](const auto& event) {
            return event.kind == "peer_close" && event.scenario_id == scenarios[i] &&
                   event.detail.find("application close code=3 transport_event_index=") == 0;
        }));
    }
    ASSERT_EQ(connection_ids.size(), 2u);
    EXPECT_NE(connection_ids[0], connection_ids[1]);
    const auto stopped = api.Post("/api/v1/runs/" + id + "/stop", "", "application/json");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->status, 409) << stopped->body;
    const auto after_stop = store->load(id);
    EXPECT_EQ(after_stop.finalized_at_unix_ns, record.finalized_at_unix_ns);
    EXPECT_EQ(after_stop.events.size(), record.events.size());
    const std::array records{record};
    EXPECT_TRUE(requirements::audit_execution(
        *draft21, requirements::draft21_executable_bindings(), records).consistent());
}

TEST(WebTransportRunApi, RawFamilySubscriberNotifyFailsForEitherWrongCloseContext) {
    for (const auto close_codes : {std::array{3u, 9u}, std::array{9u, 3u}}) {
        SCOPED_TRACE(close_codes[0]);
        SCOPED_TRACE(close_codes[1]);
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
        const std::vector<std::string> scenarios{
            "d21-subscriber-sends-publish-state-notify",
            "d21-publish-established-subscriber-sends-publish-state-notify"};
        const Json request{{"draft", 21}, {"transport", "webtransport"},
                           {"mode", "observed"}, {"scenarios", scenarios},
                           {"timeout_ms", 2500},
                           {"track", {{"namespace_hex", Json::array({"6e"})},
                                      {"name_hex", "74"}}}};
        const auto response = api.Post("/api/v1/runs", request.dump(), "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 201) << response->body;
        const auto body = Json::parse(response->body);
        const auto id = body.at("run").at("id").get<std::string>();
        const auto port = body.at("publisher_endpoint").at("port").get<unsigned>();
        RawFamilyContext first{.scenario_id = scenarios[0], .next_scenario_id = scenarios[1],
                               .close_code = close_codes[0]};
        ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt,
            "D21-9-10-MUST-370", {0xaf, 0, 0, 0}, requirements::OutcomeState::Fail, &first));
        ASSERT_EQ(store->load(id).state, storage::RunState::Active);
        RawFamilyContext second{.scenario_id = scenarios[1], .publish_origin = true,
                                .close_code = close_codes[1]};
        ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt,
            "D21-9-10-MUST-370", {0xaf, 0, 0, 0}, requirements::OutcomeState::Fail, &second));
        EXPECT_TRUE(first.connect_accepted);
        EXPECT_TRUE(second.connect_accepted);
        EXPECT_TRUE(first.gate_held_for_fragment);
        EXPECT_TRUE(second.gate_held_for_fragment);
        const auto run = store->load(id);
        EXPECT_EQ(run.state, storage::RunState::Finalized);
        EXPECT_EQ(run.outcomes.size(), draft21->requirements.size());
        EXPECT_EQ(store->list({10, 0}).total, 1u);
    }
}

TEST(WebTransportRunApi, RawFamilyCancellationAfterFirstSessionCannotPassFullRow) {
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
    api.set_read_timeout(2, 0);
    const std::vector<std::string> scenarios{
        "d21-subscriber-sends-publish-state-notify",
        "d21-publish-established-subscriber-sends-publish-state-notify"};
    const Json request{{"draft", 21}, {"transport", "webtransport"},
                       {"mode", "observed"}, {"scenarios", scenarios},
                       {"timeout_ms", 2500},
                       {"track", {{"namespace_hex", Json::array({"6e"})},
                                  {"name_hex", "74"}}}};
    const auto response = api.Post("/api/v1/runs", request.dump(), "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 201) << response->body;
    const auto body = Json::parse(response->body);
    const auto id = body.at("run").at("id").get<std::string>();
    const auto port = body.at("publisher_endpoint").at("port").get<unsigned>();
    RawFamilyContext first{.scenario_id = scenarios[0], .next_scenario_id = scenarios[1]};
    ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt,
        "D21-9-10-MUST-370", {0xaf, 0, 0, 0}, requirements::OutcomeState::Pass, &first));
    ASSERT_EQ(store->load(id).state, storage::RunState::Active);
    const auto stop_started = std::chrono::steady_clock::now();
    const auto stopped = api.Post("/api/v1/runs/" + id + "/stop", "", "application/json");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->status, 200) << stopped->body;
    EXPECT_LT(std::chrono::steady_clock::now() - stop_started, std::chrono::seconds{2});
    const auto run = store->load(id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(run.outcomes.size(), draft21->requirements.size());
    const auto row = std::find_if(run.outcomes.begin(), run.outcomes.end(), [](const auto& outcome) {
        return outcome.requirement_id == "D21-9-10-MUST-370";
    });
    ASSERT_NE(row, run.outcomes.end());
    EXPECT_EQ(row->state, requirements::OutcomeState::NotRun);
    EXPECT_EQ(std::count_if(run.events.begin(), run.events.end(), [](const auto& event) {
        return event.kind == "transport_established";
    }), 1);
    const int rebound = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(rebound, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    EXPECT_EQ(::bind(rebound, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ::close(rebound);
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
        const auto record = store->load(id);
        const auto bindings = draft == 18
            ? requirements::draft18_executable_bindings()
            : requirements::draft21_executable_bindings();
        const std::array records{record};
        const auto audit = requirements::audit_execution(
            draft == 18 ? *draft18 : *draft21, bindings, records);
        for (const auto& finding : audit.findings) {
            EXPECT_NE(finding.code, "missing_evaluator_evidence")
                << finding.requirement_id << " " << finding.detail;
        }
    }
}

// A draft 22 run over WebTransport (Draft22Wt16), wired as main.cpp wires it: the endpoint is h3 with the
// WebTransport protocol moqt-22, a publisher offering moqt-22 in its CONNECT completes the session, one offering
// moqt-21 does not, and the stored run is a draft 22 run with draft 22 evidence.
TEST(WebTransportRunApi, Draft22RunNegotiatesMoqt22OverH3) {
    const app::BuildInfo build{"test", "test", {}};
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", build);
    auto draft18 = catalog(18);
    auto draft21 = catalog(21);
    auto draft22 = catalog(22);
    auto runs = std::make_shared<app::NativeRunManager>(draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
            .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
            .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"},
        draft22);
    http::ServerConfig config;
    config.port = 0;
    config.draft22_catalog = draft22;
    http::HttpServer server(draft18, draft21, store, build, config, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    const auto start = [&](unsigned timeout_ms) {
        const Json request{{"draft", 22}, {"transport", "webtransport"},
                           {"mode", "observed"},
                           {"scenarios", Json::array({"d22-publisher-request-stream-placement"})},
                           {"timeout_ms", timeout_ms},
                           {"track", {{"namespace_hex", Json::array({"6e"})},
                                      {"name_hex", "78"}}}};
        const auto response = api.Post("/api/v1/runs", request.dump(), "application/json");
        EXPECT_TRUE(response);
        EXPECT_EQ(response ? response->status : 0, 201) << (response ? response->body : "");
        return response ? Json::parse(response->body) : Json{};
    };
    const auto stop = [&](const std::string& id) {
        const auto stopped = api.Post("/api/v1/runs/" + id + "/stop", "", "application/json");
        ASSERT_TRUE(stopped);
        if (stopped->status == 409) {
            EXPECT_EQ(store->load(id).state, storage::RunState::Finalized);
        } else {
            EXPECT_EQ(stopped->status, 200) << stopped->body;
        }
    };

    {
        const auto body = start(1000);
        ASSERT_FALSE(body.empty());
        const auto& endpoint = body.at("publisher_endpoint");
        EXPECT_EQ(endpoint.at("alpn"), "h3");
        EXPECT_EQ(endpoint.at("protocol"), "moqt-22");
        EXPECT_EQ(endpoint.at("path"), "/moq");
        const auto port = endpoint.at("port").get<unsigned>();
        EXPECT_EQ(endpoint.at("url"), "https://127.0.0.1:" + std::to_string(port) + "/moq");
        EXPECT_EQ(body.at("run").at("config").at("draft"), 22);
        EXPECT_EQ(body.at("run").at("config").at("transport"), "webtransport");
        const auto id = body.at("run").at("id").get<std::string>();
        EXPECT_TRUE(publish_setup(port, 22, store, id));
        EXPECT_TRUE(last_connect_accepted);
        stop(id);
        const auto record = store->load(id);
        EXPECT_EQ(record.config.draft, app::DraftVersion::Draft22);
        EXPECT_EQ(record.config.transport, app::TransportKind::WebTransport);
        std::size_t stamped = 0;
        for (const auto& event : record.events) {
            if (!event.scenario_id || event.scenario_id->empty()) continue;
            ++stamped;
            EXPECT_TRUE(event.scenario_id->starts_with("d22-")) << event.kind << " " << *event.scenario_id;
        }
        EXPECT_GT(stamped, 0u);
        const std::array records{record};
        const auto audit = requirements::audit_execution(
            *draft22, requirements::draft22_executable_bindings(), records);
        EXPECT_TRUE(audit.consistent());
        for (const auto& finding : audit.findings)
            ADD_FAILURE() << finding.code << " " << finding.requirement_id << " " << finding.detail;
        // The run is reported like any WebTransport run: completeness counts it under draft 22's webtransport.
        const auto completeness = api.Get("/results/completeness.json");
        ASSERT_TRUE(completeness);
        ASSERT_EQ(completeness->status, 200);
        const auto summary = Json::parse(completeness->body);
        const auto entry = std::find_if(summary.at("drafts").begin(), summary.at("drafts").end(),
                                        [](const Json& value) { return value.at("draft") == 22; });
        ASSERT_NE(entry, summary.at("drafts").end());
        const auto transport = std::find_if(entry->at("transports").begin(), entry->at("transports").end(),
                                            [](const Json& value) { return value.at("transport") == "webtransport"; });
        ASSERT_NE(transport, entry->at("transports").end());
        EXPECT_EQ(transport->at("run_count"), 1);
        EXPECT_TRUE(transport->at("execution_consistent")) << transport->at("execution_findings").dump();
    }

    // A publisher offering moqt-21 never gets a session on a draft 22 run: its CONNECT is refused.
    {
        const auto body = start(1000);
        ASSERT_FALSE(body.empty());
        const auto id = body.at("run").at("id").get<std::string>();
        const auto port = body.at("publisher_endpoint").at("port").get<unsigned>();
        EXPECT_FALSE(publish_setup(port, 21, store, id));
        EXPECT_FALSE(last_connect_accepted) << "the listener accepted a CONNECT offering moqt-21";
        const auto record = store->load(id);
        EXPECT_FALSE(std::any_of(record.events.begin(), record.events.end(), [](const auto& event) {
            return event.kind == "peer_setup_received";
        }));
        stop(id);
    }
}

TEST(WebTransportRunApi, ScoresForbiddenServerSetupOptionsByTransport) {
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
    for (const auto& [scenario, row, code] : {
             std::tuple{"d21-server-sends-authority", "D21-9-1-1-MUST-294", 0x19u},
             std::tuple{"d21-server-sends-path", "D21-9-1-2-MUST-301", 0x8u}}) {
        const Json request{{"draft", 21}, {"transport", "webtransport"},
                           {"mode", "observed"},
                           {"scenarios", Json::array({scenario})},
                           {"timeout_ms", 2000},
                           {"track", {{"namespace_hex", Json::array({"6e"})},
                                      {"name_hex", "78"}}}};
        const auto response = api.Post("/api/v1/runs", request.dump(),
                                       "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 201) << response->body;
        const auto body = Json::parse(response->body);
        const auto id = body.at("run").at("id").get<std::string>();
        const auto port = body.at("publisher_endpoint").at("port").get<unsigned>();
        ASSERT_TRUE(publish_setup(port, 21, store, id, code, row));
        const auto record = store->load(id);
        ASSERT_EQ(record.state, storage::RunState::Finalized);
        EXPECT_EQ(std::count_if(record.outcomes.begin(), record.outcomes.end(),
                                [&](const auto& outcome) {
            return outcome.requirement_id == row &&
                   outcome.state == requirements::OutcomeState::Pass;
        }), 1);
        const std::array records{record};
        const auto audit = requirements::audit_execution(
            *draft21, requirements::draft21_executable_bindings(), records);
        EXPECT_TRUE(audit.consistent());
    }
}

TEST(WebTransportRunApi, ScoresPublisherSetupOptionViolationsWithEvidence) {
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
    for (const auto& [row, option, evidence] : {
             std::tuple{"D21-9-1-1-MUST-NOT-292",
                        std::vector<std::uint8_t>{0xaf, 0, 0, 3, 5, 1, 'x'},
                        "option types: 5"},
             std::tuple{"D21-9-1-2-MUST-NOT-299",
                        std::vector<std::uint8_t>{0xaf, 0, 0, 3, 1, 1, '/'},
                        "option types: 1"},
             std::tuple{"D21-9-1-MUST-NOT-289",
                        std::vector<std::uint8_t>{0xaf, 0, 0, 4, 4, 1, 0, 2},
                        "option types: 4,4"}}) {
        const Json request{{"draft", 21}, {"transport", "webtransport"},
                           {"mode", "observed"},
                           {"scenarios", Json::array({"d21-publisher-request-stream-placement"})},
                           {"timeout_ms", 2000},
                           {"track", {{"namespace_hex", Json::array({"6e"})},
                                      {"name_hex", "78"}}}};
        const auto response = api.Post("/api/v1/runs", request.dump(),
                                       "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 201) << response->body;
        const auto body = Json::parse(response->body);
        const auto id = body.at("run").at("id").get<std::string>();
        const auto port = body.at("publisher_endpoint").at("port").get<unsigned>();
        ASSERT_TRUE(publish_setup(port, 21, store, id, std::nullopt, row,
                                  option, requirements::OutcomeState::Fail));
        const auto record = store->load(id);
        ASSERT_EQ(record.state, storage::RunState::Finalized);
        const auto received = std::find_if(record.events.begin(),
            record.events.end(), [](const auto& event) {
                return event.kind == "peer_setup_received";
            });
        ASSERT_NE(received, record.events.end());
        EXPECT_NE(received->detail.find(evidence),
                  std::string::npos);
        const std::array records{record};
        EXPECT_TRUE(requirements::audit_execution(
            *draft21, requirements::draft21_executable_bindings(), records).consistent());
    }
}

TEST(WebTransportRunApi, ScoresDraft18PublisherSetupViolationsWithEvidence) {
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
    for (const auto& [row, option, evidence] : {
             std::tuple{"D18-10-3-MUST-NOT-001",
                        std::vector<std::uint8_t>{0xaf, 0, 0, 4, 4, 1, 0, 2},
                        "option types: 4,4"},
             std::tuple{"D18-10-3-1-1-MUST-NOT-002",
                        std::vector<std::uint8_t>{0xaf, 0, 0, 3, 5, 1, 'x'},
                        "option types: 5"},
             std::tuple{"D18-10-3-1-2-MUST-NOT-002",
                        std::vector<std::uint8_t>{0xaf, 0, 0, 3, 1, 1, '/'},
                        "option types: 1"}}) {
        const Json request{{"draft", 18}, {"transport", "webtransport"},
                           {"mode", "observed"},
                           {"scenarios", Json::array({"subscribe-to-publisher-track"})},
                           {"timeout_ms", 2000},
                           {"track", {{"namespace_hex", Json::array({"6e"})},
                                      {"name_hex", "78"}}}};
        const auto response = api.Post("/api/v1/runs", request.dump(),
                                       "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 201) << response->body;
        const auto body = Json::parse(response->body);
        const auto id = body.at("run").at("id").get<std::string>();
        const auto port = body.at("publisher_endpoint").at("port").get<unsigned>();
        ASSERT_TRUE(publish_setup(port, 18, store, id, std::nullopt, row,
                                  option, requirements::OutcomeState::Fail));
        const auto record = store->load(id);
        ASSERT_EQ(record.state, storage::RunState::Finalized);
        const auto received = std::find_if(record.events.begin(),
            record.events.end(), [](const auto& event) {
                return event.kind == "peer_setup_received";
            });
        ASSERT_NE(received, record.events.end());
        EXPECT_NE(received->detail.find(evidence), std::string::npos);
        if (row == std::string{"D18-10-3-MUST-NOT-001"}) {
            const auto duplicate = std::find_if(record.events.begin(),
                record.events.end(), [](const auto& event) {
                    return event.kind == "setup_option_duplicate";
                });
            ASSERT_NE(duplicate, record.events.end());
            EXPECT_NE(duplicate->detail.find("option type 4"),
                      std::string::npos);
        }
        const std::array records{record};
        EXPECT_TRUE(requirements::audit_execution(
            *draft18, requirements::draft18_executable_bindings(), records).consistent());
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

// ---- Replacement-session port budget -------------------------------------------------

constexpr const char* kReplacementScenario = "d21-publisher-goaway-alternate-uri";

bool udp_port_is_free(unsigned port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return bound;
}

// Two adjacent UDP ports that are free right now.
std::uint16_t adjacent_free_ports() {
    for (unsigned port = 41000 + static_cast<unsigned>(::getpid() % 2000) * 8; port < 60000; port += 2)
        if (udp_port_is_free(port) && udp_port_is_free(port + 1)) return static_cast<std::uint16_t>(port);
    return 0;
}

struct ReplacementFixture {
    std::shared_ptr<storage::SqliteRunStore> store;
    std::unique_ptr<app::NativeRunManager> manager;
    std::uint16_t first{0};
    std::uint16_t last{0};
};

ReplacementFixture replacement_fixture(unsigned span, std::size_t maximum_runs) {
    ReplacementFixture fixture;
    fixture.first = adjacent_free_ports();
    fixture.last = static_cast<std::uint16_t>(fixture.first + span - 1);
    fixture.store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    fixture.manager = std::make_unique<app::NativeRunManager>(catalog(18), catalog(21), fixture.store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = fixture.first, .port_end = fixture.last, .maximum_active_runs = maximum_runs,
            .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
            .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem"});
    return fixture;
}

app::RunConfig replacement_run(std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    return app::RunConfig{app::DraftVersion::Draft21, app::TransportKind::WebTransport,
        app::RunMode::Observed, {kReplacementScenario}, timeout, app::TrackFixture{{"n"}, "x"}};
}

app::RunConfig plain_run() {
    return app::RunConfig{app::DraftVersion::Draft18, app::TransportKind::WebTransport,
        app::RunMode::Observed, {"subscribe-to-publisher-track"}, std::chrono::seconds{20},
        app::TrackFixture{{"n"}, "x"}};
}

TEST(NativeRunManagerReplacement, HoldsBothPortsOfATwoPortRangeAndReleasesThemAfterwards) {
    auto fixture = replacement_fixture(2, 2);
    ASSERT_NE(fixture.first, 0);
    const auto started = fixture.manager->start(replacement_run());
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    EXPECT_EQ(started.endpoint.port, fixture.first);
    // The run's second port is already spoken for: no unrelated run can take it.
    EXPECT_EQ(fixture.manager->start(plain_run()).status, app::RunStartStatus::PortExhausted);
    EXPECT_EQ(fixture.manager->start(replacement_run()).status, app::RunStartStatus::PortExhausted);
    EXPECT_TRUE(fixture.manager->stop(started.id));
    EXPECT_TRUE(udp_port_is_free(fixture.first));
    EXPECT_TRUE(udp_port_is_free(fixture.last));
    // Both ports are reusable: a second replacement run starts on the same range.
    const auto again = fixture.manager->start(replacement_run());
    ASSERT_EQ(again.status, app::RunStartStatus::Started);
    EXPECT_TRUE(fixture.manager->stop(again.id));
}

TEST(NativeRunManagerReplacement, RejectsAReplacementRunInAOnePortRangeButStillStartsOthers) {
    auto fixture = replacement_fixture(1, 1);
    ASSERT_NE(fixture.first, 0);
    EXPECT_EQ(fixture.manager->start(replacement_run()).status, app::RunStartStatus::PortExhausted);
    EXPECT_EQ(fixture.store->list({1, 0}).total, 0u);
    const auto plain = fixture.manager->start(plain_run());
    ASSERT_EQ(plain.status, app::RunStartStatus::Started);
    EXPECT_TRUE(fixture.manager->stop(plain.id));
}

TEST(NativeRunManagerReplacement, ARunWhoseSecondListenerFailsEndsAsAnErrorAndLeaksNeitherPort) {
    auto fixture = replacement_fixture(2, 2);
    ASSERT_NE(fixture.first, 0);
    // Something else holds the replacement port, so creating its listener throws inside the run.
    const int blocker = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(blocker, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(fixture.last);
    ASSERT_EQ(::bind(blocker, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

    const auto started = fixture.manager->start(replacement_run());
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    // No publisher connects; the run fails on its own when the second listener cannot bind.
    auto run_is_finalized = [&] {
        const auto run = fixture.store->load(started.id);
        return run.state == storage::RunState::Finalized;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    while (!run_is_finalized() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    ASSERT_TRUE(run_is_finalized());
    const auto finished = fixture.store->load(started.id);
    ASSERT_TRUE(finished.score);
    EXPECT_EQ(finished.score->verdict, requirements::RunVerdict::Error);
    ::close(blocker);
    // Neither the run's port nor the reserved replacement port is still counted: both
    // allow a fresh replacement run once the finished one is reaped.
    const auto again = fixture.manager->start(replacement_run());
    ASSERT_EQ(again.status, app::RunStartStatus::Started);
    EXPECT_TRUE(fixture.manager->stop(again.id));
    EXPECT_TRUE(udp_port_is_free(fixture.first));
    EXPECT_TRUE(udp_port_is_free(fixture.last));
}

TEST(WebTransportRunApi, DrivenPublisherExitIsDiagnosticOnly) {
    const app::BuildInfo build{"test", "test", {}};
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", build);
    auto draft18 = catalog(18);
    auto draft21 = catalog(21);
    const auto log_root = std::filesystem::temp_directory_path() /
        ("moq-interop-driven-" + std::to_string(::getpid()));
    auto runs = std::make_shared<app::NativeRunManager>(draft18, draft21, store,
        app::NativeRunManagerConfig{
            .bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
            .port_start = 0, .port_end = 0, .maximum_active_runs = 2,
            .certificate_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "cert.pem",
            .private_key_path = std::filesystem::path{PICOQUIC_TEST_CERT_DIR} / "key.pem",
            .driver_executable = "/bin/true", .driver_log_root = log_root});
    http::HttpServer server(draft18, draft21, store, build, {.port = 0}, runs);
    ASSERT_TRUE(server.start());
    httplib::Client api("127.0.0.1", server.port());
    for (const int draft : {18, 21}) {
        const Json request{{"draft", draft}, {"transport", "webtransport"},
                           {"mode", "driven"},
                           {"scenarios", Json::array({draft == 18
                               ? "subscribe-to-publisher-track"
                               : "d21-publisher-request-stream-placement"})},
                           {"timeout_ms", 200},
                           {"track", {{"namespace_hex", Json::array({"6e"})},
                                       {"name_hex", "78"}}}};
        const auto response = api.Post("/api/v1/runs", request.dump(), "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 201) << response->body;
        const auto id = Json::parse(response->body).at("run").at("id").get<std::string>();
        for (int i = 0; i < 300 && store->load(id).state != storage::RunState::Finalized; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto run = store->load(id);
        ASSERT_EQ(run.state, storage::RunState::Finalized);
        EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Error);
        bool saw_exit = false;
        for (const auto& event : run.events)
            if (event.kind == "publisher_process" &&
                event.detail.find("\"exit_code\":0") != std::string::npos)
                saw_exit = true;
        EXPECT_TRUE(saw_exit);
        EXPECT_TRUE(std::filesystem::exists(log_root / id / "request.json"));
    }
}

}  // namespace
}  // namespace moq::interop
