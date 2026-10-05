#include "moq/interop/storage/run_store.h"

#include "moq/interop/app/draft_traits.h"
#include "moq/interop/storage/schema_sql.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace moq::interop::storage {
namespace {

constexpr std::size_t kMaximumPageSize = 100;

[[noreturn]] void throw_sqlite(sqlite3* database, std::string_view operation) {
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(database));
}

class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        const auto result = sqlite3_open_v2(path.string().c_str(), &handle_,
                                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                                SQLITE_OPEN_FULLMUTEX,
                                            nullptr);
        if (result != SQLITE_OK) {
            const std::string message = handle_ == nullptr ? sqlite3_errstr(result)
                                                            : sqlite3_errmsg(handle_);
            if (handle_ != nullptr) sqlite3_close(handle_);
            handle_ = nullptr;
            throw std::runtime_error("open SQLite run store: " + message);
        }
        if (sqlite3_busy_timeout(handle_, 5'000) != SQLITE_OK) {
            const std::string message = sqlite3_errmsg(handle_);
            sqlite3_close(handle_);
            handle_ = nullptr;
            throw std::runtime_error("configure SQLite busy timeout: " + message);
        }
    }

    ~Database() {
        if (handle_ != nullptr) sqlite3_close(handle_);
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* get() const { return handle_; }

private:
    sqlite3* handle_ = nullptr;
};

class Statement {
public:
    Statement(sqlite3* database, std::string_view sql) : database_(database) {
        if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement_,
                               nullptr) != SQLITE_OK) {
            throw_sqlite(database, "prepare SQLite statement");
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return statement_; }

    void bind(int index, std::string_view value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("bind SQLite text value: value is too large");
        }
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite(database_, "bind SQLite text value");
        }
    }

    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw_sqlite(database_, "bind SQLite integer value");
        }
    }

    void bind(int index, int value) {
        if (sqlite3_bind_int(statement_, index, value) != SQLITE_OK) {
            throw_sqlite(database_, "bind SQLite integer value");
        }
    }

    void bind_optional(int index, const std::optional<std::string>& value) {
        if (value.has_value()) {
            bind(index, *value);
        } else if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw_sqlite(database_, "bind SQLite null value");
        }
    }

    bool row() {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw_sqlite(database_, "step SQLite query");
    }

    void done(std::string_view operation) {
        if (sqlite3_step(statement_) != SQLITE_DONE) throw_sqlite(database_, operation);
    }

    void reset() {
        if (sqlite3_reset(statement_) != SQLITE_OK || sqlite3_clear_bindings(statement_) != SQLITE_OK) {
            throw_sqlite(database_, "reset SQLite statement");
        }
    }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* database, const char* sql, std::string_view operation) {
    char* error = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result == SQLITE_OK) return;
    const std::string message = error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    throw std::runtime_error(std::string(operation) + ": " + message);
}

class Transaction {
public:
    Transaction(sqlite3* database, std::string_view operation)
        : database_(database), operation_(operation) {
        execute(database_, "BEGIN IMMEDIATE", std::string(operation_) + " begin transaction");
    }

    ~Transaction() {
        if (!committed_) sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        execute(database_, "COMMIT", std::string(operation_) + " commit transaction");
        committed_ = true;
    }

private:
    sqlite3* database_;
    std::string operation_;
    bool committed_ = false;
};

std::string text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (value == nullptr) return {};
    const auto size = sqlite3_column_bytes(statement, column);
    return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)};
}

std::optional<std::string> optional_text(sqlite3_stmt* statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) return std::nullopt;
    return text(statement, column);
}

std::optional<std::int64_t> optional_int64(sqlite3_stmt* statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int64(statement, column);
}

template <typename Enum>
int enum_value(Enum value) {
    return static_cast<int>(value);
}

