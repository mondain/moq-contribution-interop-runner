#pragma once

#include "moq/interop/requirements/lineage.h"

#include <set>
#include <string>
#include <vector>

namespace moq::interop::requirements {

// Hand-established equivalences the carry tool missed (page-break misses recorded in the delta notes).
std::vector<Equivalence> draft22_equivalences();

// Draft 21 scenarios whose implementation builds or inspects a LOCATION_FILTER parameter (the D2 audit).
// Draft 22 changed that parameter's encoding; all of them now build and read it in the run's wire draft.
std::set<std::string> draft21_location_filter_scenarios();

// The residual subset of draft21_location_filter_scenarios() that is `own` in draft 22 regardless of its
// rows: its draft 21 expectation is a draft 21 filter fact, or it cannot run under wire draft 22.
std::set<std::string> draft22_filter_building_scenarios();

}  // namespace moq::interop::requirements
