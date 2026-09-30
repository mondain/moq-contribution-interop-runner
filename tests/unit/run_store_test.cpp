#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <barrier>
#include <filesystem>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace moq::interop::storage {
namespace {

using namespace std::chrono_literals;

class TemporaryDatabase {
public:
    TemporaryDatabase()
        : directory_(std::filesystem::temp_directory_path() /
                     ("moq-interop-store-" + std::to_string(next_++))),
          path_(directory_ / "runs.sqlite3") {
        std::filesystem::create_directories(directory_);
    }

    ~TemporaryDatabase() { std::filesystem::remove_all(directory_); }

    const std::filesystem::path& path() const { return path_; }

private:
    inline static unsigned long long next_ = 0;
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

app::BuildInfo sample_build() {
    return {
        std::string{"0.1.0\0' ; DROP TABLE runs; --", 30},
        "abc,def|123",
        {{"sqlite3", "3.46.0'"},
         {std::string{"quiche\0binary", 13}, std::string{"rev\0with|delimiters", 19}}},
    };
}

app::RunConfig sample_config() {
    return {
        app::DraftVersion::Draft21,
        app::TransportKind::WebTransport,
        app::RunMode::Driven,
        {"session/setup", std::string{"publisher\0object", 16}, "x,y|z"},
        12'345ms,
        app::TrackFixture{{"scope", std::string{"n\0s", 3}},
                          std::string{"track\0name", 10}},
    };
}

EvidenceEvent event(std::int64_t ordinal) {
    EvidenceEvent value;
    value.monotonic_time_ns = 1'000 + ordinal;
    value.wall_time_unix_ns = 2'000 + ordinal;
    value.kind = "frame'received," + std::to_string(ordinal);
    value.detail = R"({"sql":"DELETE FROM evidence_events; --","ordinal":)" +
                   std::to_string(ordinal) + "}";
    if (ordinal == 0) {
        value.kind = std::string{"frame\0received", 14};
        value.detail = std::string{"{\"binary\":\"a\0b\"}", 16};
    }
    value.connection_id = "connection|" + std::to_string(ordinal);
    value.stream_id = "stream," + std::to_string(ordinal);
    value.request_id = "request'" + std::to_string(ordinal);
    value.scenario_id = "scenario/" + std::to_string(ordinal);
    value.requirement_id = "D21-MUST-" + std::to_string(ordinal);
    return value;
}

requirements::ScoreSummary sample_score() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return {
        requirements::RunVerdict::Fail,
        {maximum - 5, maximum},
        {maximum - 4, maximum - 3},
        {maximum - 2, maximum - 1},
    };
}

TEST(RunStoreTest, CreatesVersionTwoSchemaAndEnablesForeignKeys) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());

    EXPECT_EQ(store.schema_version(), 2);
    EXPECT_TRUE(store.foreign_keys_enabled());
}

TEST(RunStoreTest, RecoversOnlyInterruptedRunsAsErrors) {
    TemporaryDatabase database;
    app::RunId interrupted, completed;
    {
        SqliteRunStore store(database.path(), sample_build());
        interrupted = store.create_run(sample_config());
        completed = store.create_run(sample_config());
        store.finalize(completed, sample_score(), {});
    }
    SqliteRunStore reopened(database.path(), sample_build());
    EXPECT_EQ(reopened.recover_interrupted(), 1u);
    EXPECT_EQ(reopened.recover_interrupted(), 0u);
    const auto recovered = reopened.load(interrupted);
    ASSERT_EQ(recovered.state, RunState::Finalized);
    ASSERT_TRUE(recovered.score);
    EXPECT_EQ(recovered.score->verdict, requirements::RunVerdict::Error);
    ASSERT_EQ(recovered.events.size(), 1u);
    EXPECT_EQ(recovered.events.front().kind, "runner_recovery");
    EXPECT_EQ(reopened.load(completed).score->verdict,
              requirements::RunVerdict::Fail);
}

