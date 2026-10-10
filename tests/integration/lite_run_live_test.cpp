// Live moq-lite-06 runs (L1d Task 9): the production NativeRunManager constructed with the moq-lite-06 catalog,
// its real listener on the loopback, and a publisher stand-in (tests/support/lite_run_live.h) that behaves as the
// draft-conforming ConformingLitePublisher over a real connection. The expected row states of a live run are the
// ones the same scenarios give on the simulated clock (the production probe builders through lite_probe_for and
// the same publisher configuration): the live hook must store exactly what evaluate_lite gives its transcripts.
//
// Only short scenarios run live (their allowances are 2-3 s); every one of the 19 is covered on the simulated clock
// below (AllNineteenProbesBuildAndJudgeOnTheSimulatedClock) and in tests/protocol/lite_conformance_test.cpp.
#include "moq/interop/app/lite_run.h"
#include "moq/interop/app/lite_scenarios.h"
#include "moq/interop/requirements/execution_audit.h"
#include "moq/interop/requirements/lite_evaluators.h"
#include "support/lite_run_live.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace moq::interop {
namespace {

using namespace lite_live;
using requirements::OutcomeState;

std::shared_ptr<storage::SqliteRunStore> memory_store() {
    return std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
}

app::NativeRunManager lite_manager(const std::shared_ptr<storage::SqliteRunStore>& store,
                                   app::NativeRunManagerConfig config = manager_config()) {
    return app::NativeRunManager(catalog(18), catalog(21), store, std::move(config), catalog(22), catalog(106));
}

std::map<std::string, OutcomeState> by_row(const std::vector<requirements::Outcome>& outcomes) {
    std::map<std::string, OutcomeState> result;
    for (const auto& outcome : outcomes) result[outcome.requirement_id] = outcome.state;
    return result;
}

// The outcomes the scenarios `ids` give against the conforming publisher (with `tweak`) on the simulated clock.
std::vector<requirements::Outcome> simulated(const app::RunConfig& config,
                                             const std::function<void(ConformingLitePublisherConfig&)>& tweak = {}) {
    std::vector<scenarios::LiteTranscript> transcripts;
    for (const auto& id : config.scenario_ids) {
        auto publisher_settings = publisher_config(config.transport == app::TransportKind::WebTransport
                                                       ? scenarios::LiteBinding::WebTransport
                                                       : scenarios::LiteBinding::NativeQuic);
        if (tweak) tweak(publisher_settings);
        ConformingLitePublisher publisher(std::move(publisher_settings));
        ScriptedLitePeer peer(publisher.reaction());
        scenarios::ManualLiteClock clock;
        transcripts.push_back(test::lite::run_lite_probe(peer, app::lite_probe_for(config, id), clock, 10ms));
    }
    return requirements::evaluate_lite(*catalog(106), transcripts);
}

std::unique_ptr<LiteQuicPublisher> conforming(std::uint16_t port, unsigned) {
    return std::make_unique<LiteQuicPublisher>(port, publisher_config());
}

// Runs `config` against the live conforming publisher (with `tweak`) and returns the stored run.
storage::RunRecord run_live(const app::RunConfig& config,
                            const std::function<void(ConformingLitePublisherConfig&)>& tweak = {}) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    const auto started = manager.start(config);
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status != app::RunStartStatus::Started) return {};
    drive_contexts(store, started.id, started.endpoint.port, static_cast<unsigned>(config.scenario_ids.size()),
                   [&](std::uint16_t port, unsigned) {
                       auto settings = publisher_config();
                       if (tweak) tweak(settings);
                       return std::make_unique<LiteQuicPublisher>(port, std::move(settings));
                   });
    (void)manager.stop(started.id);
    return store->load(started.id);
}

void expect_audit_clean(const storage::RunRecord& run) {
    const auto bindings = requirements::lite_executable_bindings();
    const auto audit = requirements::audit_execution(*catalog(106), bindings, std::span(&run, 1));
    for (const auto& finding : audit.findings)
        EXPECT_NE(finding.code, "missing_evaluator_evidence") << finding.requirement_id;
    for (const auto& finding : audit.findings)
        EXPECT_NE(finding.code, "stored_score_mismatch") << finding.detail;
}

