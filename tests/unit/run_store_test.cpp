#include "moq/interop/storage/run_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
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


// Schema version 2, verbatim from before publisher capabilities existed.
constexpr const char* kVersionTwoSchema = R"SQL(CREATE TABLE schema_meta (
    version INTEGER NOT NULL CHECK (version = 2)
);
INSERT INTO schema_meta(version) VALUES (2);

CREATE TABLE runs (
    id TEXT PRIMARY KEY,
    draft INTEGER NOT NULL CHECK (draft IN (18, 21)),
    transport INTEGER NOT NULL CHECK (transport IN (0, 1)),
    mode INTEGER NOT NULL CHECK (mode IN (0, 1)),
    timeout_ms INTEGER NOT NULL CHECK (timeout_ms >= 0),
    state INTEGER NOT NULL CHECK (state IN (0, 1)),
    created_at_unix_ns INTEGER NOT NULL,
    finalized_at_unix_ns INTEGER,
    CHECK ((state = 0 AND finalized_at_unix_ns IS NULL) OR
           (state = 1 AND finalized_at_unix_ns IS NOT NULL AND
            finalized_at_unix_ns > created_at_unix_ns))
);

CREATE TABLE run_builds (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    version TEXT NOT NULL,
    source_revision TEXT NOT NULL
);

CREATE TABLE build_dependencies (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    revision TEXT NOT NULL,
    PRIMARY KEY (run_id, name)
);

CREATE TABLE selected_scenarios (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    scenario_id TEXT NOT NULL,
    PRIMARY KEY (run_id, position)
);

CREATE TABLE run_track_fixtures (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    track_name TEXT NOT NULL
);

CREATE TABLE run_track_namespace_fields (
    run_id TEXT NOT NULL REFERENCES run_track_fixtures(run_id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    value TEXT NOT NULL,
    PRIMARY KEY (run_id, position)
);

CREATE TABLE evidence_events (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL CHECK (sequence >= 0),
    monotonic_time_ns INTEGER NOT NULL,
    wall_time_unix_ns INTEGER NOT NULL,
    kind TEXT NOT NULL,
    detail TEXT NOT NULL,
    connection_id TEXT,
    stream_id TEXT,
    request_id TEXT,
    scenario_id TEXT,
    requirement_id TEXT,
    PRIMARY KEY (run_id, sequence)
);

CREATE TABLE final_scores (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    verdict INTEGER NOT NULL CHECK (verdict BETWEEN 0 AND 3),
    required_earned TEXT NOT NULL,
    required_possible TEXT NOT NULL,
    weighted_earned TEXT NOT NULL,
    weighted_possible TEXT NOT NULL,
    coverage_earned TEXT NOT NULL,
    coverage_possible TEXT NOT NULL
);

CREATE TABLE outcomes (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    requirement_id TEXT NOT NULL,
    state INTEGER NOT NULL CHECK (state BETWEEN 0 AND 4),
    PRIMARY KEY (run_id, requirement_id),
    UNIQUE (run_id, position)
);

CREATE INDEX runs_newest_idx ON runs(created_at_unix_ns DESC, id DESC);
CREATE INDEX evidence_events_page_idx ON evidence_events(run_id, sequence);
)SQL";

// Schema version 1, verbatim as last shipped (7b37485): version 2 without the track
// fixture tables.
constexpr const char* kVersionOneSchema = R"SQL(CREATE TABLE schema_meta (
    version INTEGER NOT NULL CHECK (version = 1)
);
INSERT INTO schema_meta(version) VALUES (1);

CREATE TABLE runs (
    id TEXT PRIMARY KEY,
    draft INTEGER NOT NULL CHECK (draft IN (18, 21)),
    transport INTEGER NOT NULL CHECK (transport IN (0, 1)),
    mode INTEGER NOT NULL CHECK (mode IN (0, 1)),
    timeout_ms INTEGER NOT NULL CHECK (timeout_ms >= 0),
    state INTEGER NOT NULL CHECK (state IN (0, 1)),
    created_at_unix_ns INTEGER NOT NULL,
    finalized_at_unix_ns INTEGER,
    CHECK ((state = 0 AND finalized_at_unix_ns IS NULL) OR
           (state = 1 AND finalized_at_unix_ns IS NOT NULL AND
            finalized_at_unix_ns > created_at_unix_ns))
);

CREATE TABLE run_builds (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    version TEXT NOT NULL,
    source_revision TEXT NOT NULL
);

CREATE TABLE build_dependencies (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    revision TEXT NOT NULL,
    PRIMARY KEY (run_id, name)
);

CREATE TABLE selected_scenarios (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    scenario_id TEXT NOT NULL,
    PRIMARY KEY (run_id, position)
);

CREATE TABLE evidence_events (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL CHECK (sequence >= 0),
    monotonic_time_ns INTEGER NOT NULL,
    wall_time_unix_ns INTEGER NOT NULL,
    kind TEXT NOT NULL,
    detail TEXT NOT NULL,
    connection_id TEXT,
    stream_id TEXT,
    request_id TEXT,
    scenario_id TEXT,
    requirement_id TEXT,
    PRIMARY KEY (run_id, sequence)
);