app::DraftVersion draft_version(int value) {
    if (value >= 0) {
        if (const auto draft = app::parse_draft(static_cast<unsigned>(value))) return *draft;
    }
    throw std::runtime_error("load run: invalid stored draft version");
}

app::TransportKind transport_kind(int value) {
    if (value == 0) return app::TransportKind::NativeQuic;
    if (value == 1) return app::TransportKind::WebTransport;
    throw std::runtime_error("load run: invalid stored transport kind");
}

app::RunMode run_mode(int value) {
    if (value == 0) return app::RunMode::Observed;
    if (value == 1) return app::RunMode::Driven;
    throw std::runtime_error("load run: invalid stored run mode");
}

RunState run_state(int value) {
    if (value == 0) return RunState::Active;
    if (value == 1) return RunState::Finalized;
    throw std::runtime_error("load run: invalid stored lifecycle state");
}

requirements::RunVerdict run_verdict(int value) {
    if (value < enum_value(requirements::RunVerdict::Pass) ||
        value > enum_value(requirements::RunVerdict::Error)) {
        throw std::runtime_error("load score: invalid stored verdict");
    }
    return static_cast<requirements::RunVerdict>(value);
}

requirements::OutcomeState outcome_state(int value) {
    if (value < enum_value(requirements::OutcomeState::Pass) ||
        value > enum_value(requirements::OutcomeState::NotApplicable)) {
        throw std::runtime_error("load outcome: invalid stored state");
    }
    return static_cast<requirements::OutcomeState>(value);
}

std::uint64_t unsigned_integer(sqlite3_stmt* statement, int column) {
    const auto value = text(statement, column);
    std::size_t used = 0;
    try {
        const auto result = std::stoull(value, &used);
        if (used != value.size()) throw std::invalid_argument("trailing data");
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error("load score: invalid stored unsigned integer");
    }
}

void validate_query(const RunQuery& query) {
    if (query.limit == 0 || query.limit > kMaximumPageSize) {
        throw std::invalid_argument("run-store page limit must be between 1 and 100");
    }
    if (query.offset > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("run-store page offset is too large");
    }
}

std::optional<std::size_t> next_offset(const RunQuery& query, std::size_t item_count,
                                       std::size_t total) {
    if (item_count < query.limit || query.offset >= total - item_count) return std::nullopt;
    return query.offset + item_count;
}

app::PublisherCapabilities read_publisher_capabilities(sqlite3* database, const app::RunId& id) {
    Statement capabilities(database,
                           "SELECT fetch FROM run_publisher_capabilities WHERE run_id=?");
    capabilities.bind(1, id);
    app::PublisherCapabilities result;  // no row: the run predates the declaration
    if (capabilities.row()) result.fetch = sqlite3_column_int(capabilities.get(), 0) != 0;
    return result;
}

std::optional<app::TrackFixture> read_track_fixture(
    sqlite3* database, const app::RunId& id) {
    Statement fixture(database,
                      "SELECT track_name FROM run_track_fixtures WHERE run_id=?");
    fixture.bind(1, id);
    if (!fixture.row()) return std::nullopt;
    app::TrackFixture result;
    result.track_name = text(fixture.get(), 0);
    Statement fields(database,
                     "SELECT value FROM run_track_namespace_fields "
                     "WHERE run_id=? ORDER BY position");
    fields.bind(1, id);
    while (fields.row()) result.namespace_fields.push_back(text(fields.get(), 0));
    return result;
}

}  // namespace

class SqliteRunStore::Impl {
public:
    Impl(const std::filesystem::path& path, app::BuildInfo supplied_build)
        : database(path), build(std::move(supplied_build)) {
        execute(database.get(), "PRAGMA foreign_keys=ON", "enable SQLite foreign keys");
        if (!foreign_keys_enabled_unlocked()) {
            throw std::runtime_error("enable SQLite foreign keys: pragma remained disabled");
        }
        initialize_schema();
        Statement latest(
            database.get(),
            "SELECT COALESCE(MAX(value), 0) FROM ("
            "SELECT created_at_unix_ns AS value FROM runs UNION ALL "
            "SELECT finalized_at_unix_ns FROM runs WHERE finalized_at_unix_ns IS NOT NULL)");
        if (!latest.row()) throw std::runtime_error("initialize run timestamps: query returned no row");
        last_timestamp = sqlite3_column_int64(latest.get(), 0);
    }

