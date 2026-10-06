#pragma once

#include "moq/interop/app/draft_traits.h"
#include "moq/interop/app/lineage.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/app/types.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/scoring.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::app {

// What the scenario layer sees for a run, and the draft the run really is on the wire.
// `execution` drives scenario behavior (family draft, implementation scenario ids); `wire_draft`
// is the run's identity: its ALPN, its stored row and the catalog it is scored against.
struct LineageRun {
    RunConfig execution;
    DraftVersion wire_draft;
};

// Drafts 18 and 21 come back unchanged. A draft 22 run executes on draft 21's family with each
// shared scenario's draft 21 implementation; nothing if any selected id is not a shared draft 22
// scenario whose implementation is executable.
inline std::optional<LineageRun> lineage_run(const RunConfig& config) {
    if (config.draft != DraftVersion::Draft22) return LineageRun{config, config.draft};
    LineageRun run{config, DraftVersion::Draft22};
    run.execution.draft = family_draft(config.draft);
    run.execution.scenario_ids.clear();
    for (const auto& id : config.scenario_ids) {
        const auto implementation = implementation_scenario_id(id);
        if (!implementation || !executable_scenario(21, *implementation)) return std::nullopt;
        run.execution.scenario_ids.emplace_back(*implementation);
    }
    return run;
}

// The outcomes a draft 22 lineage run stores: the draft 21 evaluators' outcomes translated to draft 22
// rows, then the own draft 22 evaluators' outcomes (already keyed by draft 22 rows) for rows the
// translation does not reach, plus one outcome for every row neither reaches (NotRun when scored,
// otherwise the row's own NotTestable/NotApplicable class). One outcome per catalog row, in catalog
// order; a translated or own outcome the catalog lacks is appended after the catalog rows (no severity
// merge), where scoring rejects it. An own outcome for a row the translation reached, or two own outcomes
// for one row, throws std::logic_error: own rows (kOwnRows22) name only own scenarios and evaluators, which
// LineageRunOwn.RowsNamingOwnIdsAreOwnRowsAndNameOnlyOwnIds guards on the catalog data.
std::vector<requirements::Outcome> lineage_outcomes(const requirements::RequirementCatalog& draft22,
                                                    std::span<const requirements::Outcome> draft21_outcomes,
                                                    std::span<const requirements::Outcome> own_outcomes = {});

// The requested (draft 22) scenario id for a draft 21 implementation id of a lineage run: the reverse of
// kSharedScenarios. Identity for any run whose wire draft is not 22 and, on draft 22, for ids that are not
// draft 21 ids (draft 22 ids: shared, own scenarios and unscored probes). Throws std::logic_error naming
// the id for a draft 21 id of a draft 22 run that has no reverse mapping.
std::string stamp_scenario_id(DraftVersion wire_draft, std::string_view scenario_id);

}  // namespace moq::interop::app
