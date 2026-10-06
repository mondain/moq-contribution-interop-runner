#include "moq/interop/app/lineage_run.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/own_scenario_dispatch_22.h"
#include "moq/interop/app/own_scenarios_22.h"
#include "moq/interop/app/publisher_capabilities.h"
#include "moq/interop/requirements/draft22_evaluators.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/execution_audit.h"
#include "moq/interop/requirements/lineage_translate.h"
#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/storage/run_store.h"
#include "support/picoquic_client.h"

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

TEST(RawFamilyDriver, ChattyPublisherEndsOneContextButNeverTheRun) {
    DriverLogs logs;
    const auto full21=catalog(21);
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    app::NativeRunManager manager(catalog(18),full21,store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
         .port_start=0,.port_end=0,.maximum_active_runs=1,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem",
         .driver_executable=PICOQUIC_FAMILY_FLOOD_PATH,
         .driver_arguments={"flood-first"},
         .driver_log_root=logs.path});
    const std::vector<std::string> ids={"d21-duplicate-request-goaway","d21-goaway-on-distinct-request-streams"};
    const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
        app::RunMode::Driven,ids,2s,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto deadline=std::chrono::steady_clock::now()+12s;
    while (store->load(started.id).state!=storage::RunState::Finalized && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(1ms);
    const auto run=store->load(started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    ASSERT_TRUE(run.score);
    // The first publisher sent more than the evidence bound; the second context still ran.
    const auto count=[&](std::string_view kind,std::string_view scenario) {
        return std::count_if(run.events.begin(),run.events.end(),[&](const auto& event) {
            return event.kind==kind && event.scenario_id==scenario;
        });
    };
    EXPECT_EQ(count("transport_established",ids[0]),1);
    EXPECT_EQ(count("transport_established",ids[1]),1);
    EXPECT_EQ(count("context_event_limit",ids[0]),1);
    EXPECT_EQ(count("context_event_limit",ids[1]),0);
    // What was stored is bounded, and merged chunks are stored whole (more than one 1200 byte
    // packet of bytes in a single row), so the rows equal the transcript that was cut.
    EXPECT_LE(count("raw_probe_transport_event",ids[0]),4096);
    EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
        const auto bytes=event.detail.find(" bytes=");
        return event.kind=="raw_probe_transport_event" && event.scenario_id==ids[0] &&
               bytes!=std::string::npos && event.detail.find(' ',bytes+7)-(bytes+7)>2*4000;
    }));
    EXPECT_EQ(count("harness_error",ids[0]),0);
    EXPECT_EQ(count("context_end",ids[1])+count("context_complete",ids[1]),1);
    // Truncated evidence is never scored and is not an operational error.
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Incomplete);
    const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[](const auto& value) {
        return value.requirement_id=="D21-9-2-MUST-328";
    });
    ASSERT_NE(row,run.outcomes.end());
    EXPECT_EQ(row->state,requirements::OutcomeState::NotRun);
    EXPECT_TRUE(manager.stop(started.id));
}

app::NativeRunManager capability_manager(const std::shared_ptr<storage::SqliteRunStore>& store,
                                         const DriverLogs& logs) {
    return app::NativeRunManager(catalog(18),catalog(21),store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
         .port_start=0,.port_end=0,.maximum_active_runs=1,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem",
         .driver_executable=PICOQUIC_FAMILY_DRIVER_PATH,.driver_log_root=logs.path});
}

TEST(PublisherCapabilityRun, SelectionOfOnlyFetchScenariosIsRejectedBeforeAnythingStarts) {
    DriverLogs logs;
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=capability_manager(store,logs);
    for (const auto& ids : {std::vector<std::string>{"d21-fetch-accepted"},
                            std::vector<std::string>{"d21-fetch-accepted","d21-fetch-rejected"}}) {
        app::RunConfig config{app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
            app::RunMode::Driven,ids,2s,app::TrackFixture{{"n"},"t"},{.fetch=false}};
        const auto started=manager.start(config);
        EXPECT_EQ(started.status,app::RunStartStatus::ScenarioRequiresCapability);
        EXPECT_EQ(started.scenario,"d21-fetch-accepted");
        EXPECT_EQ(started.capability,"fetch");
        EXPECT_EQ(started.endpoint.port,0u) << "no listener may be allocated";
    }
    EXPECT_EQ(store->list({1,0}).total,0u) << "no run is created";
    // The same selection starts when the publisher is capable (the default).
    const auto capable=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
        app::RunMode::Driven,{"d21-fetch-accepted"},2s,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(capable.status,app::RunStartStatus::Started);
    EXPECT_TRUE(manager.stop(capable.id));
}

TEST(PublisherCapabilityRun, MixedSelectionSkipsFetchScenariosAndScoresTheRestWithoutError) {
    DriverLogs logs;
    const auto full21=catalog(21);
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=capability_manager(store,logs);
    // The test peer exits with an error for any scenario other than the two GOAWAY ones,
    // so launching the FETCH scenario's context would surface as a harness error.
    const std::vector<std::string> ids={"d21-duplicate-request-goaway","d21-fetch-accepted",
                                        "d21-goaway-on-distinct-request-streams","d21-fetch-rejected"};
    const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
        app::RunMode::Driven,ids,2s,app::TrackFixture{{"n"},"t"},{.fetch=false}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto deadline=std::chrono::steady_clock::now()+7s;
    while (store->load(started.id).state!=storage::RunState::Finalized && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(1ms);
    const auto run=store->load(started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    EXPECT_EQ(run.config.scenario_ids,ids) << "the selection is recorded as requested";
    EXPECT_FALSE(run.config.publisher_capabilities.fetch);
    ASSERT_TRUE(run.score);
    EXPECT_NE(run.score->verdict,requirements::RunVerdict::Error);
    EXPECT_NE(run.score->verdict,requirements::RunVerdict::Fail);

    ASSERT_FALSE(run.events.empty());
    EXPECT_EQ(run.events.front().kind,"publisher_capabilities");
    EXPECT_EQ(run.events.front().detail,"fetch=false");
    for (const std::string skipped : {"d21-fetch-accepted","d21-fetch-rejected"}) {
        SCOPED_TRACE(skipped);
        std::vector<std::string> kinds;
        for (const auto& event : run.events)
            if (event.scenario_id==skipped) kinds.push_back(event.kind);
        // Only the skip record: no listener context, no publisher process, no transport.
        EXPECT_EQ(kinds,std::vector<std::string>{"context_skipped"});
        const auto event=std::find_if(run.events.begin(),run.events.end(),[&](const auto& value) {
            return value.kind=="context_skipped" && value.scenario_id==skipped;
        });
        ASSERT_NE(event,run.events.end());
        EXPECT_EQ(event->detail,"publisher declared no FETCH support");
    }
    std::set<std::string> driven;
    for (const auto& event : run.events)
        if (event.kind=="publisher_process" && event.scenario_id) driven.insert(*event.scenario_id);
    EXPECT_EQ(driven,(std::set<std::string>{"d21-duplicate-request-goaway","d21-goaway-on-distinct-request-streams"}));
    EXPECT_FALSE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="harness_error";
    }));

    // Rows whose every named scenario needs FETCH are not_applicable; every other row keeps
    // the outcome the evaluators gave it.
    std::size_t not_applicable=0;
    ASSERT_EQ(run.outcomes.size(),full21->requirements.size());
    for (std::size_t index=0; index<run.outcomes.size(); ++index) {
        // Rows the catalog itself classifies as not applicable are not scored rows.
        if (full21->requirements[index].applicability!=requirements::Applicability::Applicable ||
            full21->requirements[index].testability!=requirements::Testability::Testable) continue;
        const bool excluded=app::row_not_applicable_reason(21,full21->requirements[index],run.config.publisher_capabilities).has_value();
        EXPECT_EQ(run.outcomes[index].state==requirements::OutcomeState::NotApplicable,excluded)
            << run.outcomes[index].requirement_id;
        not_applicable+=excluded;
    }
    EXPECT_GE(not_applicable,20u);
    const auto row=std::find_if(run.outcomes.begin(),run.outcomes.end(),[](const auto& value) {
        return value.requirement_id=="D21-9-2-MUST-328";
    });
    ASSERT_NE(row,run.outcomes.end());
    EXPECT_NE(row->state,requirements::OutcomeState::NotApplicable);
    EXPECT_TRUE(manager.stop(started.id));
}

TEST(PublisherCapabilityRun, CapableSelectionRecordsTheDeclarationAndSkipsNothing) {
    DriverLogs logs;
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=capability_manager(store,logs);
    const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
        app::RunMode::Driven,{"d21-duplicate-request-goaway","d21-fetch-accepted"},2s,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    ASSERT_TRUE(manager.stop(started.id));
    const auto run=store->load(started.id);
    EXPECT_TRUE(run.config.publisher_capabilities.fetch);
    ASSERT_FALSE(run.events.empty());
    EXPECT_EQ(run.events.front().kind,"publisher_capabilities");
    EXPECT_EQ(run.events.front().detail,"fetch=true");
    EXPECT_FALSE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="context_skipped";
    }));
}

