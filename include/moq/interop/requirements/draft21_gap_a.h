#pragma once

// Draft-21 completeness-gap slice A: executable bindings for rows whose
// scenarios are executed by the announcement controller or by the raw probe
// family in scenarios/draft21_gap_a.h.

#include "moq/interop/requirements/completeness.h"

#include <vector>

namespace moq::interop::requirements {

std::vector<ExecutableBinding> draft21_gap_a_bindings();

}  // namespace moq::interop::requirements