// --- one scenario ----------------------------------------------------------------------------------------------

TEST(LiteRunLive, OneScenarioStoresAFinalizedStagedRunOverEveryRow) {
    const auto config = lite_config({"l06-setup-stream"}, 4000ms);
    const auto run = run_live(config);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(run.config.draft, app::DraftVersion::MoqLite06);
    const auto lite = catalog(106);
    ASSERT_EQ(run.outcomes.size(), lite->requirements.size());
    EXPECT_EQ(run.outcomes.size(), 212u);
    // The scenario's two Testable rows pass; nothing else is judged.
    EXPECT_EQ(state_of(run, "L06-3-1-MUST-014"), OutcomeState::Pass);
    EXPECT_EQ(state_of(run, "L06-7-3-MUST-NOT-111"), OutcomeState::Pass);
    for (const auto& outcome : run.outcomes) {
        if (outcome.requirement_id == "L06-3-1-MUST-014" || outcome.requirement_id == "L06-7-3-MUST-NOT-111") continue;
        EXPECT_TRUE(outcome.state == OutcomeState::NotRun || outcome.state == OutcomeState::NotTestable ||
                    outcome.state == OutcomeState::NotApplicable)
            << outcome.requirement_id;
    }
    EXPECT_EQ(by_row(run.outcomes), by_row(simulated(config)));
    ASSERT_TRUE(run.score.has_value());
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Incomplete);
    const auto staged = requirements::score_staged(*lite, run.outcomes);
    EXPECT_EQ(run.score->verdict, staged.verdict);
    EXPECT_EQ(run.score->required.earned, staged.required.earned);
    // The context lifecycle and the declared evidence kinds.
    EXPECT_TRUE(any_event(run, "context_ready", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "context_complete", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "transport_established", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "raw_probe_stimulus", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "lite_stream_opened", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "lite_message", "l06-setup-stream"));
    EXPECT_FALSE(any_event(run, "harness_error"));
    for (const auto& event : run.events)
        if (event.kind == "lite_stream_opened" || event.kind == "lite_message") EXPECT_TRUE(event.stream_id.has_value());
    // The session URL the run used (row 120's evidence): native QUIC lite has no request URI.
    const auto ready = std::find_if(run.events.begin(), run.events.end(),
                                    [](const auto& event) { return event.kind == "context_ready"; });
    ASSERT_NE(ready, run.events.end());
    EXPECT_NE(ready->detail.find("endpoint=moql://127.0.0.1:"), std::string::npos) << ready->detail;
    EXPECT_NE(ready->detail.find("binding=native_quic"), std::string::npos) << ready->detail;
    EXPECT_NE(ready->detail.find("session_url_has_path=false"), std::string::npos) << ready->detail;
    expect_audit_clean(run);
}

// --- several scenarios -----------------------------------------------------------------------------------------

const std::vector<std::string> kSeveral{"l06-setup-stream", "l06-setup-duplicate-parameter", "l06-setup-server-role",
                                        "l06-errors-unknown-stream-type"};

TEST(LiteRunLive, SeveralScenariosMatchTheSimulatedConformanceRowByRow) {
    const auto config = lite_config(kSeveral, 5000ms);
    const auto run = run_live(config);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    const auto expected = by_row(simulated(config));
    EXPECT_EQ(by_row(run.outcomes), expected);
    std::size_t passes = 0;
    for (const auto& [row, state] : expected) {
        EXPECT_NE(state, OutcomeState::Fail) << row;
        if (state == OutcomeState::Pass) ++passes;
    }
    // 014, 111, 112, 131, 108, 109 and 027 (judged on the two close probes; its other three scenarios did not run,
    // so 027 stays NotRun).
    EXPECT_GE(passes, 6u);
    EXPECT_EQ(state_of(run, "L06-7-2-MUST-108"), OutcomeState::Pass);
    EXPECT_EQ(state_of(run, "L06-7-3-MUST-112"), OutcomeState::Pass);
    EXPECT_EQ(state_of(run, "L06-4-4-MUST-027"), OutcomeState::NotRun);
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Incomplete);
    EXPECT_EQ(count_events(run, "context_complete"), kSeveral.size());
    EXPECT_FALSE(any_event(run, "harness_error"));
    EXPECT_TRUE(any_event(run, "peer_close", "l06-setup-duplicate-parameter"));
    expect_audit_clean(run);
}