TEST(NativeRunManagerDraftGate, Draft22IsKnownButNotSupported) {
    DriverLogs logs;
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=capability_manager(store,logs);
    EXPECT_TRUE(manager.supports(app::DraftVersion::Draft18));
    EXPECT_TRUE(manager.supports(app::DraftVersion::Draft21));
    EXPECT_FALSE(manager.supports(app::DraftVersion::Draft22));
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,{"anything"},1000ms,std::nullopt,{}});
    EXPECT_EQ(started.status,app::RunStartStatus::Unsupported);
    EXPECT_EQ(started.endpoint.port,0u) << "no listener may be allocated";
    EXPECT_EQ(store->list({10,0}).total,0u) << "no run is created";
}

// Draft 22 by lineage: shared scenarios run on draft 21's family, the run stays draft 22.

using Client=transport::test::PicoquicTestClient;

std::shared_ptr<const requirements::RequirementCatalog> catalog22() {
    const auto root=std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source=requirements::load_draft_source(22,root/"docs",root/"requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source,root/"requirements/draft22.json",requirements::CatalogLoadMode::AllowIncomplete));
}

std::vector<std::byte> wire_bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

std::vector<std::byte> alpn_of(std::string_view value) {
    std::vector<std::byte> result;
    for (const char byte : value) result.push_back(static_cast<std::byte>(byte));
    return result;
}

template <class Predicate>
bool pump_until(Client& client,Predicate predicate,std::chrono::milliseconds limit=3s) {
    const auto deadline=std::chrono::steady_clock::now()+limit;
    while (std::chrono::steady_clock::now()<deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

storage::RunRecord finalized(const std::shared_ptr<storage::SqliteRunStore>& store,const app::RunId& id) {
    const auto deadline=std::chrono::steady_clock::now()+6s;
    while (store->load(id).state!=storage::RunState::Finalized && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(1ms);
    return store->load(id);
}

app::NativeRunManager lineage_manager(const std::shared_ptr<storage::SqliteRunStore>& store,
                                      std::shared_ptr<const requirements::RequirementCatalog> draft22) {
    return app::NativeRunManager(catalog(18),catalog(21),store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
         .port_start=0,.port_end=0,.maximum_active_runs=2,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem"},
        std::move(draft22));
}

// The publisher half of the two request-GOAWAY contexts (the family driver peer's script, observed
// mode): `duplicate` is d21-duplicate-request-goaway, otherwise d21-goaway-on-distinct-request-streams.
void goaway_publisher(std::uint16_t port,std::string_view alpn,bool duplicate) {
    auto client=Client::create({.port=port,.alpn=alpn_of(alpn)});
    ASSERT_NE(client,nullptr);
    ASSERT_TRUE(pump_until(*client,[&] { const auto setup=client->stream(3); return setup && setup->data==wire_bytes({0xaf,0,0,0}); }));
    ASSERT_TRUE(client->send_stream(2,wire_bytes({0xaf,0,0,0}),false));
    const auto opening=[](unsigned id,unsigned field) { return wire_bytes({0x50,0,5,id,1,1,field,0}); };
    const auto a=opening(1,'a');
    const auto b=opening(3,'b');
    ASSERT_TRUE(pump_until(*client,[&] { const auto stream=client->stream(1); return stream && stream->data==a; }));
    if (!duplicate) {
        ASSERT_TRUE(pump_until(*client,[&] { const auto stream=client->stream(5); return stream && stream->data==b; }));
        ASSERT_TRUE(client->send_stream(5,wire_bytes({7,0,1,0}),false));
    }
    ASSERT_TRUE(client->send_stream(1,wire_bytes({7,0,1,0}),false));
    const auto goaway=wire_bytes({0x10,0,3,0,0xa7,0x10});
    auto expected=a;
    expected.insert(expected.end(),goaway.begin(),goaway.end());
    if (duplicate) expected.insert(expected.end(),goaway.begin(),goaway.end());
    ASSERT_TRUE(pump_until(*client,[&] { const auto stream=client->stream(1); return stream && stream->data==expected && !stream->fin; }));
    if (duplicate) {
        ASSERT_TRUE(client->close(3,{}));
    } else {
        auto expected_b=b;
        expected_b.insert(expected_b.end(),goaway.begin(),goaway.end());
        ASSERT_TRUE(pump_until(*client,[&] { const auto stream=client->stream(5); return stream && stream->data==expected_b && !stream->fin; }));
        ASSERT_TRUE(pump_until(*client,[&] { const auto stream=client->stream(9); return stream && stream->data==opening(5,'c'); }));
        ASSERT_TRUE(client->send_stream(9,wire_bytes({7,0,1,0}),false));
    }
    (void)pump_until(*client,[] { return false; },100ms);
}

// Waits until the run's context `ordinal` is listening.
bool context_ready(const std::shared_ptr<storage::SqliteRunStore>& store,const app::RunId& id,unsigned ordinal) {
    const auto deadline=std::chrono::steady_clock::now()+3s;
    const auto suffix=" ordinal="+std::to_string(ordinal);
    while (std::chrono::steady_clock::now()<deadline) {
        const auto run=store->load(id);
        if (std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
                return event.kind=="context_ready" && event.detail.ends_with(suffix);
            })) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// Plays both GOAWAY contexts of a run, each on a fresh connection offering `alpn`.
void goaway_publishers(const std::shared_ptr<storage::SqliteRunStore>& store,const app::RunStartResult& started,
                       std::string_view alpn) {
    ASSERT_TRUE(context_ready(store,started.id,1));
    goaway_publisher(started.endpoint.port,alpn,true);
    ASSERT_TRUE(context_ready(store,started.id,2));
    goaway_publisher(started.endpoint.port,alpn,false);
}

// The publisher half of the duplicate unknown SETUP option exchange (typed announcement path).
void duplicate_setup_publisher(std::uint16_t port,std::string_view alpn,
                               const std::shared_ptr<storage::SqliteRunStore>& store,const app::RunId& id) {
    auto client=Client::create({.port=port,.alpn=alpn_of(alpn)});
    ASSERT_NE(client,nullptr);
    ASSERT_TRUE(pump_until(*client,[&] {
        const auto setup=client->stream(3);
        return setup && setup->data==wire_bytes({0xaf,0x00,0x00,0x07,0x80,0x9d,0x01,0xaa,0x00,0x01,0xbb});
    })) << "established=" << client->established() << " setup bytes="
        << (client->stream(3) ? client->stream(3)->data.size() : 0u);
    ASSERT_TRUE(client->send_stream(2,wire_bytes({0xaf,0x00,0x00,0x00}),false));
    ASSERT_TRUE(client->send_stream(0,wire_bytes({0x1d,0x00,0x0f,0x00,0x01,0x05,'m','e','d','i','a',
                                                  0x04,'t','e','s','t',0x02,0x00}),false));
    ASSERT_TRUE(pump_until(*client,[&] {
        const auto response=client->stream(0);
        return response && response->data==wire_bytes({0x07,0x00,0x01,0x00}) &&
               store->load(id).state==storage::RunState::Finalized;
    }));
}

// The score denominators the draft 22 catalog gives when every scored row stays in the run
// (rows the publisher's declaration excludes leave them).
requirements::ScoreSummary draft22_denominators(const requirements::RequirementCatalog& draft22,
                                                const app::PublisherCapabilities& capabilities) {
    requirements::ScoreSummary expected{requirements::RunVerdict::Incomplete,{0,0},{0,0},{0,0}};
    for (const auto& row : draft22.requirements) {
        if (row.applicability!=requirements::Applicability::Applicable ||
            row.testability!=requirements::Testability::Testable ||
            app::row_not_applicable_reason(22,row,capabilities)) continue;
        const auto weight=requirements::score_weight(row.strength);
        expected.weighted.possible+=weight;
        expected.coverage.possible+=weight;
        if (row.strength==requirements::Strength::Must || row.strength==requirements::Strength::MustNot)
            expected.required.possible+=weight;
    }
    return expected;
}

void expect_draft22_outcomes(const storage::RunRecord& run,const requirements::RequirementCatalog& draft22) {
    ASSERT_EQ(run.outcomes.size(),draft22.requirements.size());
    std::set<std::string> rows;
    for (const auto& row : draft22.requirements) rows.insert(row.id);
    for (const auto& outcome : run.outcomes) {
        EXPECT_EQ(outcome.requirement_id.rfind("D22-",0),0u) << outcome.requirement_id;
        EXPECT_NE(outcome.requirement_id.rfind("D21-",0),0u) << outcome.requirement_id;
        EXPECT_TRUE(rows.contains(outcome.requirement_id)) << outcome.requirement_id;
    }
}

requirements::OutcomeState state_of(const storage::RunRecord& run,std::string_view id) {
    const auto found=std::find_if(run.outcomes.begin(),run.outcomes.end(),[&](const auto& outcome) {
        return outcome.requirement_id==id;
    });
    EXPECT_NE(found,run.outcomes.end()) << id;
    return found==run.outcomes.end() ? requirements::OutcomeState::NotRun : found->state;
}

// The identity rule at every store site: a draft 22 run stores only draft 22 scenario ids (a shared scenario's
// evidence is stamped back from its draft 21 implementation id), each one a selected scenario; no event detail
// names a draft 21 scenario id and no event carries a draft 21 requirement id. Events that belong to the run
// (publisher_capabilities) carry no scenario id.
void expect_draft22_identity(const storage::RunRecord& run) {
    ASSERT_EQ(run.config.draft,app::DraftVersion::Draft22);
    const std::set<std::string> selected(run.config.scenario_ids.begin(),run.config.scenario_ids.end());
    for (const auto& id : run.config.scenario_ids) EXPECT_TRUE(id.starts_with("d22-")) << id;
    std::size_t stamped=0;
    for (const auto& event : run.events) {
        SCOPED_TRACE(event.kind+" "+event.detail);
        if (event.scenario_id && !event.scenario_id->empty()) {
            ++stamped;
            EXPECT_FALSE(event.scenario_id->starts_with("d21-")) << *event.scenario_id;
            EXPECT_TRUE(event.scenario_id->starts_with("d22-")) << *event.scenario_id;
            EXPECT_TRUE(selected.contains(*event.scenario_id)) << *event.scenario_id;
        }
        EXPECT_EQ(event.detail.find("d21-"),std::string::npos);
        if (event.requirement_id) {
            EXPECT_FALSE(event.requirement_id->starts_with("D21-")) << *event.requirement_id;
        }
    }
    EXPECT_GT(stamped,0u);
}

// Execution audit of one stored draft 22 run against the draft 22 bindings: a passed row whose bound scenario ran
// must find its declared evidence under the bound (draft 22) scenario id.
void expect_no_missing_evaluator_evidence(const storage::RunRecord& run,const requirements::RequirementCatalog& draft22) {
    const auto bindings=requirements::draft22_executable_bindings();
    const auto audit=requirements::audit_execution(draft22,bindings,std::span(&run,1));
    EXPECT_EQ(audit.run_count,1u);
    EXPECT_GT(audit.scored_rows,0u);
    for (const auto& finding : audit.findings)
        EXPECT_NE(finding.code,"missing_evaluator_evidence") << finding.requirement_id << ": " << finding.detail;
}

TEST(NativeRunManagerDraft22Lineage, SupportsDraft22OnlyWithTheDraft22Catalog) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    const auto without=lineage_manager(store,nullptr);
    EXPECT_TRUE(without.supports(app::DraftVersion::Draft21));
    EXPECT_FALSE(without.supports(app::DraftVersion::Draft22));
    const auto with=lineage_manager(store,catalog22());
    EXPECT_TRUE(with.supports(app::DraftVersion::Draft18));
    EXPECT_TRUE(with.supports(app::DraftVersion::Draft21));
    EXPECT_TRUE(with.supports(app::DraftVersion::Draft22));
    // The draft 22 catalog runs on the draft 21 family: without a draft 21 catalog there is no draft 22.
    const app::NativeRunManager no21(catalog(18),nullptr,store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
         .port_start=0,.port_end=0,.maximum_active_runs=1,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem"},catalog22());
    EXPECT_FALSE(no21.supports(app::DraftVersion::Draft22));
    // Only a draft 22 catalog is accepted in the draft 22 position.
    EXPECT_THROW(lineage_manager(store,catalog(21)),std::invalid_argument);
    // And only a complete one, as for drafts 18 and 21: requirements::score() refuses an incomplete catalog, so
    // every run against it would score Error.
    auto incomplete=std::make_shared<requirements::RequirementCatalog>(*catalog22());
    incomplete->complete=false;
    EXPECT_THROW(lineage_manager(store,incomplete),std::invalid_argument);
}

TEST(NativeRunManagerDraft22Lineage, RefusesOwnAndUnknownDraft22Scenarios) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const std::string shared(app::executable_scenarios(22).front());
    // d22-location-filter-unknown-type served as the refused id until Task 10 made it an unscored probe.
    for (const auto& ids : {std::vector<std::string>{"d22-location-filter-no-such-probe"},
                            std::vector<std::string>{"no-such-scenario"},
                            std::vector<std::string>{"d21-duplicate-request-goaway"},
                            std::vector<std::string>{shared,"no-such-scenario"}}) {
        SCOPED_TRACE(::testing::PrintToString(ids));
        const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
            app::RunMode::Observed,ids,1000ms,app::TrackFixture{{"n"},"t"}});
        EXPECT_EQ(started.status,app::RunStartStatus::Unsupported);
        EXPECT_EQ(started.endpoint.port,0u) << "no listener may be allocated";
    }
    EXPECT_EQ(store->list({10,0}).total,0u) << "no run is created";
}

// The selection's ids are judged in selection order, the first defect deciding, the same way for every draft
// (drafts 18 and 21 exactly as before draft 22 ran): an empty id or a repeat of an earlier id is InvalidConfig
// (400 invalid_run_config), an id the draft cannot run (unknown, or a shared scenario whose implementation is
// not executable) is Unsupported (422 unsupported_run_config), and an earlier id's other defect (a typed
// scenario in a multi-scenario selection) wins over a later one. Draft 22 answers exactly as draft 21 does for
// the same selection in its own ids. Characterization: the draft 18 and 21 rows pass against cb23ec3.
TEST(NativeRunManagerIdValidation, EveryDraftJudgesTheSelectionInOrderLikeDrafts18And21) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    // A shared draft 22 scenario whose draft 21 implementation exists but is not executable.
    std::string unimplemented22;
    std::string unimplemented21;
    for (const auto& [d22,d21] : requirements::lineage_data::kSharedScenarios) {
        if (app::executable_scenario(21,d21)) continue;
        unimplemented22=std::string(d22);
        unimplemented21=std::string(d21);
        break;
    }
    ASSERT_FALSE(unimplemented22.empty());
    struct Draft { app::DraftVersion version; std::string valid; std::string typed; std::string unimplemented; };
    const std::vector<Draft> drafts{
        {app::DraftVersion::Draft18,"receive-setup-with-duplicate-unknown-options","subscribe-namespace-at-publisher",
         "d21-duplicate-request-goaway"},
        {app::DraftVersion::Draft21,"d21-duplicate-request-goaway","d21-setup-duplicate-unknown-options",
         unimplemented21},
        {app::DraftVersion::Draft22,"d22-duplicate-request-goaway","d22-setup-duplicate-unknown-options",
         unimplemented22}};
    using Status=app::RunStartStatus;
    for (const auto& [version,valid,typed,unimplemented] : drafts) {
        const auto number=app::draft_number(version);
        ASSERT_TRUE(app::executable_scenario(number,valid) && app::raw_probe_scenario(number,valid)) << valid;
        ASSERT_TRUE(app::executable_scenario(number,typed) && !app::raw_probe_scenario(number,typed)) << typed;
        ASSERT_FALSE(app::executable_scenario(number,unimplemented)) << unimplemented;
        const std::string unknown="does-not-exist";
        const std::vector<std::pair<std::vector<std::string>,Status>> rejected{
            // One defect.
            {{},Status::InvalidConfig},
            {{""},Status::InvalidConfig},
            {{valid,""},Status::InvalidConfig},
            {{valid,valid},Status::InvalidConfig},
            {{unknown},Status::Unsupported},
            {{valid,unknown},Status::Unsupported},
            {{unimplemented},Status::Unsupported},
            {{valid,unimplemented},Status::Unsupported},
            // Several defects: the first in selection order decides.
            {{"",unknown},Status::InvalidConfig},
            {{"",""},Status::InvalidConfig},
            {{unknown,""},Status::Unsupported},
            {{unknown,unknown},Status::Unsupported},
            {{unimplemented,unimplemented},Status::Unsupported},
            {{valid,unknown,""},Status::Unsupported},
            {{valid,"",unknown},Status::InvalidConfig},
            {{valid,valid,unknown},Status::InvalidConfig},
            // A typed scenario in a multi-scenario selection is refused before a later repeat is seen.
            {{typed,typed},Status::Unsupported},
            {{typed,""},Status::Unsupported},
        };
        for (const auto& [ids,status] : rejected) {
            SCOPED_TRACE(std::to_string(number)+" "+::testing::PrintToString(ids));
            const auto started=manager.start({version,app::TransportKind::NativeQuic,
                app::RunMode::Observed,ids,1000ms,app::TrackFixture{{"n"},"t"}});
            EXPECT_EQ(started.status,status);
            EXPECT_EQ(started.endpoint.port,0u) << "no listener may be allocated";
        }
        // The selection-level checks come before any id is judged: a refused id under a 1 ms timeout is
        // InvalidConfig.
        {
            SCOPED_TRACE(std::to_string(number)+" unknown id, 1 ms timeout");
            EXPECT_EQ(manager.start({version,app::TransportKind::NativeQuic,app::RunMode::Observed,{unknown},1ms,
                app::TrackFixture{{"n"},"t"}}).status,Status::InvalidConfig);
        }
        // A scenario the publisher's declaration skips does not stop the ids after it from being judged: a
        // refused id after it is Unsupported, not ScenarioRequiresCapability.
        std::string fetch;
        for (const auto id : app::executable_scenarios(number)) {
            if (!app::scenario_requires_fetch(number,id) || !app::raw_probe_scenario(number,id)) continue;
            fetch=std::string(id);
            break;
        }
        ASSERT_FALSE(fetch.empty());
        {
            SCOPED_TRACE(std::to_string(number)+" "+fetch+" skipped, then an unknown id");
            EXPECT_EQ(manager.start({version,app::TransportKind::NativeQuic,app::RunMode::Observed,{fetch,unknown},
                1000ms,app::TrackFixture{{"n"},"t"},{.fetch=false}}).status,Status::Unsupported);
            EXPECT_EQ(manager.start({version,app::TransportKind::NativeQuic,app::RunMode::Observed,{fetch,""},
                1000ms,app::TrackFixture{{"n"},"t"},{.fetch=false}}).status,Status::InvalidConfig);
        }
    }
    // A draft 22 own scenario (dispatched natively, not by lineage) is judged in order like any other id.
    const std::string own="d22-discover-original-publisher-namespaces";
    for (const auto& [ids,status] : std::vector<std::pair<std::vector<std::string>,app::RunStartStatus>>{
             {{own,""},app::RunStartStatus::InvalidConfig},
             {{"",own},app::RunStartStatus::InvalidConfig},
             {{own,own},app::RunStartStatus::InvalidConfig},
             {{own,"does-not-exist"},app::RunStartStatus::Unsupported},
             {{own,"does-not-exist",""},app::RunStartStatus::Unsupported}}) {
        SCOPED_TRACE("22 "+::testing::PrintToString(ids));
        EXPECT_EQ(manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,app::RunMode::Observed,
            ids,1000ms,app::TrackFixture{{"n"},"t"}}).status,status);
    }
    EXPECT_EQ(store->list({10,0}).total,0u) << "no run is created";
}