TEST(RunStoreTest, SchemaRejectsFinalizedStateWithoutFinalizationTimestamp) {
    TemporaryDatabase database;
    app::RunId active_id;
    {
        SqliteRunStore store(database.path(), sample_build());
        active_id = store.create_run(sample_config());
    }

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(raw,
                           "INSERT INTO runs("
                           "id,draft,transport,mode,timeout_ms,state,created_at_unix_ns,"
                           "finalized_at_unix_ns) "
                           "VALUES('invalid-finalized',21,0,0,1000,1,1,NULL)",
                           nullptr, nullptr, nullptr),
              SQLITE_CONSTRAINT);

    sqlite3_stmt* update = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  raw, "UPDATE runs SET state=1, finalized_at_unix_ns=NULL WHERE id=?", -1,
                  &update, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(update, 1, active_id.data(), static_cast<int>(active_id.size()),
                                SQLITE_TRANSIENT),
              SQLITE_OK);
    EXPECT_EQ(sqlite3_step(update), SQLITE_CONSTRAINT);
    sqlite3_finalize(update);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

TEST(RunStoreTest, RefusesUnknownNewerSchemaWithoutMutation) {
    TemporaryDatabase database;
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw,
                           "CREATE TABLE schema_meta(version INTEGER NOT NULL);"
                           "INSERT INTO schema_meta VALUES(99);",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

    EXPECT_THROW(SqliteRunStore(database.path(), sample_build()), std::runtime_error);

    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "SELECT version FROM schema_meta", -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 99);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

TEST(RunStoreTest, MigratesVersionOneMetadataAndPreservesExistingRuns) {
    TemporaryDatabase database;
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(
                  raw,
                  "CREATE TABLE schema_meta(version INTEGER NOT NULL CHECK(version=1));"
                  "INSERT INTO schema_meta VALUES(1);"
                  "CREATE TABLE runs(id TEXT PRIMARY KEY,"
                  "created_at_unix_ns INTEGER NOT NULL,"
                  "finalized_at_unix_ns INTEGER);"
                  "INSERT INTO runs VALUES('legacy-run',100,NULL);",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

    SqliteRunStore store(database.path(), sample_build());
    EXPECT_EQ(store.schema_version(), 2);
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  raw, "SELECT id FROM runs WHERE id='legacy-run'", -1,
                  &query, nullptr), SQLITE_OK);
    EXPECT_EQ(sqlite3_step(query), SQLITE_ROW);
    sqlite3_finalize(query);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

TEST(RunStoreTest, RejectsEmptyTrackNamespaceFieldAtomically) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    auto config = sample_config();
    config.track_fixture->namespace_fields = {""};
    EXPECT_THROW(store.create_run(config), std::invalid_argument);
    EXPECT_EQ(store.list({1, 0}).total, 0u);
}

TEST(RunStoreTest, PreservesRunsWithoutOptionalTrackFixture) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    auto config = sample_config();
    config.track_fixture.reset();
    const auto id = store.create_run(config);
    EXPECT_FALSE(store.load(id).config.track_fixture.has_value());
    ASSERT_EQ(store.list({1, 0}).items.size(), 1u);
    EXPECT_FALSE(store.list({1, 0}).items[0].config.track_fixture.has_value());
}

