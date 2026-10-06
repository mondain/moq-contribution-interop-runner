// Live draft 22 runs of the own scenarios for D22-3-3-1-MUST-NOT-069: a picoquic publisher stand-in plays
// the publisher half (conforming or deviating) against the production NativeRunManager on moqt-22.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/own_scenario_dispatch_22.h"
#include "moq/interop/app/own_scenarios_22.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/scenarios/draft22_location_range.h"
#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/storage/run_store.h"
#include "support/picoquic_client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace moq::interop {
namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
using Client = transport::test::PicoquicTestClient;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

Bytes alpn_of(std::string_view value) {
    Bytes result;
    for (const char byte : value) result.push_back(static_cast<std::byte>(byte));
    return result;
}

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
    const auto path = root / ("requirements/draft" + std::to_string(draft) + ".json");
    if (draft == 22)
        return std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog::load(source, path, requirements::CatalogLoadMode::AllowIncomplete));
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(source, path));
}

app::NativeRunManager manager_for(const std::shared_ptr<storage::SqliteRunStore>& store) {
    return app::NativeRunManager(catalog(18), catalog(21), store,
        {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
         .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
         .certificate_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "cert.pem",
         .private_key_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "key.pem"},
        catalog(22));
}

template <class Predicate>
bool pump_until(Client& client, Predicate predicate, std::chrono::milliseconds limit = 4s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool context_ready(const std::shared_ptr<storage::SqliteRunStore>& store, const app::RunId& id) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto run = store->load(id);
        if (std::any_of(run.events.begin(), run.events.end(),
                        [](const auto& event) { return event.kind == "context_ready"; }))
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

requirements::OutcomeState state_of(const storage::RunRecord& run, std::string_view id) {
    const auto found = std::find_if(run.outcomes.begin(), run.outcomes.end(),
                                    [&](const auto& outcome) { return outcome.requirement_id == id; });
    EXPECT_NE(found, run.outcomes.end()) << id;
    return found == run.outcomes.end() ? requirements::OutcomeState::NotRun : found->state;
}

// The publisher's half: one answer per request stream (in request order; empty means no answer); then,
// when `follow_ups` is set, waits for each follow-up request on the same streams and sends
// `follow_up_answers`; then each data stream with FIN on the publisher's unidirectional streams 6, 10, ...
struct Script {
    std::vector<Bytes> answers;
    std::vector<Bytes> data;
    std::vector<Bytes> follow_ups{};
    std::vector<Bytes> follow_up_answers{};
};

// Plays `script` once every expected request has arrived; returns what the runner wrote on each request
// stream, and keeps the session open until the run is finalized.
std::vector<Bytes> play(const std::shared_ptr<storage::SqliteRunStore>& store, const app::RunStartResult& started,
                        const std::vector<Bytes>& expected, const Script& script) {
    std::vector<Bytes> written;
    auto client = Client::create({.port = started.endpoint.port, .alpn = alpn_of("moqt-22")});
    EXPECT_NE(client, nullptr);
    if (!client) return written;
    EXPECT_TRUE(pump_until(*client, [&] {
        const auto setup = client->stream(3);
        return setup && setup->data == b({0xaf, 0, 0, 0});
    }));
    EXPECT_TRUE(client->send_stream(2, b({0xaf, 0, 0, 0}), false));
    const auto request_stream = [](std::size_t index) { return static_cast<std::uint64_t>(1 + 4 * index); };
    EXPECT_TRUE(pump_until(*client, [&] {
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto stream = client->stream(request_stream(index));
            if (!stream || stream->data.size() < expected[index].size()) return false;
        }
        return true;
    })) << "every request arrives";
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto stream = client->stream(request_stream(index));
        written.push_back(stream ? stream->data : Bytes{});
    }
    const auto answer = [&](const std::vector<Bytes>& answers) {
        for (std::size_t index = 0; index < answers.size(); ++index) {
            if (!answers[index].empty()) {
                EXPECT_TRUE(client->send_stream(request_stream(index), answers[index], false));
            }
        }
    };
    answer(script.answers);
    if (!script.follow_ups.empty()) {
        EXPECT_TRUE(pump_until(*client, [&] {
            for (std::size_t index = 0; index < script.follow_ups.size(); ++index) {
                const auto stream = client->stream(request_stream(index));
                if (!stream || stream->data.size() < expected[index].size() + script.follow_ups[index].size())
                    return false;
            }
            return true;
        })) << "every follow-up request arrives";
        for (std::size_t index = 0; index < script.follow_ups.size(); ++index) {
            const auto stream = client->stream(request_stream(index));
            written.push_back(stream ? Bytes(stream->data.begin() + static_cast<std::ptrdiff_t>(expected[index].size()),
                                             stream->data.end())
                                     : Bytes{});
        }
        answer(script.follow_up_answers);
    }
    std::uint64_t stream = 6;
    for (const auto& payload : script.data) {
        EXPECT_TRUE(client->send_stream(stream, payload, true));
        stream += 4;
    }
    EXPECT_TRUE(pump_until(*client, [&] { return store->load(started.id).state == storage::RunState::Finalized; },
                           10s));
    return written;
}