TEST(NativeRunManagerDraft22Lineage, ListenerAcceptsOnlyTheDraft22Alpn) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,{"d22-duplicate-request-goaway"},2000ms,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    {
        auto wrong=Client::create({.port=started.endpoint.port,.alpn=alpn_of("moqt-21")});
        ASSERT_NE(wrong,nullptr);
        EXPECT_FALSE(pump_until(*wrong,[&] { return wrong->established(); },300ms)) << "moqt-21 is refused";
    }
    auto right=Client::create({.port=started.endpoint.port,.alpn=alpn_of("moqt-22")});
    ASSERT_NE(right,nullptr);
    EXPECT_TRUE(pump_until(*right,[&] { return right->established(); })) << "moqt-22 is accepted";
    EXPECT_TRUE(manager.stop(started.id));
    // Over WebTransport the advertised application protocol is the draft 22 one.
    const auto webtransport=manager.start({app::DraftVersion::Draft22,app::TransportKind::WebTransport,
        app::RunMode::Observed,{"d22-duplicate-request-goaway"},2000ms,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(webtransport.status,app::RunStartStatus::Started);
    EXPECT_EQ(webtransport.protocol,"moqt-22");
    EXPECT_TRUE(manager.stop(webtransport.id));
}

TEST(NativeRunManagerDraft22Lineage, RunsASharedScenarioOnDraft22AndScoresItAgainstTheDraft22Catalog) {
    const auto draft22=catalog22();
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,draft22);
    // D21-9-2-MUST-328 (D22-9-2-MUST-339) needs both GOAWAY contexts, as in the driven tests above.
    const std::vector<std::string> ids={"d22-duplicate-request-goaway","d22-goaway-on-distinct-request-streams"};
    for (const auto& id : ids) {
        ASSERT_TRUE(app::raw_probe_scenario(22,id));
        ASSERT_TRUE(app::executable_scenario(22,id));
    }
    // The publisher declared no FETCH: capability exclusions work on draft 22 rows too.
    const app::PublisherCapabilities capabilities{.fetch=false};
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,ids,2000ms,app::TrackFixture{{"n"},"t"},capabilities});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    goaway_publishers(store,started,"moqt-22");
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    // The stored run is a draft 22 run with the draft 22 selection.
    EXPECT_EQ(run.config.draft,app::DraftVersion::Draft22);
    EXPECT_EQ(run.config.scenario_ids,ids);
    // The listener negotiated moqt-22. Stored evidence carries the requested draft 22 id, never the
    // scenario layer's draft 21 implementation id.
    EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="transport_established" &&
               event.detail.find(" alpn=6d6f71742d3232 ")!=std::string::npos;
    }));
    EXPECT_FALSE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="harness_error";
    }));
    expect_draft22_identity(run);
    for (const auto& id : ids)
        for (const std::string kind : {"context_ready","transport_established","raw_probe_transport_event",
                                       "raw_probe_stimulus","context_complete"})
            EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
                return event.kind==kind && event.scenario_id==id;
            })) << id << " " << kind;
    expect_no_missing_evaluator_evidence(run,*draft22);
    expect_draft22_outcomes(run,*draft22);
    // The same evidence and evaluator as draft 21's D21-9-2-MUST-328.
    EXPECT_EQ(state_of(run,"D22-9-2-MUST-339"),requirements::OutcomeState::Pass);
    std::size_t excluded=0;
    for (const auto& row : draft22->requirements)
        excluded+=app::row_not_applicable_reason(22,row,capabilities).has_value() &&
                  row.applicability==requirements::Applicability::Applicable &&
                  row.testability==requirements::Testability::Testable;
    // Only rows whose scenarios all need FETCH leave. The own FETCH scenario d22-fetch-bounded-location-range
    // is known to need FETCH (kDraft22OwnFetchScenarios), but the only row naming it (069) also names own
    // SUBSCRIBE scenarios and stays scored.
    EXPECT_GE(excluded,1u);
    EXPECT_EQ(static_cast<std::size_t>(std::count_if(run.outcomes.begin(),run.outcomes.end(),[&](const auto& outcome) {
        const auto row=std::find_if(draft22->requirements.begin(),draft22->requirements.end(),
            [&](const auto& value) { return value.id==outcome.requirement_id; });
        return outcome.state==requirements::OutcomeState::NotApplicable &&
               row->applicability==requirements::Applicability::Applicable &&
               row->testability==requirements::Testability::Testable;
    })),excluded);
    ASSERT_TRUE(run.score);
    const auto expected=draft22_denominators(*draft22,capabilities);
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Incomplete);
    EXPECT_EQ(run.score->required.possible,expected.required.possible);
    EXPECT_EQ(run.score->weighted.possible,expected.weighted.possible);
    EXPECT_EQ(run.score->coverage.possible,expected.coverage.possible);
    EXPECT_GT(run.score->required.earned,0u);
    EXPECT_NE(run.score->coverage.possible,draft22_denominators(*draft22,{}).coverage.possible);
    EXPECT_TRUE(manager.stop(started.id));
}