CREATE TABLE final_scores (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    verdict INTEGER NOT NULL CHECK (verdict BETWEEN 0 AND 3),
    required_earned TEXT NOT NULL,
    required_possible TEXT NOT NULL,
    weighted_earned TEXT NOT NULL,
    weighted_possible TEXT NOT NULL,
    coverage_earned TEXT NOT NULL,
    coverage_possible TEXT NOT NULL
);

CREATE TABLE outcomes (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    requirement_id TEXT NOT NULL,
    state INTEGER NOT NULL CHECK (state BETWEEN 0 AND 4),
    PRIMARY KEY (run_id, requirement_id),
    UNIQUE (run_id, position)
);

CREATE INDEX runs_newest_idx ON runs(created_at_unix_ns DESC, id DESC);
CREATE INDEX evidence_events_page_idx ON evidence_events(run_id, sequence);
)SQL";

// The version 2 to 3 step, verbatim from the store's migration chain.
constexpr const char* kVersionTwoToThreeMigration =
    "CREATE TABLE run_publisher_capabilities ("
    "run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,"
    "fetch INTEGER NOT NULL CHECK (fetch IN (0, 1)));"
    "CREATE TABLE schema_meta_v3 (version INTEGER NOT NULL CHECK (version = 3));"
    "INSERT INTO schema_meta_v3(version) VALUES (3);"
    "DROP TABLE schema_meta;"
    "ALTER TABLE schema_meta_v3 RENAME TO schema_meta;";

// Every table holding run data; all but `runs` reference it (directly or, for the
// namespace fields, through run_track_fixtures) with ON DELETE CASCADE.
const std::vector<std::string> kRunTables{
    "runs",           "run_builds",           "build_dependencies",
    "selected_scenarios", "run_track_fixtures", "run_track_namespace_fields",
    "run_publisher_capabilities", "evidence_events", "final_scores", "outcomes"};

void exec_or_fail(sqlite3* raw, const std::string& sql) {
    char* error = nullptr;
    const auto result = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "" : error;
    sqlite3_free(error);
    ASSERT_EQ(result, SQLITE_OK) << message << "\n" << sql;
}

