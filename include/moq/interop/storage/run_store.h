#pragma once

#include "moq/interop/app/types.h"
#include "moq/interop/app/version.h"
#include "moq/interop/requirements/scoring.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::storage {

enum class RunState { Active, Finalized };

struct EvidenceEvent {
    std::uint64_t sequence = 0;
    std::int64_t monotonic_time_ns = 0;
    std::int64_t wall_time_unix_ns = 0;
    std::string kind;
    std::string detail;
    std::optional<std::string> connection_id;
    std::optional<std::string> stream_id;
    std::optional<std::string> request_id;
    std::optional<std::string> scenario_id;
    std::optional<std::string> requirement_id;
};

struct RunRecord {
    app::RunId id;
    app::RunConfig config;
    app::BuildInfo build;
    RunState state;
    std::int64_t created_at_unix_ns;
    std::optional<std::int64_t> finalized_at_unix_ns;
    std::optional<requirements::ScoreSummary> score;
    std::vector<requirements::Outcome> outcomes;
    std::vector<EvidenceEvent> events;
};

struct RunSummary {
    app::RunId id;
    app::RunConfig config;
    RunState state;
    std::int64_t created_at_unix_ns;
    std::optional<std::int64_t> finalized_at_unix_ns;
    std::optional<requirements::RunVerdict> verdict;
    std::optional<requirements::ScoreSummary> score;
};

struct RunQuery {
    std::size_t limit = 50;
    std::size_t offset = 0;
};

template <typename T>
struct Page {
    std::vector<T> items;
    std::size_t limit;
    std::size_t offset;
    std::size_t total;
    std::optional<std::size_t> next_offset;
};

class RunStore {
public:
    virtual app::RunId create_run(const app::RunConfig& config) = 0;
    virtual void append_events(const app::RunId& id,
                               std::span<const EvidenceEvent> events) = 0;
    virtual void finalize(const app::RunId& id,
                          const requirements::ScoreSummary& score,
                          std::span<const requirements::Outcome> outcomes) = 0;
    virtual RunRecord load(const app::RunId& id) const = 0;
    virtual Page<RunSummary> list(RunQuery query) const = 0;
    virtual Page<EvidenceEvent> list_events(const app::RunId& id,
                                            RunQuery query) const = 0;
    virtual ~RunStore() = default;
};

class SqliteRunStore final : public RunStore {
public:
    SqliteRunStore(const std::filesystem::path& path, app::BuildInfo build);
    ~SqliteRunStore() override;

    SqliteRunStore(const SqliteRunStore&) = delete;
    SqliteRunStore& operator=(const SqliteRunStore&) = delete;
    SqliteRunStore(SqliteRunStore&&) = delete;
    SqliteRunStore& operator=(SqliteRunStore&&) = delete;

    app::RunId create_run(const app::RunConfig& config) override;
    void append_events(const app::RunId& id,
                       std::span<const EvidenceEvent> events) override;
    void finalize(const app::RunId& id,
                  const requirements::ScoreSummary& score,
                  std::span<const requirements::Outcome> outcomes) override;
    RunRecord load(const app::RunId& id) const override;
    Page<RunSummary> list(RunQuery query) const override;
    Page<EvidenceEvent> list_events(const app::RunId& id,
                                    RunQuery query) const override;

    int schema_version() const;
    bool foreign_keys_enabled() const;
    // Call once at service startup, before accepting new runs.
    std::size_t recover_interrupted();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::storage