// The publisher half of one FETCH response context: returns the FETCH the runner wrote on stream 1 and
// answers it with `reply` and FIN.
std::vector<std::byte> fetch_publisher(std::uint16_t port,std::string_view alpn,
                                       const std::vector<std::byte>& expected,
                                       const std::vector<std::byte>& reply) {
    auto client=Client::create({.port=port,.alpn=alpn_of(alpn)});
    EXPECT_NE(client,nullptr);
    if (!client) return {};
    EXPECT_TRUE(pump_until(*client,[&] { const auto setup=client->stream(3); return setup && setup->data==wire_bytes({0xaf,0,0,0}); }));
    EXPECT_TRUE(client->send_stream(2,wire_bytes({0xaf,0,0,0}),false));
    EXPECT_TRUE(pump_until(*client,[&] {
        const auto request=client->stream(1);
        return request && request->fin && request->data.size()>=expected.size();
    }));
    const auto request=client->stream(1);
    if (!request) return {};
    const auto written=request->data;
    EXPECT_TRUE(client->send_stream(1,reply,true));
    (void)pump_until(*client,[] { return false; },100ms);
    return written;
}

TEST(NativeRunManagerDraft22Lineage, SharedFetchProbeWritesTheDraft22FilterOnTheWire) {
    // d21-fetch-accepted / -rejected build LOCATION_FILTER {0,0,u64max}, where the two drafts differ:
    // draft 21 writes `21 0b 00 00 ff..` (Length 11), draft 22 writes `21 03 00 00 ff..` (Type 0x03,
    // AbsoluteBounded). A draft 22 run must put the draft 22 form on the wire, which needs
    // NativeRunManager::start() to resolve the probes under the run's wire draft.
    const auto draft22=catalog22();
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,draft22);
    const std::vector<std::string> ids={"d22-fetch-accepted","d22-fetch-rejected"};
    for (const auto& id : ids) ASSERT_TRUE(app::executable_scenario(22,id)) << id;
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,ids,2000ms,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    // FETCH: Request ID 1, namespace {"n"}, name "t", one parameter: LOCATION_FILTER.
    auto draft22_fetch=wire_bytes({0x16,0,20,1,1,1,'n',1,'t',1,0x21,0x03,0,0});
    auto draft21_fetch=wire_bytes({0x16,0,20,1,1,1,'n',1,'t',1,0x21,0x0b,0,0});
    for (unsigned index=0; index<9; ++index) {
        draft22_fetch.push_back(std::byte{0xff});
        draft21_fetch.push_back(std::byte{0xff});
    }
    const std::vector<std::vector<std::byte>> replies={wire_bytes({0x18,0,4,0,0,1,0}),wire_bytes({5,0,3,1,0,0})};
    for (unsigned context=0; context<2; ++context) {
        SCOPED_TRACE(ids[context]);
        ASSERT_TRUE(context_ready(store,started.id,context+1));
        const auto written=fetch_publisher(started.endpoint.port,"moqt-22",draft22_fetch,replies[context]);
        EXPECT_EQ(written,draft22_fetch) << "the runner must write the draft 22 LOCATION_FILTER form";
        EXPECT_NE(written,draft21_fetch);
    }
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    EXPECT_EQ(run.config.draft,app::DraftVersion::Draft22);
    // The stored stimulus carries the same bytes.
    const auto stimuli=std::count_if(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="raw_probe_stimulus" &&
               event.detail.find(" bytes=1600140101016e01740121030000ffffffffffffffffff ")!=std::string::npos;
    });
    EXPECT_EQ(stimuli,2);
    EXPECT_FALSE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="harness_error";
    }));
    expect_draft22_identity(run);
    expect_no_missing_evaluator_evidence(run,*draft22);
    expect_draft22_outcomes(run,*draft22);
    // The evaluator rebuilds the expected FETCH under the same wire draft and recognises the stimulus:
    // one accepted and one rejected context pass the draft 22 counterpart of D21-3-2-1-MUST-052.
    EXPECT_EQ(state_of(run,"D22-3-2-MUST-057"),requirements::OutcomeState::Pass);
    EXPECT_TRUE(manager.stop(started.id));
}