TEST(LiteRunLive, OneDefectFailsExactlyItsRow) {
    const auto tweak = [](ConformingLitePublisherConfig& settings) {
        settings.defect = test::lite::LiteDefect::IgnoreUnknownStreams;
    };
    const auto config = lite_config({"l06-errors-unknown-stream-type"}, 5000ms);
    const auto run = run_live(config, tweak);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(by_row(run.outcomes), by_row(simulated(config, tweak)));
    std::set<std::string> failed;
    for (const auto& outcome : run.outcomes)
        if (outcome.state == OutcomeState::Fail) failed.insert(outcome.requirement_id);
    EXPECT_EQ(failed, std::set<std::string>{"L06-7-2-MUST-108"});
    EXPECT_EQ(state_of(run, "L06-7-2-MUST-NOT-109"), OutcomeState::Pass);
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Fail);
}

// --- failures of the run hook ----------------------------------------------------------------------------------

TEST(LiteRunLive, WrongAlpnFailsTheConnectionCleanly) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    const auto started = manager.start(lite_config({"l06-setup-stream", "l06-setup-server-role"}, 2500ms));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    drive_contexts(store, started.id, started.endpoint.port, 2, [](std::uint16_t port, unsigned) {
        return std::make_unique<LiteQuicPublisher>(port, publisher_config(), "moqt-22");
    });
    const auto run = store->load(started.id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    // The context did run (its probe was built) and ended without a session.
    EXPECT_TRUE(any_event(run, "context_ready", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "context_end", "l06-setup-stream"));
    EXPECT_FALSE(any_event(run, "transport_established"));
    EXPECT_TRUE(any_event(run, "harness_error", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "context_skipped", "l06-setup-server-role"));
    for (const auto& outcome : run.outcomes)
        EXPECT_TRUE(outcome.state != OutcomeState::Pass && outcome.state != OutcomeState::Fail) << outcome.requirement_id;
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Error);
    EXPECT_EQ(run.outcomes.size(), 212u);
}

TEST(LiteRunLive, APublisherThatNeverConnectsTimesOutCleanly) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    const auto started = manager.start(lite_config({"l06-setup-stream", "l06-setup-server-role"}, 2500ms));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    ASSERT_TRUE(wait_for([&] { return finalized(store, started.id); }, 10s));
    const auto run = store->load(started.id);
    EXPECT_TRUE(any_event(run, "harness_error", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "context_end", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "context_skipped", "l06-setup-server-role"));
    EXPECT_FALSE(any_event(run, "context_ready", "l06-setup-server-role"));
    for (const auto& outcome : run.outcomes)
        EXPECT_TRUE(outcome.state != OutcomeState::Pass && outcome.state != OutcomeState::Fail) << outcome.requirement_id;
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Error);
}

TEST(LiteRunLive, ADeadlineTooShortForTheBuilderIsAStoredHarnessError) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    // The unknown stream type probe's allowance is 3 s: a 2.5 s timeout cannot hold it (the setup probe's 2 s fits).
    const auto started = manager.start(lite_config({"l06-errors-unknown-stream-type", "l06-setup-stream"}, 2500ms));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    drive_contexts(store, started.id, started.endpoint.port, 2, conforming);
    const auto run = store->load(started.id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_TRUE(any_event(run, "harness_error", "l06-errors-unknown-stream-type"));
    EXPECT_TRUE(any_event(run, "context_skipped", "l06-errors-unknown-stream-type"));
    EXPECT_EQ(state_of(run, "L06-7-2-MUST-108"), OutcomeState::NotRun);
    EXPECT_EQ(state_of(run, "L06-7-2-MUST-NOT-109"), OutcomeState::NotRun);
    // The next context still ran.
    EXPECT_TRUE(any_event(run, "context_complete", "l06-setup-stream"));
    EXPECT_EQ(state_of(run, "L06-3-1-MUST-014"), OutcomeState::Pass);
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Error);
}

