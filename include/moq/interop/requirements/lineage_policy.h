#pragma once

#include "moq/interop/requirements/lineage.h"

#include <set>
#include <string>
#include <vector>

namespace moq::interop::requirements {

// Hand-established equivalences the carry tool missed (page-break misses recorded in the delta notes).
std::vector<Equivalence> draft22_equivalences();

// Draft 21 scenarios whose implementation builds or inspects a LOCATION_FILTER parameter. Draft 22
// changed that parameter's encoding, so these are `own` regardless of their rows.
std::set<std::string> draft22_filter_building_scenarios();

}  // namespace moq::interop::requirements
