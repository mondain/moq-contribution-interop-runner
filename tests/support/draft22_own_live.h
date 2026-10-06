#pragma once

// Shared plumbing of the live draft 22 own-scenario tests: a picoquic publisher stand-in plays against the
// production NativeRunManager on moqt-22, and the tests read the stored run.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/own_scenarios_22.h"
#include "moq/interop/requirements/draft_source.h"
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
#include <string_view>
#include <thread>
#include <vector>

namespace moq::interop::live22 {

using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
using Client = transport::test::PicoquicTestClient;

inline Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

inline Bytes alpn_of(std::string_view value) {
    Bytes result;
    for (const char byte : value) result.push_back(static_cast<std::byte>(byte));
    return result;
}

inline std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
    const auto path = root / ("requirements/draft" + std::to_string(draft) + ".json");
    if (draft == 22)
        return std::make_shared<const requirements::RequirementCatalog>(
            requirements::RequirementCatalog::load(source, path, requirements::CatalogLoadMode::AllowIncomplete));
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(source, path));
}

inline app::NativeRunManager manager_for(const std::shared_ptr<storage::SqliteRunStore>& store) {
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

// Whether the run's context `ordinal` (1-based) is listening.
inline bool context_started(const std::shared_ptr<storage::SqliteRunStore>& store, const app::RunId& id,
                            unsigned ordinal) {
    const auto suffix = " ordinal=" + std::to_string(ordinal);
    const auto run = store->load(id);
    return std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
        return event.kind == "context_ready" && event.detail.ends_with(suffix);
    });
}

inline bool context_ready(const std::shared_ptr<storage::SqliteRunStore>& store, const app::RunId& id,
                          unsigned ordinal = 1) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (context_started(store, id, ordinal)) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// Pumps `client` until the run is finalized or its context `ordinal` + 1 started (the runner may close the
// connection when the context ends; only the run's progress matters).
inline void pump_until_context_ends(Client& client, const std::shared_ptr<storage::SqliteRunStore>& store,
                                    const app::RunId& id, unsigned ordinal = 1) {
    (void)pump_until(client, [&] {
        return store->load(id).state == storage::RunState::Finalized || context_started(store, id, ordinal + 1);
    }, 10s);
    EXPECT_TRUE(store->load(id).state == storage::RunState::Finalized || context_started(store, id, ordinal + 1));
}

inline requirements::OutcomeState state_of(const storage::RunRecord& run, std::string_view id) {
    const auto found = std::find_if(run.outcomes.begin(), run.outcomes.end(),
                                    [&](const auto& outcome) { return outcome.requirement_id == id; });
    EXPECT_NE(found, run.outcomes.end()) << id;
    return found == run.outcomes.end() ? requirements::OutcomeState::NotRun : found->state;
}

inline bool harness_error(const storage::RunRecord& run) {
    return std::any_of(run.events.begin(), run.events.end(),
                       [](const auto& event) { return event.kind == "harness_error"; });
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

// The publisher's SETUP (Section 9.1): Type 0x2F00, Length 0.
inline Bytes setup() { return b({0xaf, 0, 0, 0}); }

// The server-initiated bidirectional stream of the runner's request `index`: 1, 5, 9, ...
constexpr std::uint64_t request_stream(std::size_t index) { return static_cast<std::uint64_t>(1 + 4 * index); }

}  // namespace moq::interop::live22