// A version 3 database built directly with SQL, not through the store. With
// `accepts_draft22` false it is a database migrated from version 2 (the runs CHECK is
// draft IN (18, 21)); with true it is a database created fresh at version 3 after draft
// 22 was added to schema.sql.
void create_version_three_database(const std::filesystem::path& path,
                                   bool accepts_draft22 = false) {
    std::string schema = kVersionTwoSchema;
    if (accepts_draft22) {
        const std::string old_check = "draft IN (18, 21)";
        const auto at = schema.find(old_check);
        ASSERT_NE(at, std::string::npos);
        schema.replace(at, old_check.size(), "draft IN (18, 21, 22)");
    }
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    exec_or_fail(raw, schema);
    exec_or_fail(raw, kVersionTwoToThreeMigration);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

// The version 3 to 4 step, verbatim from the store's migration chain (the draft CHECK
// accepts 22 but not 106).
constexpr const char* kVersionThreeToFourMigration =
    "CREATE TABLE runs_new (\n"
    "    id TEXT PRIMARY KEY,\n"
    "    draft INTEGER NOT NULL CHECK (draft IN (18, 21, 22)),\n"
    "    transport INTEGER NOT NULL CHECK (transport IN (0, 1)),\n"
    "    mode INTEGER NOT NULL CHECK (mode IN (0, 1)),\n"
    "    timeout_ms INTEGER NOT NULL CHECK (timeout_ms >= 0),\n"
    "    state INTEGER NOT NULL CHECK (state IN (0, 1)),\n"
    "    created_at_unix_ns INTEGER NOT NULL,\n"
    "    finalized_at_unix_ns INTEGER,\n"
    "    CHECK ((state = 0 AND finalized_at_unix_ns IS NULL) OR\n"
    "           (state = 1 AND finalized_at_unix_ns IS NOT NULL AND\n"
    "            finalized_at_unix_ns > created_at_unix_ns))\n"
    ");"
    "INSERT INTO runs_new(rowid,id,draft,transport,mode,timeout_ms,state,"
    "created_at_unix_ns,finalized_at_unix_ns) "
    "SELECT rowid,id,draft,transport,mode,timeout_ms,state,"
    "created_at_unix_ns,finalized_at_unix_ns FROM runs;"
    "DROP TABLE runs;"
    "ALTER TABLE runs_new RENAME TO runs;"
    "CREATE INDEX runs_newest_idx ON runs(created_at_unix_ns DESC, id DESC);"
    "CREATE TABLE schema_meta_v4 (version INTEGER NOT NULL CHECK (version = 4));"
    "INSERT INTO schema_meta_v4(version) VALUES (4);"
    "DROP TABLE schema_meta;"
    "ALTER TABLE schema_meta_v4 RENAME TO schema_meta;";

// A version 4 database built directly with SQL, not through the store: the version 3
// fixture with the version 3 to 4 step applied.
void create_version_four_database(const std::filesystem::path& path) {
    create_version_three_database(path);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    exec_or_fail(raw, "PRAGMA foreign_keys=OFF");
    exec_or_fail(raw, std::string("BEGIN;") + kVersionThreeToFourMigration + "COMMIT;");
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

// The result code of one raw draft 106 (moq-lite-06) insert, made without the store.
int raw_draft106_insert(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    EXPECT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const auto result = sqlite3_exec(
        raw,
        "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,created_at_unix_ns) "
        "VALUES('raw-draft106',106,0,0,1000,0,900)",
        nullptr, nullptr, nullptr);
    sqlite3_close(raw);
    return result;
}

// Three runs with a row in every child table between them, written by raw SQL.
void seed_runs_with_children(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    exec_or_fail(raw, "PRAGMA foreign_keys=ON");
    exec_or_fail(
        raw,
        "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,created_at_unix_ns,"
        "finalized_at_unix_ns) VALUES"
        "('run-a',18,0,0,1000,0,100,NULL),"
        "('run-b',21,1,1,2500,1,200,250),"
        "('run-c',21,0,1,0,1,300,301);"
        "INSERT INTO run_builds VALUES('run-a','0.1','abc'),('run-b','0.2''q','d|e'),"
        "('run-c','0.3','a'||char(0)||'bc');"
        "INSERT INTO build_dependencies VALUES('run-b','sqlite3','3.46'),"
        "('run-b','quiche','rev,1'),('run-c','picoquic','r2');"
        "INSERT INTO selected_scenarios VALUES('run-a',0,'session/setup'),"
        "('run-b',0,'fetch-publisher-track-range'),('run-b',1,'x,y|z');"
        "INSERT INTO run_track_fixtures VALUES('run-b','track'),('run-c','other');"
        "INSERT INTO run_track_namespace_fields VALUES('run-b',0,'scope'),"
        "('run-b',1,'n s'),('run-c',0,'one');"
        "INSERT INTO run_publisher_capabilities VALUES('run-b',0),('run-c',1);"
        "INSERT INTO evidence_events VALUES"
        "('run-a',0,10,20,'kind-a','{\"d\":1}',NULL,NULL,NULL,NULL,NULL),"
        "('run-b',0,11,21,'kind''b','{}','c1','s1','r1','scenario/1','D21-MUST-1'),"
        "('run-b',1,12,22,'kind-c','{\"x\":\"DELETE FROM runs\"}','c1',NULL,'r2',NULL,NULL);"
        "INSERT INTO final_scores VALUES"
        "('run-b',1,'18446744073709551610','18446744073709551615','1','2','3','4'),"
        "('run-c',0,'0','0','0','0','0','0');"
        "INSERT INTO outcomes VALUES('run-b',0,'D21-MUST-1',0),('run-b',1,'D21-SHOULD-2',3),"
        "('run-c',0,'D21-MUST-1',4);");
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

// Every row of every run table, with rowids, column types and exact bytes.
std::string dump_run_tables(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    EXPECT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    std::string dump;
    for (const auto& table : kRunTables) {
        dump += "[" + table + "]\n";
        sqlite3_stmt* query = nullptr;
        const auto sql = "SELECT rowid, * FROM " + table + " ORDER BY rowid";
        EXPECT_EQ(sqlite3_prepare_v2(raw, sql.c_str(), -1, &query, nullptr), SQLITE_OK) << sql;
        while (query != nullptr && sqlite3_step(query) == SQLITE_ROW) {
            for (int column = 0; column < sqlite3_column_count(query); ++column) {
                const auto type = sqlite3_column_type(query, column);
                dump += std::to_string(type) + ":";
                if (type == SQLITE_INTEGER) {
                    dump += std::to_string(sqlite3_column_int64(query, column));
                } else if (type != SQLITE_NULL) {
                    const auto* bytes =
                        static_cast<const char*>(sqlite3_column_blob(query, column));
                    const auto size = sqlite3_column_bytes(query, column);
                    dump += std::to_string(size) + "=" + std::string(bytes, bytes + size);
                }
                dump += "|";
            }
            dump += "\n";
        }
        sqlite3_finalize(query);
    }
    sqlite3_close(raw);
    return dump;
}

std::int64_t query_integer(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* raw = nullptr;
    EXPECT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(raw, sql.c_str(), -1, &query, nullptr), SQLITE_OK) << sql;
    std::int64_t value = -1;
    if (query != nullptr && sqlite3_step(query) == SQLITE_ROW) value = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query);
    sqlite3_close(raw);
    return value;
}

std::string query_text(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* raw = nullptr;
    EXPECT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(raw, sql.c_str(), -1, &query, nullptr), SQLITE_OK) << sql;
    std::string value;
    if (query != nullptr && sqlite3_step(query) == SQLITE_ROW) {
        const auto* bytes = sqlite3_column_text(query, 0);
        if (bytes != nullptr) value = reinterpret_cast<const char*>(bytes);
    }
    sqlite3_finalize(query);
    sqlite3_close(raw);
    return value;
}