TEST(LiteRunLive, StartRefusesWhatItCannotRun) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    EXPECT_TRUE(manager.supports(app::DraftVersion::MoqLite06));
    EXPECT_EQ(manager.start(lite_config({"l06-no-such-scenario"}, 4000ms)).status, app::RunStartStatus::InvalidConfig);
    EXPECT_EQ(manager.start(lite_config({"l06-setup-stream", "l06-unknown"}, 4000ms)).status,
              app::RunStartStatus::InvalidConfig);
    EXPECT_EQ(manager.start(lite_config({"l06-setup-stream", "l06-setup-stream"}, 4000ms)).status,
              app::RunStartStatus::InvalidConfig);
    EXPECT_EQ(manager.start(lite_config({}, 4000ms)).status, app::RunStartStatus::InvalidConfig);
    std::vector<std::string> many(101, "l06-setup-stream");
    EXPECT_EQ(manager.start(lite_config(many, 4000ms)).status, app::RunStartStatus::InvalidConfig);
    EXPECT_EQ(manager.start(lite_config({"l06-setup-stream"}, 1ms)).status, app::RunStartStatus::InvalidConfig);
    // A requires_track scenario without the track fixture.
    EXPECT_EQ(manager.start(lite_config({"l06-subscribe-latest"}, 12000ms, app::TransportKind::NativeQuic,
                                        std::nullopt)).status,
              app::RunStartStatus::InvalidConfig);
    // Driven needs a driver.
    auto driven = lite_config({"l06-setup-stream"}, 4000ms);
    driven.mode = app::RunMode::Driven;
    EXPECT_EQ(manager.start(driven).status, app::RunStartStatus::Unsupported);
    EXPECT_TRUE(store->list({}).items.empty());
}

TEST(LiteRunLive, ASecondStartWhileOneIsRunningIsRefusedAndStopFinalizes) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    const auto first = manager.start(lite_config({"l06-setup-stream"}, 10000ms));
    ASSERT_EQ(first.status, app::RunStartStatus::Started);
    EXPECT_EQ(manager.start(lite_config({"l06-setup-stream"}, 10000ms)).status, app::RunStartStatus::PortExhausted);
    ASSERT_TRUE(wait_for([&] { return context_started(store->load(first.id), 1); }, 3s));
    EXPECT_TRUE(manager.stop(first.id));
    const auto run = store->load(first.id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Error);
    EXPECT_TRUE(any_event(run, "run_stopped"));
    EXPECT_EQ(run.outcomes.size(), 212u);
    // The slot and the port are free again.
    const auto second = manager.start(lite_config({"l06-setup-stream"}, 300ms));
    EXPECT_EQ(second.status, app::RunStartStatus::Started);
    EXPECT_TRUE(manager.stop(second.id));
}

TEST(LiteRunLive, StopDuringAConnectedContextFinalizesWithoutAVerdict) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    // The lifecycle probe holds its announce stream open for a 6 s window.
    const auto started = manager.start(lite_config({"l06-announce-lifecycle", "l06-setup-stream"}, 8000ms));
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    ASSERT_TRUE(wait_for([&] { return context_started(store->load(started.id), 1); }, 3s));
    LiteQuicPublisher publisher(started.endpoint.port, publisher_config());
    ASSERT_TRUE(publisher.valid());
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < until && publisher.publisher().requests().empty()) {
        (void)publisher.step();
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_FALSE(publisher.publisher().requests().empty()) << "the runner's announce request reached the publisher";
    EXPECT_TRUE(manager.stop(started.id));
    const auto run = store->load(started.id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Error);
    EXPECT_TRUE(any_event(run, "run_stopped"));
    EXPECT_TRUE(any_event(run, "context_end", "l06-announce-lifecycle"));
    EXPECT_TRUE(any_event(run, "context_skipped", "l06-setup-stream"));
    EXPECT_FALSE(any_event(run, "harness_error"));
    EXPECT_EQ(run.outcomes.size(), 212u);
    for (const auto& outcome : run.outcomes)
        EXPECT_TRUE(outcome.state != OutcomeState::Pass && outcome.state != OutcomeState::Fail) << outcome.requirement_id;
    // The port is free for the next run.
    const auto next = manager.start(lite_config({"l06-setup-stream"}, 2500ms));
    EXPECT_EQ(next.status, app::RunStartStatus::Started);
    EXPECT_TRUE(manager.stop(next.id));
}

