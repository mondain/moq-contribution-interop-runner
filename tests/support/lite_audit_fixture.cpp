// Writes run databases for tests/e2e/audit-cli-moqlite.sh: moq-lite-06 runs created by the production
// NativeRunManager (moq-lite-06 catalog, native QUIC) against the draft-conforming lite publisher stand-in
// (tests/support/lite_run_live.h).
//
//   moq-interop-lite-audit-fixture CLEAN_DB TAMPERED_DB
//
// CLEAN_DB gets two finalized moq-lite-06 runs: {l06-setup-stream} and {l06-setup-stream, l06-setup-server-role}.
// TAMPERED_DB gets one copy of the first run whose declared evidence for L06-3-1-MUST-014 was removed, so its passed
// row has no evaluator evidence.
#include "moq/interop/requirements/lite_evaluators.h"
#include "support/lite_run_live.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace moq::interop;
using namespace moq::interop::lite_live;

constexpr const char* kScenario = "l06-setup-stream";
constexpr const char* kRow = "L06-3-1-MUST-014";

bool passed(const storage::RunRecord& run, const std::string& row) {
    return std::any_of(run.outcomes.begin(), run.outcomes.end(), [&](const auto& outcome) {
        return outcome.requirement_id == row && outcome.state == requirements::OutcomeState::Pass;
    });
}

int fail(const std::string& message) {
    std::cerr << "lite-audit-fixture: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " CLEAN_DB TAMPERED_DB\n";
        return 2;
    }
    try {
        const app::BuildInfo build{"lite-audit-fixture", "test", {}};
        auto store = std::make_shared<storage::SqliteRunStore>(argv[1], build);
        app::NativeRunManager manager(catalog(18), catalog(21), store, manager_config(), catalog(22), catalog(106));
        std::vector<storage::RunRecord> runs;
        for (const std::vector<std::string>& ids : {std::vector<std::string>{kScenario},
                                                    std::vector<std::string>{kScenario, "l06-setup-server-role"}}) {
            const auto config = lite_config(ids, std::chrono::milliseconds(4000));
            const auto started = manager.start(config);
            if (started.status != app::RunStartStatus::Started) return fail("a lite run did not start");
            drive_contexts(store, started.id, started.endpoint.port, static_cast<unsigned>(ids.size()),
                           [](std::uint16_t port, unsigned) {
                               return std::make_unique<LiteQuicPublisher>(port, publisher_config());
                           });
            (void)manager.stop(started.id);
            const auto run = store->load(started.id);
            if (run.state != storage::RunState::Finalized || !passed(run, kRow))
                return fail(std::string("a lite run did not pass ") + kRow);
            runs.push_back(run);
        }

        // The tampered copy: the same configuration, score and outcomes, without the binding's evidence.
        const auto& first = runs.front();
        const auto bindings = requirements::lite_executable_bindings();
        const auto binding = std::find_if(bindings.begin(), bindings.end(), [](const auto& value) {
            return value.requirement_id == kRow && value.scenario_id == kScenario;
        });
        if (binding == bindings.end() || binding->evidence_kinds.empty())
            return fail("no evidence-declaring binding for the row");
        std::vector<storage::EvidenceEvent> kept;
        std::size_t removed = 0;
        for (const auto& event : first.events) {
            if (event.scenario_id == kScenario && event.kind == binding->evidence_kinds.front()) {
                ++removed;
                continue;
            }
            kept.push_back(event);
        }
        if (removed == 0) return fail("the run has no " + binding->evidence_kinds.front() + " evidence");
        storage::SqliteRunStore tampered(argv[2], build);
        const auto copy = tampered.create_run(first.config);
        tampered.append_events(copy, kept);
        tampered.finalize(copy, *first.score, first.outcomes);
        std::cout << "clean " << runs[0].id << ' ' << runs[1].id << "\ntampered " << copy << " without " << removed
                  << ' ' << binding->evidence_kinds.front() << '\n';
        return 0;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