// The result code of one raw draft 22 insert, made without the store.
int raw_draft22_insert(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    EXPECT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const auto result = sqlite3_exec(
        raw,
        "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,created_at_unix_ns) "
        "VALUES('raw-draft22',22,0,0,1000,0,900)",
        nullptr, nullptr, nullptr);
    sqlite3_close(raw);
    return result;
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

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

TEST(RunStoreTest, CreatesCurrentSchemaAndEnablesForeignKeys) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());

    EXPECT_EQ(store.schema_version(), 5);
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
    // The real version 1 schema: the version 4 step copies every runs column, so the
    // fixture needs the columns every shipped version 1 database had.
    ASSERT_EQ(sqlite3_exec(raw, kVersionOneSchema, nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(
                  raw,
                  "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,created_at_unix_ns) "
                  "VALUES('legacy-run',21,0,0,1000,0,100);",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

    SqliteRunStore store(database.path(), sample_build());
    EXPECT_EQ(store.schema_version(), 5);
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

TEST(RunStoreTest, RoundTripsDraft22AndRejectsOutOfRangeStoredDrafts) {
    // Every schema version 4 database (new or migrated) stores draft 22 runs; this one is new.
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    auto config = sample_config();
    config.draft = app::DraftVersion::Draft22;
    const auto id = store.create_run(config);
    EXPECT_EQ(store.load(id).config.draft, app::DraftVersion::Draft22);
    ASSERT_FALSE(store.list({10, 0}).items.empty());
    EXPECT_EQ(store.list({10, 0}).items.front().config.draft, app::DraftVersion::Draft22);
}

TEST(RunStoreTest, RejectsOutOfRangeStoredDraftOnLoad) {
    for (const int bad : {17, 23, 0, -1}) {
        TemporaryDatabase database;
        SqliteRunStore store(database.path(), sample_build());
        const auto id = store.create_run(sample_config());
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
        // The schema CHECK would refuse the value; drop it for this corruption test.
        ASSERT_EQ(sqlite3_exec(raw, "PRAGMA ignore_check_constraints=ON", nullptr, nullptr,
                               nullptr),
                  SQLITE_OK);
        const std::string sql = "UPDATE runs SET draft=" + std::to_string(bad) +
                                " WHERE id='" + id + "'";
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << bad;
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
        try {
            (void)store.load(id);
            ADD_FAILURE() << "load accepted draft " << bad;
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("invalid stored draft version"),
                      std::string::npos)
                << bad;
        }
    }
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


TEST(RunStoreTest, DefaultsToACapablePublisherAndRoundTripsDeclaredCapabilities) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    const auto capable = store.create_run(sample_config());
    EXPECT_TRUE(store.load(capable).config.publisher_capabilities.fetch);

    auto config = sample_config();
    config.publisher_capabilities.fetch = false;
    const auto declared = store.create_run(config);
    EXPECT_FALSE(store.load(declared).config.publisher_capabilities.fetch);
    // The run list carries the declaration too, newest first.
    const auto page = store.list({2, 0});
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].id, declared);
    EXPECT_FALSE(page.items[0].config.publisher_capabilities.fetch);
    EXPECT_TRUE(page.items[1].config.publisher_capabilities.fetch);
    store.finalize(declared, sample_score(), {});
    EXPECT_FALSE(store.load(declared).config.publisher_capabilities.fetch);
}

