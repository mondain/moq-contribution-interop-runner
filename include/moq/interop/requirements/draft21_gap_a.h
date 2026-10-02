#pragma once

// Draft-21 completeness-gap slice A: executable bindings and announcement-run
// evaluation for rows whose scenarios run on the announcement controller or on
// the raw probe family in scenarios/draft21_gap_a.h.

#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/draft21_announcement.h"
#include "moq/interop/scenarios/draft21_gap_a.h"
#include "moq/interop/scenarios/draft21_gap_a_token.h"

#include <optional>
#include <string>
#include <vector>

namespace moq::interop::requirements {

std::vector<ExecutableBinding> draft21_gap_a_bindings();

// Returns a state only for rows owned by slice A; other rows return nullopt so
// the caller falls through to the original announcement evaluation.
std::optional<OutcomeState> draft21_gap_a_announcement_state(
    const Requirement& row, const scenarios::Draft21AnnouncementContext& context);

// Rows whose named scenarios are per-transport alternatives: each run executes
// only the scenario for its own transport, so one passing scenario settles the
// row for that run (scores are never combined across transports).
bool draft21_gap_a_scenarios_are_alternatives(const Requirement& row);

struct Draft21GapARawResult {
    bool passed;
    std::string evaluator;
};

// Evaluates one raw probe transcript for a slice A row, if the row owns it.
std::optional<Draft21GapARawResult> draft21_gap_a_raw_result(
    const Requirement& row, const scenarios::RawProbeTranscript& transcript,
    const std::vector<scenarios::Draft21GapProbe>& probes,
    const std::vector<scenarios::Draft21TokenProbe>& token_probes);

}  // namespace moq::interop::requirements
