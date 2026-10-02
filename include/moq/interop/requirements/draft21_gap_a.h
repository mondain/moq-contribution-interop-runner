#pragma once

// Draft-21 completeness-gap slice A: executable bindings and announcement-run
// evaluation for rows whose scenarios run on the announcement controller or on
// the raw probe family in scenarios/draft21_gap_a.h.

#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/draft21_announcement.h"

#include <optional>
#include <vector>

namespace moq::interop::requirements {

std::vector<ExecutableBinding> draft21_gap_a_bindings();

// Returns a state only for rows owned by slice A; other rows return nullopt so
// the caller falls through to the original announcement evaluation.
std::optional<OutcomeState> draft21_gap_a_announcement_state(
    const Requirement& row, const scenarios::Draft21AnnouncementContext& context);

}  // namespace moq::interop::requirements