TEST(NativeRunManagerDraft22Lineage, TheSameRawScenarioUnderDraft21StillScoresDraft21Rows) {
    const auto draft21=catalog(21);
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
        app::RunMode::Observed,{"d21-duplicate-request-goaway","d21-goaway-on-distinct-request-streams"},2000ms,
        app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    goaway_publishers(store,started,"moqt-21");
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    EXPECT_EQ(run.config.draft,app::DraftVersion::Draft21);
    ASSERT_EQ(run.outcomes.size(),draft21->requirements.size());
    for (std::size_t index=0; index<run.outcomes.size(); ++index)
        EXPECT_EQ(run.outcomes[index].requirement_id,draft21->requirements[index].id);
    EXPECT_EQ(state_of(run,"D21-9-2-MUST-328"),requirements::OutcomeState::Pass);
    ASSERT_TRUE(run.score);
    // Scored by the draft 21 catalog exactly as before (capabilities are the default, so nothing is excluded).
    const auto rescored=requirements::score(*draft21,run.outcomes);
    EXPECT_EQ(run.score->verdict,rescored.verdict);
    EXPECT_EQ(run.score->required.possible,rescored.required.possible);
    EXPECT_EQ(run.score->required.earned,rescored.required.earned);
    EXPECT_EQ(run.score->coverage.possible,rescored.coverage.possible);
    EXPECT_EQ(run.score->coverage.earned,rescored.coverage.earned);
    EXPECT_TRUE(manager.stop(started.id));
}

TEST(NativeRunManagerDraft22Lineage, TypedAnnouncementScenarioOnDraft22MatchesItsDraft21Run) {
    const auto draft22=catalog22();
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,draft22);
    const auto run_with=[&](app::DraftVersion draft,const std::string& scenario,std::string_view alpn) {
        const auto started=manager.start({draft,app::TransportKind::NativeQuic,app::RunMode::Observed,
            {scenario},1000ms,app::TrackFixture{{"media"},"test"}});
        EXPECT_EQ(started.status,app::RunStartStatus::Started);
        duplicate_setup_publisher(started.endpoint.port,alpn,store,started.id);
        const auto run=finalized(store,started.id);
        EXPECT_TRUE(manager.stop(started.id));
        return run;
    };
    const auto run21=run_with(app::DraftVersion::Draft21,"d21-setup-duplicate-unknown-options","moqt-21");
    const auto run22=run_with(app::DraftVersion::Draft22,"d22-setup-duplicate-unknown-options","moqt-22");
    ASSERT_EQ(run22.state,storage::RunState::Finalized);
    EXPECT_EQ(run22.config.draft,app::DraftVersion::Draft22);
    EXPECT_EQ(run22.config.scenario_ids,std::vector<std::string>{"d22-setup-duplicate-unknown-options"});
    // The typed path's evidence (stored_draft21_evidence) carries the requested id too.
    expect_draft22_identity(run22);
    EXPECT_TRUE(std::any_of(run22.events.begin(),run22.events.end(),[](const auto& event) {
        return event.kind=="peer_setup_received" && event.scenario_id=="d22-setup-duplicate-unknown-options";
    }));
    expect_no_missing_evaluator_evidence(run22,*draft22);
    expect_draft22_outcomes(run22,*draft22);
    // Every shared row carries exactly the state its draft 21 run gave its counterpart(s).
    const auto translated=requirements::translate_shared_outcomes(run21.outcomes);
    ASSERT_FALSE(translated.empty());
    std::size_t passed=0;
    for (const auto& outcome : translated) {
        EXPECT_EQ(state_of(run22,outcome.requirement_id),outcome.state) << outcome.requirement_id;
        passed+=outcome.state==requirements::OutcomeState::Pass;
    }
    EXPECT_GT(passed,0u);
    EXPECT_EQ(state_of(run22,"D22-9-1-MUST-298"),state_of(run21,"D21-9-1-MUST-287"));
    ASSERT_TRUE(run22.score);
    const auto expected=draft22_denominators(*draft22,{});
    EXPECT_EQ(run22.score->verdict,requirements::RunVerdict::Incomplete);
    EXPECT_EQ(run22.score->required.possible,expected.required.possible);
    EXPECT_EQ(run22.score->coverage.possible,expected.coverage.possible);
    EXPECT_GT(run22.score->coverage.earned,0u);
}

// ---- Own draft 22 scenarios (Task 8 dispatch seam) ----------------------------------------------------
// A test-only stub stands in for an own scenario: it drives the duplicate request-GOAWAY probe under an
// own id, so the fake publisher harness above can play it. No production table is touched.

constexpr std::string_view kStubOwnScenario="d22-request-stream-before-peer-setup";  // row D22-6-3-MAY-159
constexpr std::string_view kStubOwnEvaluator="d22-pre-setup-request-stream-reset";

struct StubObservations {
    std::atomic<unsigned> probe_wire{0};
    std::atomic<app::DraftVersion> probe_family{app::DraftVersion::Draft18};
    std::atomic<unsigned> evaluator_wire{0};
    std::atomic<unsigned> evaluated{0};
};

app::OwnScenario22 stub_own_scenario(StubObservations& seen,std::string_view id=kStubOwnScenario) {
    return {{id,true},[&seen,id](const app::RunConfig& execution) {
        seen.probe_wire=scenarios::current_wire_draft();
        seen.probe_family=execution.draft;
        auto definition=app::NativeRunManager::resolve_probe({},execution,"d21-duplicate-request-goaway");
        if (!definition) throw std::logic_error("stub model probe missing");
        definition->id=std::string(id);
        return std::move(*definition);
    }};
}