TEST(RunStoreTest, MigratesVersionTwoDatabasesAndTreatsOldRunsAsCapable) {
    TemporaryDatabase database;
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    // The version 2 schema as shipped before publisher capabilities, plus one run.
    ASSERT_EQ(sqlite3_exec(raw, kVersionTwoSchema, nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(
                  raw,
                  "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,created_at_unix_ns) "
                  "VALUES('legacy-v2',18,0,0,1000,0,100);"
                  "INSERT INTO run_builds VALUES('legacy-v2','0.1','abc');"
                  "INSERT INTO selected_scenarios VALUES('legacy-v2',0,'fetch-publisher-track-range');",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

    {
        SqliteRunStore store(database.path(), sample_build());
        EXPECT_EQ(store.schema_version(), 5);
        const auto legacy = store.load("legacy-v2");
        EXPECT_EQ(legacy.config.scenario_ids, (std::vector<std::string>{"fetch-publisher-track-range"}));
        EXPECT_TRUE(legacy.config.publisher_capabilities.fetch);
        auto config = sample_config();
        config.publisher_capabilities.fetch = false;
        EXPECT_FALSE(store.load(store.create_run(config)).config.publisher_capabilities.fetch);
    }
    // Reopening a migrated database is a no-op.
    SqliteRunStore reopened(database.path(), sample_build());
    EXPECT_EQ(reopened.schema_version(), 5);
    EXPECT_EQ(reopened.list({10, 0}).total, 2u);
}

TEST(RunStoreTest, SchemaRejectsAnUnknownCapabilityValue) {
    TemporaryDatabase database;
    app::RunId id;
    {
        SqliteRunStore store(database.path(), sample_build());
        id = store.create_run(sample_config());
    }
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr), SQLITE_OK);
    const std::string sql =
        "UPDATE run_publisher_capabilities SET fetch=2 WHERE run_id='" + id + "'";
    EXPECT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_CONSTRAINT);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

// Schema version 4 rebuilds `runs` so every database, not only new ones, accepts draft 22.

TEST(RunStoreSchemaFourTest, VersionThreeDatabaseRejectsDraft22BeforeMigration) {
    TemporaryDatabase database;
    create_version_three_database(database.path());
    EXPECT_EQ(query_integer(database.path(), "SELECT version FROM schema_meta"), 3);
    EXPECT_EQ(raw_draft22_insert(database.path()), SQLITE_CONSTRAINT);
}

TEST(RunStoreSchemaFourTest, MigratesVersionThreeKeepingEveryRowAndTheCascade) {
    TemporaryDatabase database;
    create_version_three_database(database.path());
    seed_runs_with_children(database.path());
    const auto before = dump_run_tables(database.path());
    ASSERT_NE(before.find("run-c"), std::string::npos);

    {
        SqliteRunStore store(database.path(), sample_build());
        EXPECT_EQ(store.schema_version(), 5);
        EXPECT_TRUE(store.foreign_keys_enabled());
        EXPECT_EQ(store.list({10, 0}).total, 3u);
        const auto loaded = store.load("run-b");
        EXPECT_EQ(loaded.config.scenario_ids,
                  (std::vector<std::string>{"fetch-publisher-track-range", "x,y|z"}));
        EXPECT_FALSE(loaded.config.publisher_capabilities.fetch);
        EXPECT_EQ(loaded.events.size(), 2u);
        EXPECT_EQ(loaded.outcomes.size(), 2u);
    }
    EXPECT_EQ(dump_run_tables(database.path()), before);

    // Same table and index definitions as a database created at version 4 (the rename
    // only quotes the table name).
    TemporaryDatabase fresh;
    { SqliteRunStore store(fresh.path(), sample_build()); }
    const auto runs_sql = "SELECT sql FROM sqlite_master WHERE type='table' AND name='runs'";
    auto migrated_sql = query_text(database.path(), runs_sql);
    const std::string quoted = "CREATE TABLE \"runs\"";
    ASSERT_EQ(migrated_sql.rfind(quoted, 0), 0u) << migrated_sql;
    migrated_sql.replace(0, quoted.size(), "CREATE TABLE runs");
    EXPECT_EQ(migrated_sql, query_text(fresh.path(), runs_sql));
    const auto index_sql =
        "SELECT group_concat(sql, ';') FROM (SELECT sql FROM sqlite_master "
        "WHERE type='index' AND sql IS NOT NULL ORDER BY name)";
    EXPECT_EQ(query_text(database.path(), index_sql), query_text(fresh.path(), index_sql));
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM sqlite_master WHERE name='runs_new'"),
              0);

    // Deleting a run still cascades to every child table, including the namespace fields
    // reached through run_track_fixtures; the other runs keep their rows.
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    exec_or_fail(raw, "PRAGMA foreign_keys=ON");
    exec_or_fail(raw, "DELETE FROM runs WHERE id='run-b'");
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    for (const auto& table : kRunTables) {
        const auto column = table == "runs" ? "id" : "run_id";
        EXPECT_EQ(query_integer(database.path(), "SELECT COUNT(*) FROM " + table + " WHERE " +
                                                     column + "='run-b'"),
                  0)
            << table;
    }
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM run_track_namespace_fields WHERE run_id='run-c'"),
              1);
    EXPECT_EQ(query_integer(database.path(), "SELECT COUNT(*) FROM evidence_events"), 1);
}

TEST(RunStoreSchemaFourTest, MigrationLeavesForeignKeysCheckedAndEnabled) {
    TemporaryDatabase database;
    create_version_three_database(database.path());
    seed_runs_with_children(database.path());
    SqliteRunStore store(database.path(), sample_build());
    EXPECT_TRUE(store.foreign_keys_enabled());
    EXPECT_EQ(query_integer(database.path(), "SELECT COUNT(*) FROM pragma_foreign_key_check"), 0);
}

TEST(RunStoreSchemaFourTest, MigratedVersionOneTwoAndThreeDatabasesAcceptDraft22) {
    auto draft22 = sample_config();
    draft22.draft = app::DraftVersion::Draft22;
    const auto accepts = [&](const std::filesystem::path& path) {
        SqliteRunStore store(path, sample_build());
        EXPECT_EQ(store.schema_version(), 5);
        const auto id = store.create_run(draft22);
        EXPECT_EQ(store.load(id).config.draft, app::DraftVersion::Draft22);
    };

    TemporaryDatabase one;
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(one.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, kVersionOneSchema);
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    accepts(one.path());
    EXPECT_EQ(raw_draft22_insert(one.path()), SQLITE_OK);

    TemporaryDatabase two;
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(two.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, kVersionTwoSchema);
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    accepts(two.path());
    EXPECT_EQ(raw_draft22_insert(two.path()), SQLITE_OK);

    TemporaryDatabase three;
    create_version_three_database(three.path());
    accepts(three.path());
    EXPECT_EQ(raw_draft22_insert(three.path()), SQLITE_OK);
}