// Records the production evaluator's verdict while keeping it in charge (an overlay shadows it by id).
class VerdictRecorder {
public:
    VerdictRecorder(std::string_view evaluator, std::string_view scenario,
                    std::optional<bool> (*production)(const scenarios::RawProbeTranscript&))
        : scope_({evaluator, [this, scenario, production](const scenarios::RawProbeTranscript& transcript) {
              const auto verdict = production(transcript);
              if (transcript.scenario_id == scenario) {
                  const std::lock_guard lock(mutex_);
                  verdicts_.push_back(verdict);
              }
              return verdict;
          }}) {}

    std::vector<std::optional<bool>> verdicts() const {
        const std::lock_guard lock(mutex_);
        return verdicts_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::optional<bool>> verdicts_;
    app::ScopedOwnEvaluator22 scope_;
};

// Subgroup stream: flags 0x30 (Subgroup ID 0, default priority), alias, group, then Objects.
Bytes subgroup(unsigned alias, unsigned group, const std::vector<unsigned>& object_ids) {
    Bytes result = b({0x30, alias, group});
    unsigned previous = 0;
    bool first = true;
    for (const auto id : object_ids) {
        result.push_back(static_cast<std::byte>(first ? id : id - previous - 1));
        result.push_back(std::byte{1});
        result.push_back(std::byte{'x'});
        previous = id;
        first = false;
    }
    return result;
}

// SUBSCRIBE_OK with Track Alias `alias` and LARGEST_OBJECT {7, 9}.
Bytes subscribe_ok(unsigned alias) { return b({4, 0, 5, alias, 1, 9, 7, 9}); }

// The five draft 22 SUBSCRIBEs for track (n)/t: Types 0x01..0x05.
std::vector<Bytes> subscribe_requests() {
    return {b({3, 0, 12, 1, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x01, 1}),
            b({3, 0, 13, 3, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x02, 7, 9}),
            b({3, 0, 14, 5, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x03, 7, 9, 0}),
            b({3, 0, 15, 7, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x04, 7, 9, 0, 9}),
            b({3, 0, 11, 9, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x05})};
}

struct Played {
    storage::RunRecord run;
    std::vector<Bytes> written;
    std::vector<std::optional<bool>> verdicts;
};

// SUBSCRIBEs with FORWARD=0 and no filter, then the REQUEST_UPDATEs that set Types 0x01..0x05.
std::vector<Bytes> update_subscribes() {
    std::vector<Bytes> result;
    for (unsigned id : {1u, 3u, 5u, 7u, 9u}) result.push_back(b({3, 0, 9, id, 1, 1, 'n', 1, 't', 1, 0x10, 0}));
    return result;
}
std::vector<Bytes> update_requests() {
    return {b({2, 0, 7, 11, 2, 0x10, 1, 0x11, 0x01, 1}), b({2, 0, 8, 13, 2, 0x10, 1, 0x11, 0x02, 7, 9}),
            b({2, 0, 9, 15, 2, 0x10, 1, 0x11, 0x03, 7, 9, 0}), b({2, 0, 10, 17, 2, 0x10, 1, 0x11, 0x04, 7, 9, 0, 9}),
            b({2, 0, 6, 19, 2, 0x10, 1, 0x11, 0x05})};
}

// REQUEST_OK with LARGEST_OBJECT {7, 9}.
Bytes request_ok() { return b({7, 0, 4, 1, 9, 7, 9}); }

Played run_one(std::string_view scenario, const std::vector<Bytes>& requests, const Script& script) {
    const VerdictRecorder recorder(scenarios::kDraft22SubscriptionRangeEvaluator, scenario,
                                   scenarios::evaluate_draft22_subscription_location_range);
    auto store = std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    auto manager = manager_for(store);
    const std::string id(scenario);
    const auto started = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
        app::RunMode::Observed, {id}, 1500ms, app::TrackFixture{{"n"}, "t"}});
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status != app::RunStartStatus::Started) return {};
    EXPECT_TRUE(context_ready(store, started.id));
    Played played;
    played.written = play(store, started, requests, script);
    played.run = store->load(started.id);
    played.verdicts = recorder.verdicts();
    EXPECT_TRUE(manager.stop(started.id));
    return played;
}

Played run_subscribe(const Script& script) {
    return run_one(scenarios::kDraft22SubscribeLocationRange, subscribe_requests(), script);
}

Played run_update(Script script) {
    script.follow_ups = update_requests();
    return run_one(scenarios::kDraft22UpdateLocationRange, update_subscribes(), script);
}