app::OwnEvaluator22 stub_own_evaluator(StubObservations& seen) {
    return {kStubOwnEvaluator,[&seen](const scenarios::RawProbeTranscript& transcript) -> std::optional<bool> {
        seen.evaluator_wire=scenarios::current_wire_draft();
        ++seen.evaluated;
        if (transcript.scenario_id!=kStubOwnScenario) return std::nullopt;
        return transcript.complete && !transcript.harness_failed;
    }};
}

// Was "AnUnregisteredOwnScenarioIsRefused" (on an own id with no implementation) until Task 10 implemented the
// last own scenarios. An own id whose implementation supplies no probe is refused the same way.
TEST(NativeRunManagerDraft22Lineage, AnOwnScenarioWithoutAProbeIsRefused) {
    constexpr std::string_view probeless="d22-fill-location-filter-end-group-overflow";
    const app::ScopedOwnScenario22 stub({{probeless,true},{}});
    ASSERT_TRUE(app::own_scenario_22(probeless).has_value());
    ASSERT_FALSE(app::has_own_probe_22(probeless));
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,{std::string(probeless)},1000ms,app::TrackFixture{{"n"},"t"}});
    EXPECT_EQ(started.status,app::RunStartStatus::Unsupported);
    EXPECT_EQ(store->list({10,0}).total,0u);
}

// Every lineage-own draft 22 scenario has a production probe, and every own evaluator a production
// implementation (Task 10 completed the set).
TEST(NativeRunManagerDraft22Lineage, EveryOwnScenarioAndEvaluatorHasAProductionImplementation) {
    ASSERT_TRUE(app::own_scenario_ids_22().size()==app::kOwnScenarioTraits22.size()) << "no overlay may be registered";
    for (const auto id : requirements::lineage_data::kOwnScenarios22) EXPECT_TRUE(app::has_own_probe_22(id)) << id;
    const auto evaluators=app::production_own_evaluator_ids_22();
    for (const auto id : requirements::lineage_data::kOwnEvaluators22)
        EXPECT_NE(std::find(evaluators.begin(),evaluators.end(),id),evaluators.end()) << id;
}

TEST(NativeRunManagerDraft22Lineage, ARegisteredOwnScenarioRunsNativelyAndIsScoredByItsOwnEvaluator) {
    StubObservations seen;
    const app::ScopedOwnScenario22 scenario(stub_own_scenario(seen));
    const app::ScopedOwnEvaluator22 evaluator(stub_own_evaluator(seen));
    const auto draft22=catalog22();
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,draft22);
    // An own context next to a shared one: own ids bypass lineage_run, shared ids still go through it.
    const std::vector<std::string> ids={std::string(kStubOwnScenario),"d22-goaway-on-distinct-request-streams"};
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,ids,2000ms,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    EXPECT_EQ(seen.probe_wire.load(),22u) << "the own probe is built on the draft 22 wire";
    EXPECT_EQ(seen.probe_family.load(),app::DraftVersion::Draft21) << "on the run's execution (family) config";
    goaway_publishers(store,started,"moqt-22");
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    EXPECT_EQ(run.config.draft,app::DraftVersion::Draft22);
    EXPECT_EQ(run.config.scenario_ids,ids);
    EXPECT_FALSE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="harness_error";
    }));
    // Both contexts are recorded under their requested draft 22 ids: the own one natively, the shared one
    // stamped back from its draft 21 implementation id.
    for (const auto& id : ids)
        EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
            return event.kind=="context_complete" && event.scenario_id==id;
        })) << id;
    expect_draft22_identity(run);
    // End to end: the audit finds every passed row's declared evidence under its bound draft 22 scenario.
    expect_no_missing_evaluator_evidence(run,*draft22);
    EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="transport_established" && event.detail.find(" alpn=6d6f71742d3232 ")!=std::string::npos;
    }));
    // Evaluated on the worker under the draft 22 wire, keyed by the draft 22 row, scored by the draft 22 catalog.
    EXPECT_GT(seen.evaluated.load(),0u);
    EXPECT_EQ(seen.evaluator_wire.load(),22u);
    expect_draft22_outcomes(run,*draft22);
    EXPECT_EQ(state_of(run,"D22-6-3-MAY-159"),requirements::OutcomeState::Pass);
    ASSERT_TRUE(run.score);
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Incomplete);
    const auto rescored=requirements::score(*draft22,run.outcomes);
    EXPECT_EQ(run.score->verdict,rescored.verdict);
    EXPECT_EQ(run.score->required.possible,rescored.required.possible);
    EXPECT_EQ(run.score->coverage.possible,rescored.coverage.possible);
    EXPECT_EQ(run.score->coverage.earned,rescored.coverage.earned);
    EXPECT_EQ(run.score->weighted.earned,rescored.weighted.earned);
    EXPECT_EQ(run.score->coverage.possible,draft22_denominators(*draft22,{}).coverage.possible);
    // Without the own evaluator the same row would not be reached: the earned weight includes it.
    auto unscored=run.outcomes;
    for (auto& outcome : unscored)
        if (outcome.requirement_id=="D22-6-3-MAY-159") outcome.state=requirements::OutcomeState::NotRun;
    EXPECT_EQ(run.score->coverage.earned,requirements::score(*draft22,unscored).coverage.earned+1u)
        << "D22-6-3-MAY-159 is a MAY row: weight 1";
    EXPECT_TRUE(manager.stop(started.id));
}

TEST(NativeRunManagerDraft22Lineage, AnOwnFetchScenarioIsSkippedForAPublisherWithoutFetch) {
    StubObservations seen;
    constexpr std::string_view fetch="d22-fetch-bounded-location-range";
    const app::ScopedOwnScenario22 scenario(stub_own_scenario(seen,fetch));
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,{std::string(fetch)},1000ms,app::TrackFixture{{"n"},"t"},{.fetch=false}});
    EXPECT_EQ(started.status,app::RunStartStatus::ScenarioRequiresCapability);
    EXPECT_EQ(started.scenario,fetch);
    EXPECT_EQ(started.capability,"fetch");
    EXPECT_EQ(started.endpoint.port,0u) << "no listener may be allocated";
    EXPECT_EQ(seen.probe_wire.load(),0u) << "the probe of a skipped scenario is never built";
    EXPECT_EQ(store->list({10,0}).total,0u);
    // A capable publisher starts it.
    const auto capable=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,{std::string(fetch)},1000ms,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(capable.status,app::RunStartStatus::Started);
    EXPECT_TRUE(manager.stop(capable.id));
}

// The production own tables cannot drift: with no overlay registered, an own id has a probe exactly when the
// header traits (which the predicates read) list it, and every production evaluator is an own evaluator.
TEST(OwnScenarioTables22, ProductionProbesAndTraitsNameTheSameIds) {
    ASSERT_TRUE(app::own_scenario_ids_22().size()==app::kOwnScenarioTraits22.size()) << "no overlay may be registered";
    for (const auto id : requirements::lineage_data::kOwnScenarios22)
        EXPECT_EQ(app::has_own_probe_22(id),app::own_scenario_22(id).has_value()) << id;
    for (const auto& traits : app::kOwnScenarioTraits22)
        EXPECT_TRUE(std::find(requirements::lineage_data::kOwnScenarios22.begin(),
                              requirements::lineage_data::kOwnScenarios22.end(),traits.id)!=
                    requirements::lineage_data::kOwnScenarios22.end()) << traits.id;
    for (const auto id : app::production_own_evaluator_ids_22())
        EXPECT_TRUE(std::find(requirements::lineage_data::kOwnEvaluators22.begin(),
                              requirements::lineage_data::kOwnEvaluators22.end(),id)!=
                    requirements::lineage_data::kOwnEvaluators22.end()) << id;
    // Unscored probes: a probe and exactly one listed evaluator each, never a catalog row's scenario.
    const auto unscored=app::unscored_probe_evaluators_22();
    EXPECT_EQ(unscored.size(),app::kUnscoredProbeTraits22.size());
    const auto draft22=catalog22();
    for (const auto& traits : app::kUnscoredProbeTraits22) {
        EXPECT_TRUE(app::has_own_probe_22(traits.id)) << traits.id;
        EXPECT_TRUE(app::own_scenario_22(traits.id).has_value()) << traits.id;
        EXPECT_EQ(std::count_if(unscored.begin(),unscored.end(),[&](const auto& entry) { return entry.first==traits.id; }),1)
            << traits.id;
        for (const auto& row : draft22->requirements)
            EXPECT_EQ(std::find(row.scenarios.begin(),row.scenarios.end(),traits.id),row.scenarios.end())
                << row.id << " names " << traits.id;
    }
    for (const auto& [probe,evaluator] : unscored) {
        EXPECT_TRUE(app::unscored_probe_22(probe)) << probe;
        EXPECT_NE(std::find(app::kUnscoredEvaluators22.begin(),app::kUnscoredEvaluators22.end(),evaluator),
                  app::kUnscoredEvaluators22.end()) << evaluator;
        for (const auto& row : draft22->requirements)
            EXPECT_EQ(std::find(row.evaluators.begin(),row.evaluators.end(),evaluator),row.evaluators.end())
                << row.id << " names " << evaluator;
    }
}