TEST(RunStoreSchemaFourTest, MigratesAFreshVersionThreeDatabaseHoldingDraft22Runs) {
    // A database created at version 3 after schema.sql accepted draft 22 already holds
    // draft 22 runs; the rebuild keeps them.
    TemporaryDatabase database;
    create_version_three_database(database.path(), true);
    ASSERT_EQ(raw_draft22_insert(database.path()), SQLITE_OK);
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, "INSERT INTO run_builds VALUES('raw-draft22','0.4','abc')");
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    const auto before = dump_run_tables(database.path());
    SqliteRunStore store(database.path(), sample_build());
    EXPECT_EQ(store.schema_version(), 5);
    EXPECT_EQ(store.load("raw-draft22").config.draft, app::DraftVersion::Draft22);
    EXPECT_EQ(dump_run_tables(database.path()), before);
}

TEST(RunStoreSchemaFourTest, ReopeningAVersionFourDatabaseChangesNothing) {
    TemporaryDatabase database;
    create_version_three_database(database.path());
    seed_runs_with_children(database.path());
    { SqliteRunStore store(database.path(), sample_build()); }
    const auto bytes = file_bytes(database.path());
    const auto rows = dump_run_tables(database.path());
    ASSERT_FALSE(bytes.empty());

    {
        SqliteRunStore reopened(database.path(), sample_build());
        EXPECT_EQ(reopened.schema_version(), 5);
        EXPECT_TRUE(reopened.foreign_keys_enabled());
    }
    EXPECT_EQ(file_bytes(database.path()), bytes);
    EXPECT_EQ(dump_run_tables(database.path()), rows);
}

// Failure injection: a row the old schema accepted but the version 4 runs definition
// rejects (draft 99, written with CHECK constraints ignored) makes the copy into runs_new
// fail deterministically, after runs_new has been created.
TEST(RunStoreSchemaFourTest, FailedCopyRollsBackToVersionThree) {
    TemporaryDatabase database;
    create_version_three_database(database.path());
    seed_runs_with_children(database.path());
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, "PRAGMA ignore_check_constraints=ON");
        exec_or_fail(raw,
                     "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,"
                     "created_at_unix_ns) VALUES('run-bad',99,0,0,1000,0,400)");
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    const auto before = dump_run_tables(database.path());
    const auto runs_sql = query_text(
        database.path(), "SELECT sql FROM sqlite_master WHERE type='table' AND name='runs'");

    try {
        SqliteRunStore store(database.path(), sample_build());
        ADD_FAILURE() << "migration accepted a draft 99 row";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("migrate SQLite schema version 3 to 4"),
                  std::string::npos)
            << error.what();
    }

    EXPECT_EQ(query_integer(database.path(), "SELECT version FROM schema_meta"), 3);
    EXPECT_EQ(dump_run_tables(database.path()), before);
    EXPECT_EQ(query_text(database.path(),
                         "SELECT sql FROM sqlite_master WHERE type='table' AND name='runs'"),
              runs_sql);
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM sqlite_master WHERE name='runs_new'"),
              0);
    EXPECT_EQ(raw_draft22_insert(database.path()), SQLITE_CONSTRAINT);
}

// Failure injection: an orphan child row (written with foreign keys off) passes the copy
// and fails PRAGMA foreign_key_check after `runs` has been dropped and replaced; the
// rollback must undo the drop and the rename.
TEST(RunStoreSchemaFourTest, FailedForeignKeyCheckRollsBackToVersionThree) {
    TemporaryDatabase database;
    create_version_three_database(database.path());
    seed_runs_with_children(database.path());
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, "PRAGMA foreign_keys=OFF");
        exec_or_fail(raw, "INSERT INTO selected_scenarios VALUES('run-ghost',0,'orphan')");
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    const auto before = dump_run_tables(database.path());

    try {
        SqliteRunStore store(database.path(), sample_build());
        ADD_FAILURE() << "migration accepted an orphan child row";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("foreign key"), std::string::npos)
            << error.what();
    }

    EXPECT_EQ(query_integer(database.path(), "SELECT version FROM schema_meta"), 3);
    EXPECT_EQ(dump_run_tables(database.path()), before);
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                            "name='runs_newest_idx' AND tbl_name='runs'"),
              1);
    EXPECT_EQ(raw_draft22_insert(database.path()), SQLITE_CONSTRAINT);
}

TEST(RunStoreSchemaFourTest, NewDatabaseIsCreatedAtVersionFourAndAcceptsDraft22) {
    TemporaryDatabase database;
    {
        SqliteRunStore store(database.path(), sample_build());
        EXPECT_EQ(store.schema_version(), 5);
        auto config = sample_config();
        config.draft = app::DraftVersion::Draft22;
        EXPECT_EQ(store.load(store.create_run(config)).config.draft, app::DraftVersion::Draft22);
    }
    EXPECT_EQ(raw_draft22_insert(database.path()), SQLITE_OK);
}