TEST(RunStoreTest, RoundTripsCompleteConfigurationAndBuildIdentity) {
    TemporaryDatabase database;
    const auto build = sample_build();
    const auto config = sample_config();
    SqliteRunStore store(database.path(), build);

    const auto id = store.create_run(config);
    const auto loaded = store.load(id);

    EXPECT_FALSE(id.empty());
    EXPECT_EQ(loaded.id, id);
    EXPECT_EQ(loaded.state, RunState::Active);
    EXPECT_GT(loaded.created_at_unix_ns, 0);
    EXPECT_FALSE(loaded.finalized_at_unix_ns.has_value());
    EXPECT_FALSE(loaded.score.has_value());
    EXPECT_TRUE(loaded.outcomes.empty());
    EXPECT_TRUE(loaded.events.empty());
    EXPECT_EQ(loaded.config.draft, config.draft);
    EXPECT_EQ(loaded.config.transport, config.transport);
    EXPECT_EQ(loaded.config.mode, config.mode);
    EXPECT_EQ(loaded.config.scenario_ids, config.scenario_ids);
    EXPECT_EQ(loaded.config.timeout, config.timeout);
    ASSERT_TRUE(loaded.config.track_fixture.has_value());
    EXPECT_EQ(loaded.config.track_fixture->namespace_fields,
              config.track_fixture->namespace_fields);
    EXPECT_EQ(loaded.config.track_fixture->track_name,
              config.track_fixture->track_name);
    const auto page = store.list({1, 0});
    ASSERT_EQ(page.items.size(), 1u);
    ASSERT_TRUE(page.items[0].config.track_fixture.has_value());
    EXPECT_EQ(page.items[0].config.track_fixture->namespace_fields,
              config.track_fixture->namespace_fields);
    EXPECT_EQ(loaded.build.version, build.version);
    EXPECT_EQ(loaded.build.source_revision, build.source_revision);
    EXPECT_EQ(loaded.build.dependencies, build.dependencies);
}

TEST(RunStoreTest, AllocatesUniqueRunIdsAcrossStoresOpenedAtTheSameDatabaseState) {
    TemporaryDatabase database;
    app::RunId seed_id;
    {
        SqliteRunStore bootstrap(database.path(), sample_build());
        seed_id = bootstrap.create_run(sample_config());
    }

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* update = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "UPDATE runs SET created_at_unix_ns=? WHERE id=?", -1,
                                 &update, nullptr),
              SQLITE_OK);
    const auto future =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (std::chrono::system_clock::now() + std::chrono::hours(24)).time_since_epoch())
            .count();
    ASSERT_EQ(sqlite3_bind_int64(update, 1, future), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(update, 2, seed_id.data(), static_cast<int>(seed_id.size()),
                                SQLITE_TRANSIENT),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(update), SQLITE_DONE);
    sqlite3_finalize(update);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

    SqliteRunStore first(database.path(), sample_build());
    SqliteRunStore second(database.path(), sample_build());
    std::barrier start(3);
    auto create = [&start](SqliteRunStore& store) {
        start.arrive_and_wait();
        return store.create_run(sample_config());
    };
    auto first_result = std::async(std::launch::async, create, std::ref(first));
    auto second_result = std::async(std::launch::async, create, std::ref(second));
    start.arrive_and_wait();
    const auto first_id = first_result.get();
    const auto second_id = second_result.get();

    EXPECT_NE(first_id, second_id);
    EXPECT_EQ(first.load(first_id).id, first_id);
    EXPECT_EQ(first.load(second_id).id, second_id);
}

TEST(RunStoreTest, FinalizationTimestampAdvancesPastRunsCreatedByAnotherStore) {
    TemporaryDatabase database;
    app::RunId seed_id;
    {
        SqliteRunStore bootstrap(database.path(), sample_build());
        seed_id = bootstrap.create_run(sample_config());
    }

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* update = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "UPDATE runs SET created_at_unix_ns=? WHERE id=?", -1,
                                 &update, nullptr),
              SQLITE_OK);
    const auto future =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (std::chrono::system_clock::now() + std::chrono::hours(24)).time_since_epoch())
            .count();
    ASSERT_EQ(sqlite3_bind_int64(update, 1, future), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(update, 2, seed_id.data(), static_cast<int>(seed_id.size()),
                                SQLITE_TRANSIENT),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(update), SQLITE_DONE);
    sqlite3_finalize(update);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

    SqliteRunStore creator(database.path(), sample_build());
    SqliteRunStore stale_finalizer(database.path(), sample_build());
    creator.create_run(sample_config());
    creator.create_run(sample_config());
    creator.create_run(sample_config());
    const auto target_id = creator.create_run(sample_config());
    const auto before = stale_finalizer.load(target_id);
    const std::vector outcomes{
        requirements::Outcome{"D21-MUST-1", requirements::OutcomeState::Pass}};

    stale_finalizer.finalize(target_id, sample_score(), outcomes);
    const auto finalized = stale_finalizer.load(target_id);

    ASSERT_TRUE(finalized.finalized_at_unix_ns.has_value());
    EXPECT_GT(*finalized.finalized_at_unix_ns, before.created_at_unix_ns);
}

