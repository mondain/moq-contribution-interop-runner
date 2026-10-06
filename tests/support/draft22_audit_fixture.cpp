// Writes run databases for tests/e2e/audit-cli-draft22.sh: draft 22 runs created by the production
// NativeRunManager (draft 22 catalog, moqt-22) against picoquic publisher stand-ins.
//
//   moq-interop-draft22-audit-fixture CLEAN_DB TAMPERED_DB
//
// CLEAN_DB gets two finalized draft 22 runs: the shared raw pair {d22-fetch-accepted, d22-fetch-rejected} and
// the own scenario d22-request-stream-before-peer-setup. TAMPERED_DB gets one copy of the own run whose
// declared evidence for D22-6-3-MAY-159 was removed, so its passed row has no evaluator evidence.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/draft22_evaluators.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"
#include "support/draft22_publisher_scripts.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace moq::interop;

constexpr const char* kOwnScenario = "d22-request-stream-before-peer-setup";
constexpr const char* kOwnRow = "D22-6-3-MAY-159";

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, root / ("requirements/draft" + std::to_string(draft) + ".json")));
}

bool passed(const storage::RunRecord& run, const std::string& row) {
    return std::any_of(run.outcomes.begin(), run.outcomes.end(), [&](const auto& outcome) {
        return outcome.requirement_id == row && outcome.state == requirements::OutcomeState::Pass;
    });
}

int fail(const std::string& message) {
    std::cerr << "draft22-audit-fixture: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " CLEAN_DB TAMPERED_DB\n";
        return 2;
    }
    try {
        const app::BuildInfo build{"draft22-audit-fixture", "test", {}};
        auto store = std::make_shared<storage::SqliteRunStore>(argv[1], build);
        app::NativeRunManager manager(
            catalog(18), catalog(21), store,
            {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
             .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
             .certificate_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "cert.pem",
             .private_key_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "key.pem"},
            catalog(22));
        const app::TrackFixture fixture{{"n"}, "t"};

        const auto shared = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
            app::RunMode::Observed, {"d22-fetch-accepted", "d22-fetch-rejected"}, std::chrono::milliseconds(2000),
            fixture});
        if (shared.status != app::RunStartStatus::Started) return fail("shared run did not start");
        if (!d22pub::play_fetch_pair(*store, shared.id, shared.endpoint.port)) return fail("shared publisher failed");
        const auto shared_run = d22pub::finalized(*store, shared.id);
        if (shared_run.state != storage::RunState::Finalized || !passed(shared_run, "D22-3-2-MUST-057"))
            return fail("shared run did not pass D22-3-2-MUST-057");

        const auto own = manager.start({app::DraftVersion::Draft22, app::TransportKind::NativeQuic,
            app::RunMode::Observed, {kOwnScenario}, std::chrono::milliseconds(1500), fixture});
        if (own.status != app::RunStartStatus::Started) return fail("own run did not start");
        if (!d22pub::play_pre_setup_reset(*store, own.id, own.endpoint.port)) return fail("own publisher failed");
        const auto own_run = d22pub::finalized(*store, own.id);
        if (own_run.state != storage::RunState::Finalized || !passed(own_run, kOwnRow))
            return fail(std::string("own run did not pass ") + kOwnRow);

        // The tampered copy: the same configuration, score and outcomes, without the binding's evidence.
        const auto bindings = requirements::draft22_executable_bindings();
        const auto binding = std::find_if(bindings.begin(), bindings.end(), [](const auto& value) {
            return value.requirement_id == kOwnRow && value.scenario_id == kOwnScenario;
        });
        if (binding == bindings.end() || binding->evidence_kinds.empty())
            return fail("no evidence-declaring binding for the own row");
        std::vector<storage::EvidenceEvent> kept;
        std::size_t removed = 0;
        for (const auto& event : own_run.events) {
            if (event.scenario_id == kOwnScenario && event.kind == binding->evidence_kinds.front()) {
                ++removed;
                continue;
            }
            kept.push_back(event);
        }
        if (removed == 0) return fail("the own run has no " + binding->evidence_kinds.front() + " evidence");
        storage::SqliteRunStore tampered(argv[2], build);
        const auto copy = tampered.create_run(own_run.config);
        tampered.append_events(copy, kept);
        tampered.finalize(copy, *own_run.score, own_run.outcomes);
        std::cout << "clean " << shared.id << ' ' << own.id << "\ntampered " << copy << " without "
                  << removed << ' ' << binding->evidence_kinds.front() << '\n';
        return 0;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