// Schema version 5 rebuilds `runs` so every database accepts draft 106 (moq-lite-06).

TEST(RunStoreSchemaFiveTest, VersionFourDatabaseRejectsDraft106BeforeMigration) {
    TemporaryDatabase database;
    create_version_four_database(database.path());
    EXPECT_EQ(query_integer(database.path(), "SELECT version FROM schema_meta"), 4);
    EXPECT_EQ(raw_draft22_insert(database.path()), SQLITE_OK);
    EXPECT_EQ(raw_draft106_insert(database.path()), SQLITE_CONSTRAINT);
}

TEST(RunStoreSchemaFiveTest, MigratesVersionFourKeepingEveryRowAndTheCascade) {
    TemporaryDatabase database;
    create_version_four_database(database.path());
    seed_runs_with_children(database.path());
    {
        // A draft 22 run alongside the 18 and 21 runs: all three stay byte-identical.
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, "PRAGMA foreign_keys=ON");
        exec_or_fail(raw,
                     "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,"
                     "created_at_unix_ns) VALUES('run-d',22,1,0,7,0,400);"
                     "INSERT INTO run_builds VALUES('run-d','0.4','abc');");
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    const auto before = dump_run_tables(database.path());
    ASSERT_NE(before.find("run-d"), std::string::npos);

    {
        SqliteRunStore store(database.path(), sample_build());
        EXPECT_EQ(store.schema_version(), 5);
        EXPECT_TRUE(store.foreign_keys_enabled());
        EXPECT_EQ(store.list({10, 0}).total, 4u);
        EXPECT_EQ(store.load("run-a").config.draft, app::DraftVersion::Draft18);
        EXPECT_EQ(store.load("run-b").config.draft, app::DraftVersion::Draft21);
        EXPECT_EQ(store.load("run-d").config.draft, app::DraftVersion::Draft22);
        const auto loaded = store.load("run-b");
        EXPECT_EQ(loaded.events.size(), 2u);
        EXPECT_EQ(loaded.outcomes.size(), 2u);
    }
    EXPECT_EQ(dump_run_tables(database.path()), before);

    TemporaryDatabase fresh;
    { SqliteRunStore store(fresh.path(), sample_build()); }
    const auto runs_sql = "SELECT sql FROM sqlite_master WHERE type='table' AND name='runs'";
    auto migrated_sql = query_text(database.path(), runs_sql);
    const std::string quoted = "CREATE TABLE \"runs\"";
    ASSERT_EQ(migrated_sql.rfind(quoted, 0), 0u) << migrated_sql;
    migrated_sql.replace(0, quoted.size(), "CREATE TABLE runs");
    EXPECT_EQ(migrated_sql, query_text(fresh.path(), runs_sql));
    const auto index_sql =
        "SELECT group_concat(sql, ';') FROM (SELECT sql FROM sqlite_master "
        "WHERE type='index' AND sql IS NOT NULL ORDER BY name)";
    EXPECT_EQ(query_text(database.path(), index_sql), query_text(fresh.path(), index_sql));
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM sqlite_master WHERE name='runs_new'"),
              0);

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    exec_or_fail(raw, "PRAGMA foreign_keys=ON");
    exec_or_fail(raw, "DELETE FROM runs WHERE id='run-b'");
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    for (const auto& table : kRunTables) {
        const auto column = table == "runs" ? "id" : "run_id";
        EXPECT_EQ(query_integer(database.path(), "SELECT COUNT(*) FROM " + table + " WHERE " +
                                                     column + "='run-b'"),
                  0)
            << table;
    }
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM run_track_namespace_fields WHERE run_id='run-c'"),
              1);
    EXPECT_EQ(query_integer(database.path(), "SELECT COUNT(*) FROM evidence_events"), 1);
}

TEST(RunStoreSchemaFiveTest, MigrationLeavesForeignKeysCheckedAndEnabled) {
    TemporaryDatabase database;
    create_version_four_database(database.path());
    seed_runs_with_children(database.path());
    SqliteRunStore store(database.path(), sample_build());
    EXPECT_TRUE(store.foreign_keys_enabled());
    EXPECT_EQ(query_integer(database.path(), "SELECT COUNT(*) FROM pragma_foreign_key_check"), 0);
}

TEST(RunStoreSchemaFiveTest, MigratedVersionOneToFourDatabasesAcceptDraft106) {
    auto moqlite = sample_config();
    moqlite.draft = app::DraftVersion::MoqLite06;
    const auto accepts = [&](const std::filesystem::path& path) {
        {
            SqliteRunStore store(path, sample_build());
            EXPECT_EQ(store.schema_version(), 5);
            const auto id = store.create_run(moqlite);
            EXPECT_EQ(store.load(id).config.draft, app::DraftVersion::MoqLite06);
            EXPECT_EQ(store.list({10, 0}).items.front().config.draft,
                      app::DraftVersion::MoqLite06);
        }
        EXPECT_EQ(raw_draft106_insert(path), SQLITE_OK);
    };

    TemporaryDatabase one;
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(one.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, kVersionOneSchema);
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    accepts(one.path());

    TemporaryDatabase two;
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(two.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, kVersionTwoSchema);
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    accepts(two.path());

    TemporaryDatabase three;
    create_version_three_database(three.path());
    accepts(three.path());

    TemporaryDatabase four;
    create_version_four_database(four.path());
    accepts(four.path());
}