app::OwnEvaluator22 passing_own_evaluator(std::string_view id,std::atomic<unsigned>& calls) {
    return {id,[&calls](const scenarios::RawProbeTranscript& transcript) -> std::optional<bool> {
        ++calls;
        return transcript.complete && !transcript.harness_failed;
    }};
}

// Row 069 end to end with fetch=false: both SUBSCRIBE scenarios run (stubs on the duplicate request-GOAWAY
// probe) and pass their evaluator; the FETCH scenario is skipped. The row stays NotRun and scored.
TEST(NativeRunManagerDraft22Lineage, WithoutFetchRow069StaysNotRunOnPassingSubscribeEvidence) {
    StubObservations seen;
    constexpr std::string_view subscribe="d22-subscribe-bounded-location-range";
    constexpr std::string_view fetch="d22-fetch-bounded-location-range";
    constexpr std::string_view update="d22-update-subscription-location-range";
    const app::ScopedOwnScenario22 subscribe_stub(stub_own_scenario(seen,subscribe));
    const app::ScopedOwnScenario22 fetch_stub(stub_own_scenario(seen,fetch));
    const app::ScopedOwnScenario22 update_stub(stub_own_scenario(seen,update));
    std::atomic<unsigned> subscription_calls{0},fetch_calls{0};
    const app::ScopedOwnEvaluator22 subscription(passing_own_evaluator(
        "d22-subscription-objects-within-effective-location-range",subscription_calls));
    const app::ScopedOwnEvaluator22 fetch_evaluator(passing_own_evaluator(
        "d22-fetch-objects-within-requested-location-range",fetch_calls));
    const auto draft22=catalog22();
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,draft22);
    const std::vector<std::string> ids={std::string(subscribe),std::string(fetch),std::string(update)};
    const app::PublisherCapabilities capabilities{.fetch=false};
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,ids,2000ms,app::TrackFixture{{"n"},"t"},capabilities});
    ASSERT_EQ(started.status,app::RunStartStatus::Started) << "a mixed selection skips only the FETCH scenario";
    ASSERT_TRUE(context_ready(store,started.id,1));
    goaway_publisher(started.endpoint.port,"moqt-22",true);
    ASSERT_TRUE(context_ready(store,started.id,2));
    goaway_publisher(started.endpoint.port,"moqt-22",true);
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    EXPECT_EQ(run.config.scenario_ids,ids);
    std::vector<std::string> fetch_kinds;
    for (const auto& event : run.events)
        if (event.scenario_id==fetch) fetch_kinds.push_back(event.kind);
    EXPECT_EQ(fetch_kinds,std::vector<std::string>{"context_skipped"}) << "the own FETCH id is only skipped";
    const auto skip=std::find_if(run.events.begin(),run.events.end(),[&](const auto& event) {
        return event.kind=="context_skipped" && event.scenario_id==fetch;
    });
    ASSERT_NE(skip,run.events.end());
    EXPECT_EQ(skip->detail,"publisher declared no FETCH support");
    for (const auto id : {subscribe,update})
        EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
            return event.kind=="context_complete" && event.scenario_id==id;
        })) << id;
    EXPECT_GT(subscription_calls.load(),0u) << "the SUBSCRIBE evidence was evaluated";
    expect_draft22_identity(run);
    expect_draft22_outcomes(run,*draft22);
    EXPECT_EQ(state_of(run,"D22-3-3-1-MUST-NOT-069"),requirements::OutcomeState::NotRun);
    ASSERT_TRUE(run.score);
    EXPECT_EQ(run.score->required.possible,draft22_denominators(*draft22,capabilities).required.possible)
        << "069 is not excluded by the declaration";
    EXPECT_TRUE(manager.stop(started.id));
}

// ---- Draft 22 identity at every store site and in the driver contract (D4 Task 3) --------------------------

app::NativeRunManager driven_lineage_manager(const std::shared_ptr<storage::SqliteRunStore>& store,
                                             const DriverLogs& logs,std::vector<std::string> arguments={}) {
    return app::NativeRunManager(catalog(18),catalog(21),store,
        {.bind_address="127.0.0.1",.advertised_address="127.0.0.1",
         .port_start=0,.port_end=0,.maximum_active_runs=1,
         .certificate_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"cert.pem",
         .private_key_path=std::filesystem::path(PICOQUIC_TEST_CERT_DIR)/"key.pem",
         .driver_executable=PICOQUIC_FAMILY_DRIVER_PATH,.driver_arguments=std::move(arguments),
         .driver_log_root=logs.path},
        catalog22());
}

nlohmann::json driver_request_file(const std::filesystem::path& directory) {
    std::ifstream input(directory/"request.json");
    EXPECT_TRUE(input.good()) << directory;
    return input.good() ? nlohmann::json::parse(input) : nlohmann::json::object();
}

TEST(NativeRunManagerDraft22Identity, DrivenRawRunHandsTheDriverTheRequestedIdsAndTheWireDraft) {
    DriverLogs logs;
    const auto draft22=catalog22();
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=driven_lineage_manager(store,logs);
    const std::vector<std::string> ids={"d22-duplicate-request-goaway","d22-goaway-on-distinct-request-streams"};
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Driven,ids,2s,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    EXPECT_EQ(run.config.scenario_ids,ids);
    EXPECT_FALSE(std::any_of(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="harness_error";
    }));
    // The test peer only plays a context whose request names the draft 22 id under draft 22 (and connects
    // with moqt-22), so the pass itself shows the driver contract; the request files say it directly.
    EXPECT_EQ(state_of(run,"D22-9-2-MUST-339"),requirements::OutcomeState::Pass);
    std::vector<std::string> driven;
    for (const auto& event : run.events) {
        if (event.kind!="publisher_process") continue;
        ASSERT_TRUE(event.scenario_id);
        driven.push_back(*event.scenario_id);
        const auto result=nlohmann::json::parse(event.detail);
        const auto directory=std::filesystem::path(result.at("stdout_log").at("path").get<std::string>()).parent_path();
        const auto request=driver_request_file(directory);
        EXPECT_EQ(request.at("draft"),22);
        EXPECT_EQ(request.at("scenario_id"),*event.scenario_id);
        EXPECT_EQ(request.at("endpoint"),"moqt://127.0.0.1:"+std::to_string(started.endpoint.port)+"/moq");
        EXPECT_TRUE(directory.filename().string().ends_with("-"+*event.scenario_id)) << directory;
    }
    EXPECT_EQ(driven,ids);
    expect_draft22_identity(run);
    expect_no_missing_evaluator_evidence(run,*draft22);
    EXPECT_TRUE(manager.stop(started.id));
}

TEST(NativeRunManagerDraft22Identity, HarnessErrorsAndAbortsOfARawRunCarryTheRequestedIds) {
    DriverLogs logs;
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    // The peer fails the distinct-streams context before connecting, so the first context ends in a harness
    // error and the second is never run.
    auto manager=driven_lineage_manager(store,logs,{"--fail-control"});
    const std::vector<std::string> ids={"d22-goaway-on-distinct-request-streams","d22-duplicate-request-goaway"};
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Driven,ids,2s,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    ASSERT_TRUE(run.score);
    EXPECT_EQ(run.score->verdict,requirements::RunVerdict::Error);
    const auto errors=std::count_if(run.events.begin(),run.events.end(),[&](const auto& event) {
        return event.kind=="harness_error" && event.scenario_id==ids[0];
    });
    EXPECT_GE(errors,1);
    const auto aborted=std::find_if(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="run_aborted";
    });
    ASSERT_NE(aborted,run.events.end());
    EXPECT_EQ(aborted->scenario_id,ids[0]);
    EXPECT_NE(aborted->detail.find("contexts not run: "+ids[1]+" ordinal=1"),std::string::npos) << aborted->detail;
    expect_draft22_identity(run);
    EXPECT_TRUE(manager.stop(started.id));
}

TEST(NativeRunManagerDraft22Identity, DrivenTypedRunHandsTheDriverTheRequestedIdAndRecordsItsFailure) {
    DriverLogs logs;
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=driven_lineage_manager(store,logs);
    // The test peer plays only the GOAWAY scenarios: it exits for this typed one before connecting, so the run
    // ends in error with the publisher_process and harness_error events of the typed path.
    const std::string id="d22-setup-duplicate-unknown-options";
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Driven,{id},1000ms,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    const auto request=driver_request_file(logs.path/started.id);
    EXPECT_EQ(request.at("draft"),22);
    EXPECT_EQ(request.at("scenario_id"),id);
    for (const std::string kind : {"publisher_process","harness_error"})
        EXPECT_TRUE(std::any_of(run.events.begin(),run.events.end(),[&](const auto& event) {
            return event.kind==kind && event.scenario_id==id;
        })) << kind;
    expect_draft22_identity(run);
    EXPECT_TRUE(manager.stop(started.id));
}