TEST(Draft22LocationRangeLive, SubscriptionScenariosAreImplementedAndNeedATrack) {
    for (const auto id : {scenarios::kDraft22SubscribeLocationRange, scenarios::kDraft22UpdateLocationRange}) {
        EXPECT_TRUE(app::executable_scenario(22, id)) << id;
        EXPECT_TRUE(app::raw_probe_scenario(22, id)) << id;
        EXPECT_TRUE(app::scenario_requires_track(22, id)) << id;
        EXPECT_FALSE(app::scenario_requires_fetch(22, id)) << id;
    }
}

TEST(Draft22LocationRangeLive, ConformingPublisherPassesTheSubscribeScenario) {
    const auto played = run_subscribe({{subscribe_ok(1), subscribe_ok(2), subscribe_ok(3), subscribe_ok(4),
                                        subscribe_ok(5)},
                                       {subgroup(1, 7, {0, 9}), subgroup(2, 7, {9, 10}), subgroup(3, 7, {9, 12}),
                                        subgroup(4, 7, {9}), subgroup(5, 7, {10})}});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.written, subscribe_requests()) << "the draft 22 Location Filter Types go on the wire";
    EXPECT_FALSE(std::any_of(played.run.events.begin(), played.run.events.end(),
                             [](const auto& event) { return event.kind == "harness_error"; }));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true}));
    // The row also names the update and FETCH scenarios, which did not run.
    EXPECT_EQ(state_of(played.run, "D22-3-3-1-MUST-NOT-069"), requirements::OutcomeState::NotRun);
}

TEST(Draft22LocationRangeLive, DeviatingPublisherFailsTheSubscribeScenario) {
    // The 0x04 subscription {7, 9}..{7, 9} also receives {7, 10}, past its inclusive end.
    const auto played = run_subscribe({{subscribe_ok(1), subscribe_ok(2), subscribe_ok(3), subscribe_ok(4),
                                        subscribe_ok(5)},
                                       {subgroup(4, 7, {9, 10})}});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, "D22-3-3-1-MUST-NOT-069"), requirements::OutcomeState::Fail);
}

TEST(Draft22LocationRangeLive, NextObjectSubscriptionReceivingTheLargestObjectFails) {
    // The 0x05 subscription starts after the reported Largest Object {7, 9}, which it must not receive.
    const auto played = run_subscribe({{subscribe_ok(1), subscribe_ok(2), subscribe_ok(3), subscribe_ok(4),
                                        subscribe_ok(5)},
                                       {subgroup(4, 7, {9}), subgroup(5, 7, {9})}});
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, "D22-3-3-1-MUST-NOT-069"), requirements::OutcomeState::Fail);
}

std::vector<Bytes> five_subscribe_oks() {
    return {subscribe_ok(1), subscribe_ok(2), subscribe_ok(3), subscribe_ok(4), subscribe_ok(5)};
}
std::vector<Bytes> five_request_oks() { return {request_ok(), request_ok(), request_ok(), request_ok(), request_ok()}; }

TEST(Draft22LocationRangeLive, ConformingPublisherPassesTheUpdateScenario) {
    Script script{five_subscribe_oks(),
                  {subgroup(1, 7, {0, 9}), subgroup(2, 7, {9, 10}), subgroup(3, 7, {9, 12}), subgroup(4, 7, {9}),
                   subgroup(5, 7, {10})}};
    script.follow_up_answers = five_request_oks();
    const auto played = run_update(script);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    auto expected = update_subscribes();
    for (const auto& update : update_requests()) expected.push_back(update);
    EXPECT_EQ(played.written, expected) << "each update carries its draft 22 Location Filter Type";
    EXPECT_FALSE(std::any_of(played.run.events.begin(), played.run.events.end(),
                             [](const auto& event) { return event.kind == "harness_error"; }));
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{true}));
    EXPECT_EQ(state_of(played.run, "D22-3-3-1-MUST-NOT-069"), requirements::OutcomeState::NotRun);
}

TEST(Draft22LocationRangeLive, DeviatingPublisherFailsTheUpdateScenario) {
    // The Relative Start (0x01, StartGroup 1) update starts at {7, 0}; Group 6 is before it.
    Script script{five_subscribe_oks(), {subgroup(4, 7, {9}), subgroup(1, 6, {3})}};
    script.follow_up_answers = five_request_oks();
    const auto played = run_update(script);
    ASSERT_EQ(played.run.state, storage::RunState::Finalized);
    EXPECT_EQ(played.verdicts, (std::vector<std::optional<bool>>{false}));
    EXPECT_EQ(state_of(played.run, "D22-3-3-1-MUST-NOT-069"), requirements::OutcomeState::Fail);
}

}  // namespace
}  // namespace moq::interop