TEST(LiteRunLive, TheListenerAndPortAreReleasedAfterTheRun) {
    const auto port = free_udp_port();
    ASSERT_NE(port, 0);
    auto store = memory_store();
    auto manager = lite_manager(store, manager_config(port, port));
    for (int round = 0; round < 2; ++round) {
        const auto started = manager.start(lite_config({"l06-setup-stream"}, 4000ms));
        ASSERT_EQ(started.status, app::RunStartStatus::Started) << round;
        EXPECT_EQ(started.endpoint.port, port);
        drive_contexts(store, started.id, started.endpoint.port, 1, conforming);
        const auto run = store->load(started.id);
        EXPECT_EQ(state_of(run, "L06-3-1-MUST-014"), OutcomeState::Pass) << round;
        EXPECT_TRUE(wait_for([&] {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            ::close(fd);
            return bound;
        }, 3s)) << "the run's port is released (round " << round << ")";
    }
}

TEST(LiteRunLive, DrivenRunWhosePublisherExitsEarlyStoresAHarnessError) {
    auto store = memory_store();
    auto settings = manager_config();
    settings.driver_executable = kRoot / "tests/support/fake_driver.sh";
    settings.driver_arguments = {"early-exit"};
    const auto logs = std::filesystem::temp_directory_path() / ("lite-run-driver-" + std::to_string(::getpid()));
    settings.driver_log_root = logs;
    auto manager = lite_manager(store, settings);
    auto config = lite_config({"l06-setup-stream", "l06-setup-server-role"}, 3000ms);
    config.mode = app::RunMode::Driven;
    const auto started = manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    ASSERT_TRUE(wait_for([&] { return finalized(store, started.id); }, 15s));
    const auto run = store->load(started.id);
    EXPECT_TRUE(any_event(run, "publisher_process", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "harness_error", "l06-setup-stream"));
    EXPECT_TRUE(any_event(run, "context_skipped", "l06-setup-server-role"));
    EXPECT_EQ(run.score->verdict, requirements::RunVerdict::Error);
    std::filesystem::remove_all(logs);
}

// --- other drafts ----------------------------------------------------------------------------------------------

TEST(LiteRunLive, OtherDraftStartsAreUnchangedByTheLiteCatalog) {
    auto store = memory_store();
    auto with_lite = lite_manager(store);
    app::NativeRunManager without_lite(catalog(18), catalog(21), store, manager_config(), catalog(22));
    EXPECT_FALSE(without_lite.supports(app::DraftVersion::MoqLite06));
    for (const auto draft : {app::DraftVersion::Draft18, app::DraftVersion::Draft21, app::DraftVersion::Draft22}) {
        EXPECT_EQ(with_lite.supports(draft), without_lite.supports(draft));
        for (const auto& ids : std::vector<std::vector<std::string>>{{}, {"no-such-scenario"}, {"l06-setup-stream"}}) {
            const app::RunConfig config{draft, app::TransportKind::NativeQuic, app::RunMode::Observed, ids, 1000ms,
                                        std::nullopt, {}};
            EXPECT_EQ(with_lite.start(config).status, without_lite.start(config).status);
        }
    }
    // A runnable draft 18 run still starts (and stops) as before.
    const app::RunConfig setup{app::DraftVersion::Draft18, app::TransportKind::NativeQuic, app::RunMode::Observed,
                               {"subscribe-to-publisher-track"}, 300ms, app::TrackFixture{{"n"}, "t"}, {}};
    const auto started = with_lite.start(setup);
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status == app::RunStartStatus::Started) {
        EXPECT_TRUE(with_lite.stop(started.id));
        EXPECT_EQ(store->load(started.id).config.draft, app::DraftVersion::Draft18);
    }
}