    bool foreign_keys_enabled_unlocked() const {
        Statement query(database.get(), "PRAGMA foreign_keys");
        return query.row() && sqlite3_column_int(query.get(), 0) == 1;
    }

    int schema_version_unlocked() const {
        Statement query(database.get(), "SELECT version FROM schema_meta");
        if (!query.row()) throw std::runtime_error("read schema version: schema_meta is empty");
        const auto version = sqlite3_column_int(query.get(), 0);
        if (query.row()) throw std::runtime_error("read schema version: multiple rows found");
        return version;
    }

    void initialize_schema() {
        std::int64_t table_count = 0;
        bool has_metadata = false;
        {
            Statement tables(database.get(),
                             "SELECT COUNT(*) FROM sqlite_master "
                             "WHERE type='table' AND name NOT LIKE 'sqlite_%'");
            if (!tables.row()) throw std::runtime_error("inspect SQLite schema: query returned no row");
            table_count = sqlite3_column_int64(tables.get(), 0);

            Statement metadata(database.get(),
                               "SELECT COUNT(*) FROM sqlite_master "
                               "WHERE type='table' AND name='schema_meta'");
            if (!metadata.row()) throw std::runtime_error("inspect schema metadata: query returned no row");
            has_metadata = sqlite3_column_int(metadata.get(), 0) == 1;
        }

        if (!has_metadata) {
            if (table_count != 0) {
                throw std::runtime_error("open SQLite run store: non-empty database has no schema version");
            }
            Transaction transaction(database.get(), "create SQLite schema version 3");
            execute(database.get(), detail::kSchemaSql, "create SQLite schema version 3");
            transaction.commit();
        }

        auto version = schema_version_unlocked();
        if (version == 1) {
            Transaction transaction(database.get(), "migrate SQLite schema version 1 to 2");
            execute(database.get(),
                    "CREATE TABLE run_track_fixtures ("
                    "run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,"
                    "track_name TEXT NOT NULL);"
                    "CREATE TABLE run_track_namespace_fields ("
                    "run_id TEXT NOT NULL REFERENCES run_track_fixtures(run_id) ON DELETE CASCADE,"
                    "position INTEGER NOT NULL CHECK (position >= 0),"
                    "value TEXT NOT NULL, PRIMARY KEY (run_id, position));"
                    "CREATE TABLE schema_meta_v2 (version INTEGER NOT NULL CHECK (version = 2));"
                    "INSERT INTO schema_meta_v2(version) VALUES (2);"
                    "DROP TABLE schema_meta;"
                    "ALTER TABLE schema_meta_v2 RENAME TO schema_meta;",
                    "migrate SQLite schema version 1 to 2");
            transaction.commit();
            version = 2;
        }
        if (version == 2) {
            // Publisher capabilities are a table of their own, so the runs table is untouched
            // and existing runs (no row) read back as a fully capable publisher.
            Transaction transaction(database.get(), "migrate SQLite schema version 2 to 3");
            execute(database.get(),
                    "CREATE TABLE run_publisher_capabilities ("
                    "run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,"
                    "fetch INTEGER NOT NULL CHECK (fetch IN (0, 1)));"
                    "CREATE TABLE schema_meta_v3 (version INTEGER NOT NULL CHECK (version = 3));"
                    "INSERT INTO schema_meta_v3(version) VALUES (3);"
                    "DROP TABLE schema_meta;"
                    "ALTER TABLE schema_meta_v3 RENAME TO schema_meta;",
                    "migrate SQLite schema version 2 to 3");
            transaction.commit();
            version = 3;
        }
        // New databases get schema.sql (draft CHECK accepts 22); databases migrated up to
        // here keep CHECK (draft IN (18, 21)) until a version 4 rebuild of `runs` exists.
        if (version != 3) {
            throw std::runtime_error("open SQLite run store: unsupported schema version " +
                                     std::to_string(version));
        }
    }

