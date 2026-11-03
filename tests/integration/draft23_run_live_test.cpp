#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"
#include "support/picoquic_client.h"
#include <gtest/gtest.h>
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
#include <thread>

namespace moq::interop {
namespace {
using namespace std::chrono_literals;
using Bytes=std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> input) {
    Bytes result;
    for (const auto value:input) result.push_back(static_cast<std::byte>(value));
    return result;
}
Bytes publish_message() { return b({0x1d,15,0,1,5,'m','e','d','i','a',4,'v','i','d','e',7,0}); }
Bytes media_objects(unsigned count) {
    Bytes result=b({0x30,7,0});
    for (unsigned i=0;i<count;++i) {
        const auto object=b({0,1,static_cast<unsigned>('a')+i});
        result.insert(result.end(),object.begin(),object.end());
    }
    return result;
}
std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const std::filesystem::path root(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source=requirements::load_draft_source(draft,root/"docs",root/"requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source,root/("requirements/draft"+std::to_string(draft)+".json"),requirements::CatalogLoadMode::AllowIncomplete));
}
app::NativeRunManager manager_for(const std::shared_ptr<storage::SqliteRunStore>& store) {
    return app::NativeRunManager(catalog(18),catalog(21),store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",.port_start=0,.port_end=0,.maximum_active_runs=1,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem"},catalog(22),nullptr,catalog(23));
}
app::RunConfig config(app::TransportKind transport, std::chrono::milliseconds timeout=2000ms) {
    return {app::DraftVersion::Draft23,transport,app::RunMode::Observed,{"d23-publisher-smoke"},timeout,
        app::TrackFixture{{"media"},"vide"},{}};
}
bool wait_finalized(const std::shared_ptr<storage::SqliteRunStore>& store,const app::RunId& id,
                    const std::function<bool()>& pump={}) {
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while (std::chrono::steady_clock::now()<deadline) {
        if (store->load(id).state==storage::RunState::Finalized) return true;
        if (pump) (void)pump();
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
nlohmann::json smoke(const storage::RunRecord& run) {
    for (const auto& event:run.events) if (event.kind=="draft23_smoke") return nlohmann::json::parse(event.detail);
    ADD_FAILURE()<<"missing draft23_smoke evidence";
    return nlohmann::json::object();
}
void expect_unscored(const storage::RunRecord& run) {
    ASSERT_TRUE(run.score);
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Incomplete);
    EXPECT_EQ(run.outcomes.size(),catalog(23)->requirements.size());
    for (const auto& row:run.outcomes) EXPECT_EQ(row.state,requirements::OutcomeState::NotRun)<<row.requirement_id;
}
class Draft23WebTransportPublisher {
public:
    Draft23WebTransportPublisher(std::uint16_t port, Bytes setup_stream, std::string target = "/moq")
        : port_(port), setup_(std::move(setup_stream)), target_(std::move(target)) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) return;
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
            return;
        socklen_t size = sizeof(local);
        (void)::getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &size);
        local_ = local;
        remote_.sin_family = AF_INET;
        remote_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        remote_.sin_port = htons(port);
        quic_ = picoquic_create(1, nullptr, nullptr, nullptr, "h3", nullptr, nullptr, nullptr, nullptr, nullptr,
                                picoquic_current_time(), nullptr, nullptr, nullptr, 0);
        if (!quic_) return;
        picoquic_set_null_verifier(quic_);
        ok_ = picowt_prepare_client_cnx(quic_, reinterpret_cast<sockaddr*>(&remote_), &cnx_, &h3_, &control_,
                                        picoquic_current_time(), "runner.test") == 0 &&
              h3zero_declare_stream_prefix(h3_, control_->stream_id, callback, this) == 0 &&
              picoquic_start_client_cnx(cnx_) == 0;
    }
    ~Draft23WebTransportPublisher() {
        if (h3_ != nullptr) h3zero_callback_delete_context(cnx_, h3_);
        if (quic_ != nullptr) picoquic_free(quic_);
        if (fd_ >= 0) ::close(fd_);
    }
    Draft23WebTransportPublisher(const Draft23WebTransportPublisher&) = delete;
    Draft23WebTransportPublisher& operator=(const Draft23WebTransportPublisher&) = delete;

    [[nodiscard]] bool accepted() const { return accepted_; }
    [[nodiscard]] bool setup_sent() const { return setup_sent_; }

    bool step() {
        if (!ok_) return false;
        std::array<std::uint8_t, 2048> buffer{};
        for (int packet = 0; packet < 16; ++packet) {
            sockaddr_storage destination{};
            sockaddr_storage source{};
            picoquic_connection_id_t log_id{};
            picoquic_cnx_t* last = nullptr;
            std::size_t length = 0;
            int interface_index = 0;
            if (picoquic_prepare_next_packet(quic_, picoquic_current_time(), buffer.data(), buffer.size(), &length,
                                             &destination, &source, &interface_index, &log_id, &last) != 0 ||
                length == 0)
                break;
            (void)::sendto(fd_, buffer.data(), length, 0, reinterpret_cast<sockaddr*>(&destination), sizeof(remote_));
        }
        for (int packet = 0; packet < 16; ++packet) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const auto length = ::recvfrom(fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&peer),
                                           &peer_size);
            if (length < 0) break;
            auto received_at = local_;
            (void)picoquic_incoming_packet(quic_, buffer.data(), static_cast<std::size_t>(length),
                                           reinterpret_cast<sockaddr*>(&peer),
                                           reinterpret_cast<sockaddr*>(&received_at), 0, 0, picoquic_current_time());
        }
        if (!connect_sent_ && h3_->settings.settings_received) {
            const std::string authority = "127.0.0.1:" + std::to_string(port_);
            const std::string offered = "\"" + std::string("moqt-23") + "\"";
            std::array<std::uint8_t, 512> qpack{};
            auto* end = h3zero_create_connect_header_frame(qpack.data(), qpack.data() + qpack.size(),
                                                           authority.c_str(),
                                                           reinterpret_cast<const std::uint8_t*>(target_.data()), target_.size(),
                                                           "webtransport-h3", nullptr, nullptr, offered.c_str());
            if (end == nullptr) return false;
            std::array<std::uint8_t, 1024> frame{};
            auto* cursor = picoquic_frames_varint_encode(frame.data(), frame.data() + frame.size(), h3zero_frame_header);
            cursor = picoquic_frames_varint_encode(cursor, frame.data() + frame.size(),
                                                   static_cast<std::uint64_t>(end - qpack.data()));
            if (cursor == nullptr) return false;
            std::memcpy(cursor, qpack.data(), static_cast<std::size_t>(end - qpack.data()));
            cursor += end - qpack.data();
            control_->path_callback = callback;
            control_->path_callback_ctx = this;
            control_->is_open = 1;
            control_->ps.stream_state.is_upgrade_requested = 1;
            if (picoquic_add_to_stream_with_ctx(cnx_, control_->stream_id, frame.data(),
                                                static_cast<std::size_t>(cursor - frame.data()), 0, control_) != 0)
                return false;
            connect_sent_ = true;
        }
        if (accepted_ && !setup_sent_) {
            auto* stream = picowt_create_local_stream(cnx_, 0, h3_, control_->stream_id);
            if (stream == nullptr ||
                picoquic_add_to_stream_with_ctx(cnx_, stream->stream_id, reinterpret_cast<const std::uint8_t*>(setup_.data()),
                                                setup_.size(), 0, stream) != 0)
                return false;
            auto* request = picowt_create_local_stream(cnx_, 1, h3_, control_->stream_id);
            auto* data = picowt_create_local_stream(cnx_, 0, h3_, control_->stream_id);
            const auto publish = publish_message();
            const auto objects = media_objects(2);
            if (!request || !data ||
                picoquic_add_to_stream_with_ctx(cnx_,request->stream_id,reinterpret_cast<const std::uint8_t*>(publish.data()),publish.size(),0,request) != 0 ||
                picoquic_add_to_stream_with_ctx(cnx_,data->stream_id,reinterpret_cast<const std::uint8_t*>(objects.data()),objects.size(),1,data) != 0) return false;
            setup_sent_ = true;
        }
        return true;
    }