// --- WebTransport ----------------------------------------------------------------------------------------------

TEST(LiteRunLive, WebTransportRunJudgesTheSetupStream) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    const auto config = lite_config({"l06-setup-stream"}, 4000ms, app::TransportKind::WebTransport);
    const auto started = manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    EXPECT_EQ(started.protocol, "moq-lite-06");
    EXPECT_EQ(started.path, "/moq");
    const auto setup = test::lite::setup_stream(wire::moqlite06::SetupMessage{publisher_config().setup_parameters});
    drive_contexts(store, started.id, started.endpoint.port, 1, [&](std::uint16_t port, unsigned) {
        return std::make_unique<LiteWebTransportPublisher>(port, setup);
    });
    const auto run = store->load(started.id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_EQ(state_of(run, "L06-3-1-MUST-014"), OutcomeState::Pass);
    EXPECT_EQ(state_of(run, "L06-7-3-MUST-NOT-111"), OutcomeState::Pass);
    EXPECT_EQ(by_row(run.outcomes), by_row(simulated(config)));
    const auto ready = std::find_if(run.events.begin(), run.events.end(),
                                    [](const auto& event) { return event.kind == "context_ready"; });
    ASSERT_NE(ready, run.events.end());
    EXPECT_NE(ready->detail.find("endpoint=https://127.0.0.1:"), std::string::npos) << ready->detail;
    EXPECT_NE(ready->detail.find("binding=webtransport session_url_has_path=true session_url_path=/moq"),
              std::string::npos)
        << ready->detail;
    expect_audit_clean(run);
}

// --- per-context evaluation and the evidence cap (L1e Task 1) ------------------------------------------------------