    std::int64_t logical_timestamp() {
        Statement latest(
            database.get(),
            "SELECT MAX(value) FROM ("
            "SELECT created_at_unix_ns AS value FROM runs UNION ALL "
            "SELECT finalized_at_unix_ns FROM runs WHERE finalized_at_unix_ns IS NOT NULL)");
        if (!latest.row()) {
            throw std::runtime_error("allocate logical timestamp: query returned no row");
        }

        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        auto lower_bound = last_timestamp;
        if (sqlite3_column_type(latest.get(), 0) != SQLITE_NULL) {
            const auto stored =
                static_cast<std::int64_t>(sqlite3_column_int64(latest.get(), 0));
            lower_bound = std::max(lower_bound, stored);
        }
        if (lower_bound == std::numeric_limits<std::int64_t>::max()) {
            throw std::runtime_error("allocate logical timestamp: timestamp space exhausted");
        }
        const auto candidate = std::max(now, lower_bound + 1);
        last_timestamp = candidate;
        return candidate;
    }

    static std::string run_id(std::int64_t created_at) {
        std::ostringstream stream;
        stream << "run-" << std::hex << std::setw(16) << std::setfill('0')
               << static_cast<std::uint64_t>(created_at);
        return stream.str();
    }

    RunState require_run(const app::RunId& id, std::string_view operation) const {
        Statement query(database.get(), "SELECT state FROM runs WHERE id=?");
        query.bind(1, id);
        if (!query.row()) throw std::out_of_range(std::string(operation) + ": unknown run " + id);
        return run_state(sqlite3_column_int(query.get(), 0));
    }

    Database database;
    app::BuildInfo build;
    mutable std::mutex mutex;
    std::int64_t last_timestamp = 0;
};

SqliteRunStore::SqliteRunStore(const std::filesystem::path& path, app::BuildInfo build)
    : impl_(std::make_unique<Impl>(path, std::move(build))) {}

SqliteRunStore::~SqliteRunStore() = default;