private:
    static int callback(picoquic_cnx_t*, std::uint8_t*, std::size_t, picohttp_call_back_event_t event,
                        h3zero_stream_ctx_t*, void* context) {
        if (event == picohttp_callback_connect_accepted) static_cast<Draft23WebTransportPublisher*>(context)->accepted_ = true;
        return 0;
    }

    std::uint16_t port_;
    Bytes setup_;
    std::string target_;
    int fd_{-1};
    sockaddr_in local_{};
    sockaddr_in remote_{};
    picoquic_quic_t* quic_{nullptr};
    picoquic_cnx_t* cnx_{nullptr};
    h3zero_callback_ctx_t* h3_{nullptr};
    h3zero_stream_ctx_t* control_{nullptr};
    bool ok_{false};
    bool connect_sent_{false};
    bool accepted_{false};
    bool setup_sent_{false};
};


TEST(Draft23RunLive, NativeQuicStoresProgressingMediaAsUnscoredSmoke) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=manager_for(store);
    const auto started=manager.start(config(app::TransportKind::NativeQuic));
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    auto client=transport::test::PicoquicTestClient::create({.port=started.endpoint.port,.alpn=b({'m','o','q','t','-','2','3'})});
    ASSERT_TRUE(client);
    bool sent=false;
    ASSERT_TRUE(wait_finalized(store,started.id,[&] {
        if (!client->pump()) return false;
        if (!sent && client->established()) {
            EXPECT_TRUE(client->send_stream(2,b({0xaf,0,0}),false));
            EXPECT_TRUE(client->send_stream(0,publish_message(),false));
            EXPECT_TRUE(client->send_stream(6,media_objects(2),true));
            sent=true;
        }
        return true;
    }));
    const auto run=store->load(started.id);
    const auto summary=smoke(run);
    EXPECT_EQ(summary.at("status"),"PASS");
    EXPECT_EQ(summary.at("distinct_objects"),2);
    EXPECT_EQ(summary.at("payload_bytes"),2);
    EXPECT_FALSE(summary.at("conformance_scored"));
    expect_unscored(run);
}
TEST(Draft23RunLive, WebTransportStoresProgressingMediaAsUnscoredSmoke) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=manager_for(store);
    const auto started=manager.start(config(app::TransportKind::WebTransport));
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    Draft23WebTransportPublisher publisher(started.endpoint.port,b({0xaf,0,0}));
    ASSERT_TRUE(wait_finalized(store,started.id,[&] { return publisher.step(); }));
    EXPECT_TRUE(publisher.accepted());
    EXPECT_TRUE(publisher.setup_sent());
    const auto run=store->load(started.id);
    const auto summary=smoke(run);
    EXPECT_EQ(summary.at("status"),"PASS")<<summary.dump();
    EXPECT_EQ(summary.at("distinct_objects"),2);
    EXPECT_EQ(summary.at("payload_bytes"),2);
    expect_unscored(run);
}
TEST(Draft23RunLive, MissingPublisherIsHarnessError) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=manager_for(store);
    const auto started=manager.start(config(app::TransportKind::NativeQuic,100ms));
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    ASSERT_TRUE(wait_finalized(store,started.id));
    const auto run=store->load(started.id);
    ASSERT_TRUE(run.score);
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Error);
    EXPECT_EQ(smoke(run).at("status"),"NOT_RUN");
}
TEST(Draft23RunLive, OneObjectCannotPassProgressingMedia) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=manager_for(store);
    const auto started=manager.start(config(app::TransportKind::NativeQuic,300ms));
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    auto client=transport::test::PicoquicTestClient::create({.port=started.endpoint.port,.alpn=b({'m','o','q','t','-','2','3'})});
    ASSERT_TRUE(client);
    bool sent=false;
    ASSERT_TRUE(wait_finalized(store,started.id,[&] {
        if (!client->pump()) return false;
        if (!sent && client->established()) {
            EXPECT_TRUE(client->send_stream(2,b({0xaf,0,0}),false));
            EXPECT_TRUE(client->send_stream(0,publish_message(),false));
            EXPECT_TRUE(client->send_stream(6,media_objects(1),true)); sent=true;
        }
        return true;
    }));
    const auto run=store->load(started.id);
    EXPECT_EQ(smoke(run).at("status"),"NOT_RUN");
    EXPECT_EQ(smoke(run).at("distinct_objects"),1);
    expect_unscored(run);
}
TEST(Draft23RunLive, MalformedSetupCannotPassSmoke) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=manager_for(store);
    const auto started=manager.start(config(app::TransportKind::NativeQuic,300ms));
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    auto client=transport::test::PicoquicTestClient::create({.port=started.endpoint.port,.alpn=b({'m','o','q','t','-','2','3'})});
    ASSERT_TRUE(client);
    bool sent=false;
    ASSERT_TRUE(wait_finalized(store,started.id,[&] {
        if (!client->pump()) return false;
        if (!sent && client->established()) {
            EXPECT_TRUE(client->send_stream(2,b({0xaf,0,3,9,1,0}),false)); sent=true;
        }
        return true;
    }));
    const auto summary=smoke(store->load(started.id));
    EXPECT_EQ(summary.at("status"),"NOT_RUN");
    EXPECT_FALSE(summary.at("setup_received"));
    ASSERT_TRUE(summary.contains("error"));
    EXPECT_NE(summary.at("error").get<std::string>().find("SETUP"),std::string::npos);
}
} // namespace
} // namespace moq::interop