// run_lite itself over in-process scripted publishers (ScriptedLitePeer as the listener): the first context's
// publisher also pushes 20 MiB of Group streams. The run hook judges each context as it ends and keeps only the
// verdicts (a bounded size reported through on_context_judged), the flood costs no verdict (before L1e it set
// event_limit_reached and 014/111 were NotRun), and the stored outcomes are what evaluate_lite gives the same
// scenarios on the simulated clock.
TEST(LiteRunLive, TheRunHookJudgesEachContextAndKeepsOnlyItsVerdicts) {
    auto store = memory_store();
    const auto lite = catalog(106);
    const auto settings = manager_config();
    const auto config = lite_config({"l06-setup-stream", "l06-setup-server-role"}, 4000ms);
    const auto id = store->create_run(config);
    std::atomic<bool> stop{false};
    std::vector<std::unique_ptr<ConformingLitePublisher>> publishers;
    std::size_t flood_bytes = 0;
    const auto make = [&]() -> std::unique_ptr<transport::SessionTransport> {
        auto publisher_settings = publisher_config();
        if (publishers.empty()) {
            publisher_settings.hooks.on_poll = [&flood_bytes, done = false](ConformingLitePublisher&,
                                                                            ScriptedLitePeer& peer) mutable {
                if (done) return;
                done = true;
                for (std::uint64_t group = 0; group < 20; ++group) {
                    Bytes bytes = test::lite::join({test::lite::stream_type(0x0),
                                                    test::lite::group_header({1, group, 0})});
                    for (int f = 0; f < 2; ++f) {
                        wire::moqlite06::Frame value;
                        value.payload = Bytes(512 * 1024, std::byte{0x2e});
                        const auto encoded = test::lite::frame(value);
                        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
                    }
                    flood_bytes += bytes.size();
                    peer.data(peer.open_peer_uni(), std::move(bytes), true);
                }
            };
        }
        publishers.push_back(std::make_unique<ConformingLitePublisher>(std::move(publisher_settings)));
        return std::make_unique<ScriptedLitePeer>(publishers.back()->reaction());
    };
    std::vector<std::pair<std::size_t, std::size_t>> judged;
    app::LiteRunEnvironment environment{id, "127.0.0.1:4443", *store, *lite, settings, stop,
                                        [&]() { return app::LiteRunListener{make(), {}}; },
                                        [&](std::size_t contexts, std::size_t bytes) {
                                            judged.emplace_back(contexts, bytes);
                                        }};
    app::run_lite(environment, make(), config);
    const auto run = store->load(id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    ASSERT_GE(flood_bytes, std::size_t{20} << 20);
    EXPECT_FALSE(any_event(run, "context_event_limit"));
    EXPECT_EQ(count_events(run, "context_complete"), 2u);
    EXPECT_EQ(state_of(run, "L06-3-1-MUST-014"), OutcomeState::Pass);
    EXPECT_EQ(state_of(run, "L06-7-3-MUST-NOT-111"), OutcomeState::Pass);
    EXPECT_EQ(state_of(run, "L06-7-3-3-MUST-131"), OutcomeState::Pass);
    EXPECT_EQ(by_row(run.outcomes), by_row(simulated(config)));
    // One report per context; what the run holds stays a few kilobytes whatever the context carried.
    ASSERT_EQ(judged.size(), 2u);
    EXPECT_EQ(judged[0].first, 1u);
    EXPECT_EQ(judged[1].first, 2u);
    for (const auto& [contexts, bytes] : judged) EXPECT_LT(bytes, std::size_t{16} << 10) << contexts;
    // The context end carries the dropped payload count.
    const auto end = std::find_if(run.events.begin(), run.events.end(), [](const auto& event) {
        return event.kind == "context_complete" && event.scenario_id == "l06-setup-stream";
    });
    ASSERT_NE(end, run.events.end());
    EXPECT_NE(end->detail.find("group_payload_bytes_dropped="), std::string::npos) << end->detail;
    EXPECT_EQ(end->detail.find("group_payload_bytes_dropped=0 "), std::string::npos) << end->detail;
}

// A publisher that FINs its announce answer right after ANNOUNCE_OK: over the real native QUIC connection the runner
// FINs its request side (draft 4.3) as an engine action, with no harness error and the row states the simulated
// clock gives the same publisher.
TEST(LiteRunLive, ThePublisherEndingItsAnnounceAnswerIsAnsweredWithTheRunnersFin) {
    const auto tweak = [](ConformingLitePublisherConfig& settings) {
        settings.hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                       const test::lite::LiteRunnerRequest& request) {
            const auto* announce = std::get_if<wire::moqlite06::AnnounceRequest>(&request.message);
            if (!announce) return false;
            publisher.answer_announce(peer, request.stream, *announce);
            publisher.fin_answer(peer, request.stream);
            return true;
        };
    };
    const auto config = lite_config({"l06-announce-prefix"}, 5000ms);
    const auto run = run_live(config, tweak);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_FALSE(any_event(run, "harness_error"));
    const auto end = std::find_if(run.events.begin(), run.events.end(), [](const auto& event) {
        return event.kind == "context_complete" && event.scenario_id == "l06-announce-prefix";
    });
    ASSERT_NE(end, run.events.end());
    EXPECT_NE(end->detail.find("engine_actions=fin_send_after_peer_end:stream=1:"), std::string::npos)
        << end->detail;
    EXPECT_NE(end->detail.find(":status=0"), std::string::npos) << end->detail;
    EXPECT_EQ(by_row(run.outcomes), by_row(simulated(config, tweak)));
    EXPECT_EQ(state_of(run, "L06-7-4-MUST-139"), OutcomeState::Pass);
}