app::RunId SqliteRunStore::create_run(const app::RunConfig& config) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->database.get(), "create run");
    const auto created_at = impl_->logical_timestamp();
    const auto id = Impl::run_id(created_at);

    Statement insert_run(
        impl_->database.get(),
        "INSERT INTO runs(id,draft,transport,mode,timeout_ms,state,created_at_unix_ns) "
        "VALUES(?,?,?,?,?,0,?)");
    insert_run.bind(1, id);
    insert_run.bind(2, enum_value(config.draft));
    insert_run.bind(3, enum_value(config.transport));
    insert_run.bind(4, enum_value(config.mode));
    insert_run.bind(5, static_cast<std::int64_t>(config.timeout.count()));
    insert_run.bind(6, created_at);
    insert_run.done("create run row");

    Statement insert_build(impl_->database.get(),
                           "INSERT INTO run_builds(run_id,version,source_revision) VALUES(?,?,?)");
    insert_build.bind(1, id);
    insert_build.bind(2, impl_->build.version);
    insert_build.bind(3, impl_->build.source_revision);
    insert_build.done("create run build identity");

    Statement insert_dependency(
        impl_->database.get(),
        "INSERT INTO build_dependencies(run_id,name,revision) VALUES(?,?,?)");
    for (const auto& [name, revision] : impl_->build.dependencies) {
        insert_dependency.bind(1, id);
        insert_dependency.bind(2, name);
        insert_dependency.bind(3, revision);
        insert_dependency.done("create run build dependency");
        insert_dependency.reset();
    }

    Statement insert_scenario(
        impl_->database.get(),
        "INSERT INTO selected_scenarios(run_id,position,scenario_id) VALUES(?,?,?)");
    for (std::size_t position = 0; position < config.scenario_ids.size(); ++position) {
        insert_scenario.bind(1, id);
        insert_scenario.bind(2, static_cast<std::int64_t>(position));
        insert_scenario.bind(3, config.scenario_ids[position]);
        insert_scenario.done("create run selected scenario");
        insert_scenario.reset();
    }

    Statement insert_capabilities(
        impl_->database.get(),
        "INSERT INTO run_publisher_capabilities(run_id,fetch) VALUES(?,?)");
    insert_capabilities.bind(1, id);
    insert_capabilities.bind(2, config.publisher_capabilities.fetch ? 1 : 0);
    insert_capabilities.done("create run publisher capabilities");

    if (config.track_fixture) {
        Statement insert_fixture(
            impl_->database.get(),
            "INSERT INTO run_track_fixtures(run_id,track_name) VALUES(?,?)");
        insert_fixture.bind(1, id);
        insert_fixture.bind(2, config.track_fixture->track_name);
        insert_fixture.done("create run track fixture");
        Statement insert_field(
            impl_->database.get(),
            "INSERT INTO run_track_namespace_fields(run_id,position,value) "
            "VALUES(?,?,?)");
        for (std::size_t position = 0;
             position < config.track_fixture->namespace_fields.size(); ++position) {
            const auto& field = config.track_fixture->namespace_fields[position];
            if (field.empty()) {
                throw std::invalid_argument("track namespace fields must be nonempty");
            }
            insert_field.bind(1, id);
            insert_field.bind(2, static_cast<std::int64_t>(position));
            insert_field.bind(3, field);
            insert_field.done("create run track namespace field");
            insert_field.reset();
        }
    }

    transaction.commit();
    return id;
}

void SqliteRunStore::append_events(const app::RunId& id,
                                   std::span<const EvidenceEvent> events) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->require_run(id, "append evidence") == RunState::Finalized) {
        throw std::logic_error("append evidence: run is finalized");
    }
    if (events.empty()) return;

    Transaction transaction(impl_->database.get(), "append evidence");
    if (impl_->require_run(id, "append evidence") == RunState::Finalized) {
        throw std::logic_error("append evidence: run is finalized");
    }
    Statement sequence_query(
        impl_->database.get(),
        "SELECT COALESCE(MAX(sequence), -1) FROM evidence_events WHERE run_id=?");
    sequence_query.bind(1, id);
    if (!sequence_query.row()) {
        throw std::runtime_error("append evidence: sequence query returned no row");
    }
    auto sequence = sqlite3_column_int64(sequence_query.get(), 0) + 1;

    Statement insert(
        impl_->database.get(),
        "INSERT INTO evidence_events("
        "run_id,sequence,monotonic_time_ns,wall_time_unix_ns,kind,detail,connection_id,"
        "stream_id,request_id,scenario_id,requirement_id) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    for (const auto& event : events) {
        insert.bind(1, id);
        insert.bind(2, static_cast<std::int64_t>(sequence++));
        insert.bind(3, event.monotonic_time_ns);
        insert.bind(4, event.wall_time_unix_ns);
        insert.bind(5, event.kind);
        insert.bind(6, event.detail);
        insert.bind_optional(7, event.connection_id);
        insert.bind_optional(8, event.stream_id);
        insert.bind_optional(9, event.request_id);
        insert.bind_optional(10, event.scenario_id);
        insert.bind_optional(11, event.requirement_id);
        insert.done("append evidence event");
        insert.reset();
    }
    transaction.commit();
}