// The unknown-alias request probe records how its unassigned error code is mapped, naming the row it maps: on a
// draft 22 run that is the draft 22 row (D21-8-9-MUST-269 -> D22-8-9-MUST-281), under the draft 22 scenario id.
// No publisher connects; the mapping event is recorded however the context ends.
TEST(NativeRunManagerDraft22Identity, AnErrorMappingEventNamesTheDraft22RowAndScenario) {
    const std::string id="d22-request-unknown-token-alias";
    ASSERT_TRUE(app::executable_scenario(22,id));
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,{id},200ms,app::TrackFixture{{"n"},"t"}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    const auto mapping=std::find_if(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="unresolved_error_mapping" || event.kind=="compatibility_error_mapping";
    });
    ASSERT_NE(mapping,run.events.end());
    EXPECT_EQ(mapping->scenario_id,id);
    EXPECT_EQ(mapping->requirement_id,"D22-8-9-MUST-281");
    expect_draft22_identity(run);
    EXPECT_TRUE(manager.stop(started.id));
}

TEST(NativeRunManagerDraft22Identity, ASkippedSharedScenarioIsRecordedUnderItsRequestedId) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const std::vector<std::string> ids={"d22-duplicate-request-goaway","d22-fetch-accepted"};
    const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
        app::RunMode::Observed,ids,2000ms,app::TrackFixture{{"n"},"t"},{.fetch=false}});
    ASSERT_EQ(started.status,app::RunStartStatus::Started);
    ASSERT_TRUE(context_ready(store,started.id,1));
    goaway_publisher(started.endpoint.port,"moqt-22",true);
    const auto run=finalized(store,started.id);
    ASSERT_EQ(run.state,storage::RunState::Finalized);
    EXPECT_EQ(run.config.scenario_ids,ids);
    const auto skip=std::find_if(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="context_skipped";
    });
    ASSERT_NE(skip,run.events.end());
    EXPECT_EQ(skip->scenario_id,"d22-fetch-accepted");
    // The declaration belongs to the run: no scenario id.
    const auto declaration=std::find_if(run.events.begin(),run.events.end(),[](const auto& event) {
        return event.kind=="publisher_capabilities";
    });
    ASSERT_NE(declaration,run.events.end());
    EXPECT_FALSE(declaration->scenario_id.has_value() && !declaration->scenario_id->empty());
    expect_draft22_identity(run);
    EXPECT_TRUE(manager.stop(started.id));
}

// The API answer for a selection of only skipped shared scenarios names the requested draft 22 scenario and
// its capability, as draft 21 names its own.
TEST(NativeRunManagerDraft22Identity, AllSkippedSharedSelectionNamesTheRequestedScenario) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    for (const auto& [version,id] : {std::pair{app::DraftVersion::Draft21,std::string("d21-fetch-accepted")},
                                     std::pair{app::DraftVersion::Draft22,std::string("d22-fetch-accepted")}}) {
        SCOPED_TRACE(id);
        const auto started=manager.start({version,app::TransportKind::NativeQuic,
            app::RunMode::Observed,{id},1000ms,app::TrackFixture{{"n"},"t"},{.fetch=false}});
        EXPECT_EQ(started.status,app::RunStartStatus::ScenarioRequiresCapability);
        EXPECT_EQ(started.scenario,id);
        EXPECT_EQ(started.capability,"fetch");
        EXPECT_EQ(started.endpoint.port,0u) << "no listener may be allocated";
    }
    EXPECT_EQ(store->list({10,0}).total,0u);
}
// Degenerate track fixtures: what the API's fixture parser can hand the manager (an empty namespace, an empty
// track name), the bounds valid_fixture refuses (33 fields, a field or name over 4096 bytes, an empty field),
// and the largest fixture it accepts. Each name says which.
std::vector<std::pair<std::string,app::TrackFixture>> degenerate_fixtures() {
    return {
        {"empty namespace",app::TrackFixture{{},"t"}},
        {"empty namespace and name",app::TrackFixture{{},""}},
        {"empty name",app::TrackFixture{{"n"},""}},
        {"empty field",app::TrackFixture{{""},"t"}},
        {"33 fields",app::TrackFixture{std::vector<std::string>(33,"n"),"t"}},
        {"field over 4096 bytes",app::TrackFixture{{std::string(4097,'n')},"t"}},
        {"name over 4096 bytes",app::TrackFixture{{"n"},std::string(4097,'t')}},
        {"largest accepted",app::TrackFixture{std::vector<std::string>(32,std::string(120,'n')),std::string(256,'t')}},
    };
}

// Fills both of lineage_manager's run slots, so every selection that passes validation answers PortExhausted:
// start() then builds every probe (where a fixture could crash it) but binds and stores nothing more.
std::vector<app::RunId> fill_run_slots(app::NativeRunManager& manager) {
    std::vector<app::RunId> blockers;
    for (unsigned index = 0; index < 2; ++index) {
        const auto started=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
            app::RunMode::Observed,{"d21-duplicate-request-goaway"},60000ms,app::TrackFixture{{"n"},"t"}});
        EXPECT_EQ(started.status,app::RunStartStatus::Started);
        blockers.push_back(started.id);
    }
    return blockers;
}

// No fixture crashes an own draft 22 probe (or an unscored one): each answers InvalidConfig before anything is
// bound or stored, or passes validation. An own probe needs a fixture fetch_first_object_fixture_valid accepts;
// the namespace discovery probe also needs a namespace field (its prefixes are cut from the first one).
TEST(NativeRunManagerDraft22Fixtures, OwnProbesRefuseFixturesTheyCannotBuildInsteadOfCrashing) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const auto blockers=fill_run_slots(manager);
    std::vector<std::string_view> ids;
    for (const auto& traits : app::kOwnScenarioTraits22) ids.push_back(traits.id);
    for (const auto& traits : app::kUnscoredProbeTraits22) ids.push_back(traits.id);
    ASSERT_EQ(ids.size(),10u);
    for (const auto id : ids) {
        for (const auto& [name,fixture] : degenerate_fixtures()) {
            SCOPED_TRACE(std::string(id)+" / "+name);
            const auto started=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
                app::RunMode::Observed,{std::string(id)},1000ms,fixture});
            const bool out_of_bounds=name=="empty field" || name=="33 fields" || name.find("over 4096")!=std::string::npos;
            const bool discovery_without_prefix=id==std::string_view("d22-discover-original-publisher-namespaces") &&
                fixture.namespace_fields.empty();
            if (out_of_bounds || discovery_without_prefix)
                EXPECT_EQ(started.status,app::RunStartStatus::InvalidConfig);
            else
                EXPECT_EQ(started.status,app::RunStartStatus::PortExhausted) << "the fixture is buildable";
        }
    }
    EXPECT_EQ(store->list({100,0}).total,blockers.size()) << "no run is created";
    for (const auto& id : blockers) EXPECT_TRUE(manager.stop(id));
}

// A shared draft 22 scenario runs its draft 21 implementation, so it answers every degenerate fixture exactly
// as that draft 21 scenario does (and neither crashes).
TEST(NativeRunManagerDraft22Fixtures, SharedScenariosAnswerDegenerateFixturesLikeTheirDraft21Implementations) {
    auto store=std::make_shared<storage::SqliteRunStore>(":memory:",app::BuildInfo{"test","test",{}});
    auto manager=lineage_manager(store,catalog22());
    const auto blockers=fill_run_slots(manager);
    std::size_t compared=0;
    for (const auto id : app::executable_scenarios(22)) {
        const auto implementation=app::implementation_scenario_id(id);
        if (!implementation) continue;
        for (const auto& [name,fixture] : degenerate_fixtures()) {
            SCOPED_TRACE(std::string(id)+" / "+name);
            const auto draft21=manager.start({app::DraftVersion::Draft21,app::TransportKind::NativeQuic,
                app::RunMode::Observed,{std::string(*implementation)},1000ms,fixture});
            const auto draft22=manager.start({app::DraftVersion::Draft22,app::TransportKind::NativeQuic,
                app::RunMode::Observed,{std::string(id)},1000ms,fixture});
            EXPECT_EQ(draft22.status,draft21.status);
            ++compared;
        }
    }
    EXPECT_GT(compared,0u);
    EXPECT_EQ(store->list({100,0}).total,blockers.size()) << "no run is created";
    for (const auto& id : blockers) EXPECT_TRUE(manager.stop(id));
}
}
}