TEST(RunStoreSchemaFiveTest, ReopeningAVersionFiveDatabaseChangesNothing) {
    TemporaryDatabase database;
    create_version_four_database(database.path());
    seed_runs_with_children(database.path());
    { SqliteRunStore store(database.path(), sample_build()); }
    const auto bytes = file_bytes(database.path());
    const auto rows = dump_run_tables(database.path());
    ASSERT_FALSE(bytes.empty());

    {
        SqliteRunStore reopened(database.path(), sample_build());
        EXPECT_EQ(reopened.schema_version(), 5);
        EXPECT_TRUE(reopened.foreign_keys_enabled());
    }
    EXPECT_EQ(file_bytes(database.path()), bytes);
    EXPECT_EQ(dump_run_tables(database.path()), rows);
}

// Failure injection: a draft 99 row (written with CHECK constraints ignored) makes the copy
// into runs_new fail deterministically, after runs_new has been created.
TEST(RunStoreSchemaFiveTest, FailedCopyRollsBackToVersionFour) {
    TemporaryDatabase database;
    create_version_four_database(database.path());
    seed_runs_with_children(database.path());
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, "PRAGMA ignore_check_constraints=ON");
        exec_or_fail(raw,
                     "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,"
                     "created_at_unix_ns) VALUES('run-bad',99,0,0,1000,0,400)");
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    const auto before = dump_run_tables(database.path());
    const auto runs_sql = query_text(
        database.path(), "SELECT sql FROM sqlite_master WHERE type='table' AND name='runs'");

    try {
        SqliteRunStore store(database.path(), sample_build());
        ADD_FAILURE() << "migration accepted a draft 99 row";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("migrate SQLite schema version 4 to 5"),
                  std::string::npos)
            << error.what();
    }

    EXPECT_EQ(query_integer(database.path(), "SELECT version FROM schema_meta"), 4);
    EXPECT_EQ(dump_run_tables(database.path()), before);
    EXPECT_EQ(query_text(database.path(),
                         "SELECT sql FROM sqlite_master WHERE type='table' AND name='runs'"),
              runs_sql);
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM sqlite_master WHERE name='runs_new'"),
              0);
    EXPECT_EQ(raw_draft106_insert(database.path()), SQLITE_CONSTRAINT);
}

// Failure injection: an orphan child row (written with foreign keys off) passes the copy
// and fails PRAGMA foreign_key_check after `runs` has been dropped and replaced; the
// rollback must undo the drop and the rename.
TEST(RunStoreSchemaFiveTest, FailedForeignKeyCheckRollsBackToVersionFour) {
    TemporaryDatabase database;
    create_version_four_database(database.path());
    seed_runs_with_children(database.path());
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
        exec_or_fail(raw, "PRAGMA foreign_keys=OFF");
        exec_or_fail(raw, "INSERT INTO selected_scenarios VALUES('run-ghost',0,'orphan')");
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }
    const auto before = dump_run_tables(database.path());

    try {
        SqliteRunStore store(database.path(), sample_build());
        ADD_FAILURE() << "migration accepted an orphan child row";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("foreign key"), std::string::npos)
            << error.what();
    }

    EXPECT_EQ(query_integer(database.path(), "SELECT version FROM schema_meta"), 4);
    EXPECT_EQ(dump_run_tables(database.path()), before);
    EXPECT_EQ(query_integer(database.path(),
                            "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                            "name='runs_newest_idx' AND tbl_name='runs'"),
              1);
    EXPECT_EQ(raw_draft106_insert(database.path()), SQLITE_CONSTRAINT);
}

TEST(RunStoreSchemaFiveTest, NewDatabaseIsCreatedAtVersionFiveAndAcceptsDraft106) {
    TemporaryDatabase database;
    {
        SqliteRunStore store(database.path(), sample_build());
        EXPECT_EQ(store.schema_version(), 5);
        auto config = sample_config();
        config.draft = app::DraftVersion::MoqLite06;
        EXPECT_EQ(store.load(store.create_run(config)).config.draft,
                  app::DraftVersion::MoqLite06);
    }
    EXPECT_EQ(raw_draft106_insert(database.path()), SQLITE_OK);
}

TEST(RunStoreSchemaFiveTest, StillMapsDrafts18And21And22) {
    TemporaryDatabase database;
    SqliteRunStore store(database.path(), sample_build());
    for (const auto draft : {app::DraftVersion::Draft18, app::DraftVersion::Draft21,
                             app::DraftVersion::Draft22}) {
        auto config = sample_config();
        config.draft = draft;
        EXPECT_EQ(store.load(store.create_run(config)).config.draft, draft);
    }
}

}  // namespace
}  // namespace moq::interop::storage