void SqliteRunStore::finalize(const app::RunId& id,
                              const requirements::ScoreSummary& score,
                              std::span<const requirements::Outcome> outcomes) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->database.get(), "finalize run");
    if (impl_->require_run(id, "finalize run") == RunState::Finalized) {
        throw std::logic_error("finalize run: run is already finalized");
    }
    Statement insert_score(
        impl_->database.get(),
        "INSERT INTO final_scores("
        "run_id,verdict,required_earned,required_possible,weighted_earned,weighted_possible,"
        "coverage_earned,coverage_possible) VALUES(?,?,?,?,?,?,?,?)");
    insert_score.bind(1, id);
    insert_score.bind(2, enum_value(score.verdict));
    insert_score.bind(3, std::to_string(score.required.earned));
    insert_score.bind(4, std::to_string(score.required.possible));
    insert_score.bind(5, std::to_string(score.weighted.earned));
    insert_score.bind(6, std::to_string(score.weighted.possible));
    insert_score.bind(7, std::to_string(score.coverage.earned));
    insert_score.bind(8, std::to_string(score.coverage.possible));
    insert_score.done("finalize run score");

    Statement update_run(
        impl_->database.get(),
        "UPDATE runs SET state=1, finalized_at_unix_ns=? WHERE id=? AND state=0");
    update_run.bind(1, impl_->logical_timestamp());
    update_run.bind(2, id);
    update_run.done("finalize run lifecycle");
    if (sqlite3_changes(impl_->database.get()) != 1) {
        throw std::logic_error("finalize run: lifecycle changed concurrently");
    }

    Statement insert_outcome(
        impl_->database.get(),
        "INSERT INTO outcomes(run_id,position,requirement_id,state) VALUES(?,?,?,?)");
    for (std::size_t position = 0; position < outcomes.size(); ++position) {
        insert_outcome.bind(1, id);
        insert_outcome.bind(2, static_cast<std::int64_t>(position));
        insert_outcome.bind(3, outcomes[position].requirement_id);
        insert_outcome.bind(4, enum_value(outcomes[position].state));
        insert_outcome.done("finalize run outcome");
        insert_outcome.reset();
    }
    transaction.commit();
}

