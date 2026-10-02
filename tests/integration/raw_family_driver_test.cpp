#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace moq::interop {
namespace {
using namespace std::chrono_literals;

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source=requirements::load_draft_source(draft,root/"docs",root/"requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(
        requirements::RequirementCatalog::load(source,root/"requirements"/("draft"+std::to_string(draft)+".json")));
}

class DriverLogs {
public:
    DriverLogs() : path(std::filesystem::temp_directory_path()/
        ("moq-family-driver-"+std::to_string(getpid())+"-"+std::to_string(next++))) {
        std::filesystem::create_directories(path);
    }
    ~DriverLogs() { std::filesystem::remove_all(path); }
    const std::filesystem::path path;
private:
    inline static unsigned next=0;
};

class RemoveDriverAfterFirstContext final : public storage::RunStore {
public:
    RemoveDriverAfterFirstContext(std::shared_ptr<storage::SqliteRunStore> store,
                                 std::filesystem::path executable)
        : store_(std::move(store)), executable_(std::move(executable)) {}
    app::RunId create_run(const app::RunConfig& config) override { return store_->create_run(config); }
    void append_events(const app::RunId& id, std::span<const storage::EvidenceEvent> events) override {
        store_->append_events(id,events);
        for (const auto& event : events) {
            if (event.kind=="context_complete" &&
                event.scenario_id=="d21-duplicate-request-goaway" && !removed.exchange(true)) {
                // Remove only this test's copied executable, after the first
                // real publisher has been retired and before the next spawn.
                if (!std::filesystem::remove(executable_)) throw std::runtime_error("failed to remove test executable");
            }
        }
    }
    void finalize(const app::RunId& id, const requirements::ScoreSummary& score,
                  std::span<const requirements::Outcome> outcomes) override { store_->finalize(id,score,outcomes); }
    storage::RunRecord load(const app::RunId& id) const override { return store_->load(id); }
    storage::Page<storage::RunSummary> list(storage::RunQuery query) const override { return store_->list(query); }
    storage::Page<storage::EvidenceEvent> list_events(const app::RunId& id,storage::RunQuery query) const override {
        return store_->list_events(id,query);
    }
    std::atomic<bool> removed{false};
private:
    const std::shared_ptr<storage::SqliteRunStore> store_;
    const std::filesystem::path executable_;
};

TEST(RawFamilyDriver, RestartsRealPublisherForEachContextAndRetainsDistinctLogs) {
    for (const std::string mode : {"","--fail-control","--ignore-term-control"}) {
        SCOPED_TRACE(mode);
        const bool fail_control=mode=="--fail-control";
        const bool process_error=!mode.empty();
        DriverLogs logs;
        const auto full21=catalog(21);
        auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
        app::NativeRunManager manager(catalog(18),full21,store,
            {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
             .port_start=0,.port_end=0,.maximum_active_runs=1,
             .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
             .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem",
             .driver_executable=PICOQUIC_FAMILY_DRIVER_PATH,
             .driver_arguments=mode.empty() ? std::vector<std::string>{} : std::vector<std::string>{mode},
             .driver_log_root=logs.path});
        const std::vector<std::string> ids={"d21-duplicate-request-goaway","d21-goaway-on-distinct-request-streams"};
        const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
            app::RunMode::Driven,ids,2s,app::TrackFixture{{"n"},"t"}});
        ASSERT_EQ(started.status,app::RunStartStatus::Started);
        const auto deadline=std::chrono::steady_clock::now()+7s;
        while (store->load(started.id).state!=storage::RunState::Finalized && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(1ms);
        const auto run=store->load(started.id);
        ASSERT_EQ(run.state,storage::RunState::Finalized);
        EXPECT_EQ(run.config.scenario_ids,ids);
        EXPECT_EQ(run.outcomes.size(),full21->requirements.size());
        const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[](const auto& value) {
            return value.requirement_id=="D21-9-2-MUST-328";
        });
        ASSERT_NE(row,run.outcomes.end());
        EXPECT_EQ(row->state,process_error ? requirements::OutcomeState::NotRun : requirements::OutcomeState::Pass);
        if (!process_error && row->state!=requirements::OutcomeState::Pass) {
            for (const auto& event : run.events) std::cerr << event.kind << " " << event.detail << '\n';
        }
        if (process_error) { EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Error); }
        std::set<std::string> connections;
        std::set<std::string> driven_ids;
        std::set<std::string> stdout_paths;
        for (const auto& event : run.events) {
            if (event.kind=="transport_established") {
                ASSERT_TRUE(event.connection_id);
                connections.insert(*event.connection_id);
            }
            if (event.kind=="publisher_process") {
                ASSERT_TRUE(event.scenario_id);
                driven_ids.insert(*event.scenario_id);
                const auto result=nlohmann::json::parse(event.detail);
                if (mode=="--ignore-term-control" && event.scenario_id==ids[1]) {
                    EXPECT_EQ(result.at("status"),"stopped");
                    EXPECT_EQ(result.at("term_signal"),SIGKILL);
                }
                const auto path=result.at("stdout_log").at("path").get<std::string>();
                ASSERT_FALSE(path.empty());
                stdout_paths.insert(path);
                EXPECT_GT(result.at("stdout_log").at("bytes").get<unsigned>(),0u);
                EXPECT_EQ(result.at("stdout_log").at("sha256").get<std::string>().size(),64u);
                const auto request_path=std::filesystem::path(path).parent_path()/"request.json";
                std::ifstream input(request_path);
                const auto request=nlohmann::json::parse(input);
                EXPECT_EQ(request.at("run_id"),started.id);
                EXPECT_EQ(request.at("scenario_id"),*event.scenario_id);
                EXPECT_EQ(request.at("endpoint"),"moqt://127.0.0.1:"+std::to_string(started.endpoint.port)+"/moq");
                EXPECT_EQ(request.at("namespace_hex"),nlohmann::json::array({"6e"}));
                EXPECT_EQ(request.at("track_name_hex"),"74");
            }
        }
        EXPECT_EQ(connections.size(),fail_control ? 1u : 2u);
        EXPECT_EQ(driven_ids,std::set<std::string>(ids.begin(),ids.end()));
        EXPECT_EQ(stdout_paths.size(),2u);
        // Waiting for finalization does not replace joining the worker and
        // releasing its process handles and bound listener.
        EXPECT_TRUE(manager.stop(started.id));
    }
}