TEST(RunStoreTest, AppendsEventsInCallerOrderAndPaginatesWithoutGaps) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    const auto id = store.create_run(sample_config());
    const std::vector events{event(0), event(1), event(2), event(3), event(4)};

    store.append_events(id, {});
    store.append_events(id, std::span<const EvidenceEvent>(events).first(2));
    store.append_events(id, std::span<const EvidenceEvent>(events).subspan(2));

    const auto first = store.list_events(id, {.limit = 2, .offset = 0});
    const auto second = store.list_events(id, {.limit = 2, .offset = 2});
    const auto third = store.list_events(id, {.limit = 2, .offset = 4});
    ASSERT_EQ(first.items.size(), 2U);
    ASSERT_EQ(second.items.size(), 2U);
    ASSERT_EQ(third.items.size(), 1U);
    EXPECT_EQ(first.total, 5U);
    EXPECT_EQ(first.next_offset, 2U);
    EXPECT_EQ(second.next_offset, 4U);
    EXPECT_FALSE(third.next_offset.has_value());

    std::vector<EvidenceEvent> combined;
    combined.insert(combined.end(), first.items.begin(), first.items.end());
    combined.insert(combined.end(), second.items.begin(), second.items.end());
    combined.insert(combined.end(), third.items.begin(), third.items.end());
    ASSERT_EQ(combined.size(), events.size());
    for (std::size_t index = 0; index < events.size(); ++index) {
        EXPECT_EQ(combined[index].sequence, index);
        EXPECT_EQ(combined[index].monotonic_time_ns, events[index].monotonic_time_ns);
        EXPECT_EQ(combined[index].wall_time_unix_ns, events[index].wall_time_unix_ns);
        EXPECT_EQ(combined[index].kind, events[index].kind);
        EXPECT_EQ(combined[index].detail, events[index].detail);
        EXPECT_EQ(combined[index].connection_id, events[index].connection_id);
        EXPECT_EQ(combined[index].stream_id, events[index].stream_id);
        EXPECT_EQ(combined[index].request_id, events[index].request_id);
        EXPECT_EQ(combined[index].scenario_id, events[index].scenario_id);
        EXPECT_EQ(combined[index].requirement_id, events[index].requirement_id);
    }

    const auto loaded = store.load(id);
    EXPECT_EQ(loaded.events.size(), events.size());
    EXPECT_THROW((store.list_events(id, {.limit = 0, .offset = 0})), std::invalid_argument);
    EXPECT_THROW((store.list_events(id, {.limit = 101, .offset = 0})), std::invalid_argument);
}

TEST(RunStoreTest, RejectsUnknownRunIds) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    const std::vector one_event{event(1)};
    const std::vector one_outcome{
        requirements::Outcome{"D21-MUST-1", requirements::OutcomeState::Pass}};

    EXPECT_THROW(store.load("missing"), std::out_of_range);
    EXPECT_THROW(store.append_events("missing", one_event), std::out_of_range);
    EXPECT_THROW(store.finalize("missing", sample_score(), one_outcome), std::out_of_range);
    EXPECT_THROW((store.list_events("missing", {})), std::out_of_range);
}