std::size_t SqliteRunStore::recover_interrupted() {
    std::size_t recovered = 0;
    for (std::size_t offset = 0;; offset += kMaximumPageSize) {
        const auto page = list({kMaximumPageSize, offset});
        for (const auto& summary : page.items) {
            if (summary.state != RunState::Active) continue;
            const auto record = load(summary.id);
            const bool has_marker = std::any_of(record.events.begin(), record.events.end(),
                [](const auto& event) { return event.kind == "runner_recovery"; });
            if (!has_marker) {
                EvidenceEvent marker;
                marker.kind = "runner_recovery";
                marker.detail = "runner restarted while this run was active";
                marker.wall_time_unix_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                append_events(summary.id, std::span(&marker, 1));
            }
            const requirements::ScoreSummary error{
                requirements::RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
            finalize(summary.id, error, {});
            ++recovered;
        }
        if (!page.next_offset) break;
    }
    return recovered;
}

RunRecord SqliteRunStore::load(const app::RunId& id) const {
    std::lock_guard lock(impl_->mutex);
    Statement run(
        impl_->database.get(),
        "SELECT draft,transport,mode,timeout_ms,state,created_at_unix_ns,finalized_at_unix_ns "
        "FROM runs WHERE id=?");
    run.bind(1, id);
    if (!run.row()) throw std::out_of_range("load run: unknown run " + id);

    RunRecord record{
        id,
        {draft_version(sqlite3_column_int(run.get(), 0)),
         transport_kind(sqlite3_column_int(run.get(), 1)),
         run_mode(sqlite3_column_int(run.get(), 2)),
         {},
         std::chrono::milliseconds(sqlite3_column_int64(run.get(), 3))},
        {},
        run_state(sqlite3_column_int(run.get(), 4)),
        sqlite3_column_int64(run.get(), 5),
        optional_int64(run.get(), 6),
        std::nullopt,
        {},
        {},
    };

    Statement build(impl_->database.get(),
                    "SELECT version,source_revision FROM run_builds WHERE run_id=?");
    build.bind(1, id);
    if (!build.row()) throw std::runtime_error("load run: build identity is missing");
    record.build.version = text(build.get(), 0);
    record.build.source_revision = text(build.get(), 1);

    Statement dependencies(
        impl_->database.get(),
        "SELECT name,revision FROM build_dependencies WHERE run_id=? ORDER BY name");
    dependencies.bind(1, id);
    while (dependencies.row()) {
        record.build.dependencies.emplace(text(dependencies.get(), 0), text(dependencies.get(), 1));
    }

    Statement scenarios(
        impl_->database.get(),
        "SELECT scenario_id FROM selected_scenarios WHERE run_id=? ORDER BY position");
    scenarios.bind(1, id);
    while (scenarios.row()) record.config.scenario_ids.push_back(text(scenarios.get(), 0));
    record.config.track_fixture = read_track_fixture(impl_->database.get(), id);
    record.config.publisher_capabilities = read_publisher_capabilities(impl_->database.get(), id);

    Statement score(
        impl_->database.get(),
        "SELECT verdict,required_earned,required_possible,weighted_earned,weighted_possible,"
        "coverage_earned,coverage_possible FROM final_scores WHERE run_id=?");
    score.bind(1, id);
    if (score.row()) {
        record.score = requirements::ScoreSummary{
            run_verdict(sqlite3_column_int(score.get(), 0)),
            {unsigned_integer(score.get(), 1), unsigned_integer(score.get(), 2)},
            {unsigned_integer(score.get(), 3), unsigned_integer(score.get(), 4)},
            {unsigned_integer(score.get(), 5), unsigned_integer(score.get(), 6)},
        };
    }

    Statement outcomes(
        impl_->database.get(),
        "SELECT requirement_id,state FROM outcomes WHERE run_id=? ORDER BY position");
    outcomes.bind(1, id);
    while (outcomes.row()) {
        record.outcomes.push_back(
            {text(outcomes.get(), 0), outcome_state(sqlite3_column_int(outcomes.get(), 1))});
    }

    Statement events(
        impl_->database.get(),
        "SELECT sequence,monotonic_time_ns,wall_time_unix_ns,kind,detail,connection_id,"
        "stream_id,request_id,scenario_id,requirement_id FROM evidence_events "
        "WHERE run_id=? ORDER BY sequence");
    events.bind(1, id);
    while (events.row()) {
        record.events.push_back({
            static_cast<std::uint64_t>(sqlite3_column_int64(events.get(), 0)),
            sqlite3_column_int64(events.get(), 1),
            sqlite3_column_int64(events.get(), 2),
            text(events.get(), 3),
            text(events.get(), 4),
            optional_text(events.get(), 5),
            optional_text(events.get(), 6),
            optional_text(events.get(), 7),
            optional_text(events.get(), 8),
            optional_text(events.get(), 9),
        });
    }
    return record;
}

Page<RunSummary> SqliteRunStore::list(RunQuery query) const {
    validate_query(query);
    std::lock_guard lock(impl_->mutex);

    Statement count(impl_->database.get(), "SELECT COUNT(*) FROM runs");
    if (!count.row()) throw std::runtime_error("list runs: count query returned no row");
    const auto total = static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));

    Page<RunSummary> page{{}, query.limit, query.offset, total, std::nullopt};
    Statement runs(
        impl_->database.get(),
        "SELECT r.id,r.draft,r.transport,r.mode,r.timeout_ms,r.state,r.created_at_unix_ns,"
        "r.finalized_at_unix_ns,s.verdict,s.required_earned,s.required_possible,"
        "s.weighted_earned,s.weighted_possible,s.coverage_earned,s.coverage_possible "
        "FROM runs r LEFT JOIN final_scores s ON s.run_id=r.id "
        "ORDER BY r.created_at_unix_ns DESC,r.id DESC LIMIT ? OFFSET ?");
    runs.bind(1, static_cast<std::int64_t>(query.limit));
    runs.bind(2, static_cast<std::int64_t>(query.offset));
    while (runs.row()) {
        RunSummary summary{
            text(runs.get(), 0),
            {draft_version(sqlite3_column_int(runs.get(), 1)),
             transport_kind(sqlite3_column_int(runs.get(), 2)),
             run_mode(sqlite3_column_int(runs.get(), 3)),
             {},
             std::chrono::milliseconds(sqlite3_column_int64(runs.get(), 4))},
            run_state(sqlite3_column_int(runs.get(), 5)),
            sqlite3_column_int64(runs.get(), 6),
            optional_int64(runs.get(), 7),
            std::nullopt,
            std::nullopt,
        };
        if (sqlite3_column_type(runs.get(), 8) != SQLITE_NULL) {
            const auto verdict = run_verdict(sqlite3_column_int(runs.get(), 8));
            summary.verdict = verdict;
            summary.score = requirements::ScoreSummary{
                verdict,
                {unsigned_integer(runs.get(), 9), unsigned_integer(runs.get(), 10)},
                {unsigned_integer(runs.get(), 11), unsigned_integer(runs.get(), 12)},
                {unsigned_integer(runs.get(), 13), unsigned_integer(runs.get(), 14)},
            };
        }

        Statement scenarios(
            impl_->database.get(),
            "SELECT scenario_id FROM selected_scenarios WHERE run_id=? ORDER BY position");
        scenarios.bind(1, summary.id);
        while (scenarios.row()) summary.config.scenario_ids.push_back(text(scenarios.get(), 0));
        summary.config.track_fixture = read_track_fixture(impl_->database.get(), summary.id);
        summary.config.publisher_capabilities =
            read_publisher_capabilities(impl_->database.get(), summary.id);
        page.items.push_back(std::move(summary));
    }
    page.next_offset = next_offset(query, page.items.size(), total);
    return page;
}