TEST(RawFamilyDriver, CancellationRetiresSecondRealPeerAndReleasesEndpoint) {
    DriverLogs logs;
    const auto full21=catalog(21);
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    app::NativeRunManager manager(catalog(18),full21,store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
         .port_start=0,.port_end=0,.maximum_active_runs=1,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem",
         .driver_executable=PICOQUIC_FAMILY_DRIVER_PATH,.driver_arguments={"--stall-control"},
         .driver_log_root=logs.path});
    const std::vector<std::string> ids={"d21-duplicate-request-goaway","d21-goaway-on-distinct-request-streams"};
    const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
        app::RunMode::Driven,ids,5s,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto second_connected=[&] {
        const auto run=store->load(started.id);
        return std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
            return event.scenario_id==ids[1] && event.kind=="raw_probe_transport_event" &&
                event.detail.find("bytes=af000000")!=std::string::npos;
        });
    };
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while (!second_connected() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(second_connected());
    ASSERT_EQ(store->load(started.id).state,storage::RunState::Active);
    const auto stop_started=std::chrono::steady_clock::now();
    ASSERT_TRUE(manager.stop(started.id));
    EXPECT_LT(std::chrono::steady_clock::now()-stop_started,1s);
    const auto run=store->load(started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    ASSERT_TRUE(run.score);
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Error);
    ASSERT_EQ(run.outcomes.size(),full21->requirements.size());
    const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[](const auto& outcome) {
        return outcome.requirement_id=="D21-9-2-MUST-328";
    });
    ASSERT_NE(row,run.outcomes.end());
    EXPECT_EQ(row->state,requirements::OutcomeState::NotRun);
    EXPECT_FALSE(std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
        return event.scenario_id==ids[1] && (event.kind=="context_complete" || event.kind=="peer_close");
    }));
    EXPECT_EQ(std::count_if(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="publisher_process";
    }),2);
    const auto retired=std::find_if(run.events.begin(),run.events.end(),[&](const auto& event) {
        return event.scenario_id==ids[1] && event.kind=="publisher_process";
    });
    ASSERT_NE(retired,run.events.end());
    const auto process=nlohmann::json::parse(retired->detail);
    EXPECT_EQ(process.at("status"),"stopped");
    EXPECT_EQ(process.at("term_signal"),SIGTERM);
    transport::NativeQuicListenerConfig listener;
    listener.bind_port=started.endpoint.port;
    listener.certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem";
    listener.private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem";
    for (const char byte : std::string("moqt-21")) listener.expected_alpn.push_back(static_cast<std::byte>(byte));
    const auto rebound=transport::NativeQuicListener::create(listener);
    ASSERT_NE(rebound.listener,nullptr);
    EXPECT_EQ(rebound.listener->bound_endpoint().port,started.endpoint.port);
    EXPECT_FALSE(manager.stop(started.id));
}

TEST(RawFamilyDriver, SecondSpawnFailurePreservesFirstProofWithoutFullFamilyPass) {
    DriverLogs logs;
    const auto copied_peer=logs.path/"peer";
    std::filesystem::copy_file(PICOQUIC_FAMILY_DRIVER_PATH,copied_peer);
    const auto full21=catalog(21);
    auto database=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto store=std::make_shared<RemoveDriverAfterFirstContext>(database,copied_peer);
    app::NativeRunManager manager(catalog(18),full21,store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
         .port_start=0,.port_end=0,.maximum_active_runs=1,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem",
         .driver_executable=copied_peer,.driver_log_root=logs.path/"logs"});
    const std::vector<std::string> ids={"d21-duplicate-request-goaway","d21-goaway-on-distinct-request-streams"};
    const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
        app::RunMode::Driven,ids,2s,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto deadline=std::chrono::steady_clock::now()+4s;
    while (store->load(started.id).state!=storage::RunState::Finalized && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(1ms);
    const auto run=store->load(started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    ASSERT_TRUE(store->removed);
    ASSERT_TRUE(run.score);
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Error);
    EXPECT_EQ(run.outcomes.size(),full21->requirements.size());
    const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[](const auto& outcome) {
        return outcome.requirement_id=="D21-9-2-MUST-328";
    });
    ASSERT_NE(row,run.outcomes.end());
    EXPECT_EQ(row->state,requirements::OutcomeState::NotRun);
    EXPECT_EQ(std::count_if(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="context_complete" || event.kind=="transport_established";
    }),2); // One completed actual session; the second process never starts.
    const auto failure=std::find_if(run.events.begin(),run.events.end(),[&](const auto& event) {
        return event.kind=="publisher_process" && event.scenario_id==ids[1];
    });
    ASSERT_NE(failure,run.events.end());
    const auto process=nlohmann::json::parse(failure->detail);
    EXPECT_EQ(process.at("status"),"error");
    EXPECT_FALSE(process.at("error").get<std::string>().empty());
    EXPECT_TRUE(manager.stop(started.id));
}
}
}