TEST(RunStoreTest, FinalizesAtomicallyAndMakesRunImmutable) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    const auto id = store.create_run(sample_config());
    const std::vector events{event(1)};
    store.append_events(id, events);
    const std::vector outcomes{
        requirements::Outcome{"D21-MUST-1", requirements::OutcomeState::Pass},
        requirements::Outcome{"D21-SHOULD-2", requirements::OutcomeState::NotRun},
    };

    store.finalize(id, sample_score(), outcomes);
    const auto loaded = store.load(id);

    ASSERT_EQ(loaded.state, RunState::Finalized);
    ASSERT_TRUE(loaded.finalized_at_unix_ns.has_value());
    ASSERT_TRUE(loaded.score.has_value());
    EXPECT_EQ(loaded.score->verdict, requirements::RunVerdict::Fail);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(loaded.score->required.earned, maximum - 5);
    EXPECT_EQ(loaded.score->required.possible, maximum);
    EXPECT_EQ(loaded.score->weighted.earned, maximum - 4);
    EXPECT_EQ(loaded.score->weighted.possible, maximum - 3);
    EXPECT_EQ(loaded.score->coverage.earned, maximum - 2);
    EXPECT_EQ(loaded.score->coverage.possible, maximum - 1);
    ASSERT_EQ(loaded.outcomes.size(), outcomes.size());
    EXPECT_EQ(loaded.outcomes[0].requirement_id, outcomes[0].requirement_id);
    EXPECT_EQ(loaded.outcomes[0].state, outcomes[0].state);
    EXPECT_EQ(loaded.outcomes[1].requirement_id, outcomes[1].requirement_id);
    EXPECT_EQ(loaded.outcomes[1].state, outcomes[1].state);

    EXPECT_THROW(store.append_events(id, events), std::logic_error);
    EXPECT_THROW(store.finalize(id, sample_score(), outcomes), std::logic_error);
    const auto unchanged = store.load(id);
    EXPECT_EQ(unchanged.events.size(), 1U);
    EXPECT_EQ(unchanged.outcomes.size(), 2U);
    EXPECT_EQ(unchanged.finalized_at_unix_ns, loaded.finalized_at_unix_ns);
}

TEST(RunStoreTest, RollsBackEveryFinalizeWriteWhenALaterOutcomeViolatesConstraint) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    const auto id = store.create_run(sample_config());
    const std::vector invalid{
        requirements::Outcome{"duplicate", requirements::OutcomeState::Pass},
        requirements::Outcome{"duplicate", requirements::OutcomeState::Fail},
    };

    EXPECT_THROW(store.finalize(id, sample_score(), invalid), std::runtime_error);
    const auto rolled_back = store.load(id);
    EXPECT_EQ(rolled_back.state, RunState::Active);
    EXPECT_FALSE(rolled_back.finalized_at_unix_ns.has_value());
    EXPECT_FALSE(rolled_back.score.has_value());
    EXPECT_TRUE(rolled_back.outcomes.empty());

    const std::vector valid{
        requirements::Outcome{"D21-MUST-1", requirements::OutcomeState::Pass}};
    EXPECT_NO_THROW(store.finalize(id, sample_score(), valid));
    EXPECT_EQ(store.load(id).state, RunState::Finalized);
}

TEST(RunStoreTest, ListsNewestFirstWithStablePaginationAndIdTieBreaker) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    const auto first_id = store.create_run(sample_config());
    const auto second_id = store.create_run(sample_config());
    const auto third_id = store.create_run(sample_config());

    const auto page1 = store.list({.limit = 2, .offset = 0});
    const auto page2 = store.list({.limit = 2, .offset = 2});
    ASSERT_EQ(page1.items.size(), 2U);
    ASSERT_EQ(page2.items.size(), 1U);
    EXPECT_EQ(page1.total, 3U);
    EXPECT_EQ(page1.next_offset, 2U);
    EXPECT_FALSE(page2.next_offset.has_value());
    EXPECT_EQ(page1.items[0].id, third_id);
    EXPECT_EQ(page1.items[1].id, second_id);
    EXPECT_EQ(page2.items[0].id, first_id);
    EXPECT_EQ(page1.items[0].config.draft, app::DraftVersion::Draft21);
    EXPECT_EQ(page1.items[0].config.transport, app::TransportKind::WebTransport);
    EXPECT_EQ(page1.items[0].config.mode, app::RunMode::Driven);
    EXPECT_EQ(page1.items[0].config.scenario_ids, sample_config().scenario_ids);
    EXPECT_EQ(page1.items[0].config.timeout, 12'345ms);

    EXPECT_THROW((store.list({.limit = 101, .offset = 0})), std::invalid_argument);
}

}  // namespace
}  // namespace moq::interop::storage