Page<EvidenceEvent> SqliteRunStore::list_events(const app::RunId& id,
                                                 RunQuery query) const {
    validate_query(query);
    std::lock_guard lock(impl_->mutex);
    impl_->require_run(id, "list evidence");

    Statement count(impl_->database.get(),
                    "SELECT COUNT(*) FROM evidence_events WHERE run_id=?");
    count.bind(1, id);
    if (!count.row()) throw std::runtime_error("list evidence: count query returned no row");
    const auto total = static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));

    Page<EvidenceEvent> page{{}, query.limit, query.offset, total, std::nullopt};
    Statement events(
        impl_->database.get(),
        "SELECT sequence,monotonic_time_ns,wall_time_unix_ns,kind,detail,connection_id,"
        "stream_id,request_id,scenario_id,requirement_id FROM evidence_events "
        "WHERE run_id=? ORDER BY sequence LIMIT ? OFFSET ?");
    events.bind(1, id);
    events.bind(2, static_cast<std::int64_t>(query.limit));
    events.bind(3, static_cast<std::int64_t>(query.offset));
    while (events.row()) {
        page.items.push_back({
            static_cast<std::uint64_t>(sqlite3_column_int64(events.get(), 0)),
            sqlite3_column_int64(events.get(), 1),
            sqlite3_column_int64(events.get(), 2),
            text(events.get(), 3),
            text(events.get(), 4),
            optional_text(events.get(), 5),
            optional_text(events.get(), 6),
            optional_text(events.get(), 7),
            optional_text(events.get(), 8),
            optional_text(events.get(), 9),
        });
    }
    page.next_offset = next_offset(query, page.items.size(), total);
    return page;
}

int SqliteRunStore::schema_version() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->schema_version_unlocked();
}

bool SqliteRunStore::foreign_keys_enabled() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->foreign_keys_enabled_unlocked();
}

}  // namespace moq::interop::storage