// A WebTransport publisher whose SETUP carries Path (draft 7.3.2: MUST NOT on this binding): the runner closes the
// session with PROTOCOL_VIOLATION over the real transport as soon as it decoded that SETUP. Row 014 needs the whole
// allowance, so it is NotRun; row 111 judges the same SETUP's Parameter IDs (Pass).
TEST(LiteRunLive, AWebTransportPublisherSendingPathIsClosedWithProtocolViolation) {
    auto store = memory_store();
    auto manager = lite_manager(store);
    const auto config = lite_config({"l06-setup-stream"}, 4000ms, app::TransportKind::WebTransport);
    const auto started = manager.start(config);
    ASSERT_EQ(started.status, app::RunStartStatus::Started);
    auto parameters = publisher_config().setup_parameters;
    parameters.insert(parameters.begin(), {wire::moqlite06::kParamPath, test::lite::bytes_of("/moq")});
    const auto setup = test::lite::setup_stream(wire::moqlite06::SetupMessage{parameters});
    drive_contexts(store, started.id, started.endpoint.port, 1, [&](std::uint16_t port, unsigned) {
        return std::make_unique<LiteWebTransportPublisher>(port, setup);
    });
    const auto run = store->load(started.id);
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    EXPECT_FALSE(any_event(run, "harness_error"));
    const auto end = std::find_if(run.events.begin(), run.events.end(), [](const auto& event) {
        return event.kind == "context_complete" && event.scenario_id == "l06-setup-stream";
    });
    ASSERT_NE(end, run.events.end());
    EXPECT_NE(end->detail.find("runner_closed=true runner_closed_for_path=true"), std::string::npos) << end->detail;
    EXPECT_NE(end->detail.find("engine_actions=close_for_webtransport_path:stream=none:"), std::string::npos)
        << end->detail;
    EXPECT_NE(end->detail.find(":code=3:status=0"), std::string::npos) << end->detail;
    EXPECT_EQ(state_of(run, "L06-3-1-MUST-014"), OutcomeState::NotRun);
    EXPECT_EQ(state_of(run, "L06-7-3-MUST-NOT-111"), OutcomeState::Pass);
}

// --- every scenario, simulated ---------------------------------------------------------------------------------

// The production builder dispatch (lite_probe_for) for all 19 scenarios at a live-sized timeout, judged against the
// conforming publisher on the simulated clock: every row the conformance table passes on native QUIC passes here
// (the client-path rows stay NotRun: native QUIC lite gives the publisher no session URL path), and no row fails.
TEST(LiteRunLive, AllNineteenProbesBuildAndJudgeOnTheSimulatedClock) {
    std::vector<std::string> ids;
    for (const auto& traits : app::kLiteExecutableScenarios) ids.emplace_back(traits.id);
    ASSERT_EQ(ids.size(), 19u);
    // Above every builder's stated sum (the largest, abutting: 2 * 3 s + 6 s) with a margin.
    auto config = lite_config(ids, 15000ms);
    for (const auto& id : ids) {
        const auto definition = app::lite_probe_for(config, id);
        EXPECT_EQ(definition.id, id);
        EXPECT_EQ(definition.requires_track, app::lite_executable_scenario(id)->requires_track) << id;
        EXPECT_EQ(definition.binding, scenarios::LiteBinding::NativeQuic);
        EXPECT_FALSE(definition.session_url_has_path);
        EXPECT_EQ(definition.connect_deadline, std::optional{std::chrono::milliseconds{15000}});
        // The announce probes name only the broadcast.
        if (definition.requires_track) EXPECT_EQ(definition.broadcast_path, "demo/live") << id;
        if (definition.requires_track && !id.starts_with("l06-announce-")) EXPECT_EQ(definition.track_name, "video") << id;
    }
    const auto states = by_row(simulated(config));
    for (const auto& [row, state] : states) EXPECT_NE(state, OutcomeState::Fail) << row;
    for (const auto* row : {"L06-3-1-MUST-014", "L06-7-2-MUST-108", "L06-4-4-MUST-027", "L06-7-13-MUST-172",
                            "L06-3-6-MUST-020", "L06-4-4-MUST-030", "L06-7-4-MUST-139"})
        EXPECT_EQ(states.at(row), OutcomeState::Pass) << row;
    for (const auto* row : {"L06-7-3-2-MUST-120", "L06-7-3-2-SHOULD-124", "L06-7-3-2-MUST-NOT-125",
                            "L06-7-7-MUST-NOT-152"})
        EXPECT_EQ(states.at(row), OutcomeState::NotRun) << row;
    // Too short a timeout for a builder is an exception lite_probe_for passes on (run_lite stores it).
    config.timeout = 1000ms;
    EXPECT_THROW((void)app::lite_probe_for(config, "l06-errors-unknown-reset-code"), std::invalid_argument);
    EXPECT_THROW((void)app::lite_probe_for(config, "no-such-scenario"), std::invalid_argument);
}

}  // namespace
}  // namespace moq::interop
