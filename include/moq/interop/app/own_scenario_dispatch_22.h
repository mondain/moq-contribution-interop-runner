#pragma once

#include "moq/interop/app/own_scenarios_22.h"
#include "moq/interop/app/types.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/raw_probe.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace moq::interop::app {

// The probe of an implemented own draft 22 scenario (a registered one first, then production), built
// from the run's execution config; nothing for any other id. Call under ScopedWireDraft(22).
std::optional<scenarios::RawProbeDefinition> own_probe_22(std::string_view id, const RunConfig& execution);

// Whether own_probe_22 has a probe for `id` (without building it).
bool has_own_probe_22(std::string_view id);

// The ids of the production own evaluators (tests check them against kOwnEvaluators22).
std::vector<std::string_view> production_own_evaluator_ids_22();

// The own draft 22 evaluators' outcomes over a run's transcripts, keyed by draft 22 row ids: one outcome
// for every applicable, testable row of `draft22` that names an implemented own evaluator. A row passes
// when every scenario it names ran once with a verdict and every evaluator it names was exercised, and
// fails on any failing verdict (the rule evaluate_draft21_raw_probes applies to draft 21 rows).
std::vector<requirements::Outcome> evaluate_own_draft22(const requirements::RequirementCatalog& draft22,
                                                        std::span<const scenarios::RawProbeTranscript> transcripts);

}  // namespace moq::interop::app
