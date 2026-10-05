#pragma once

#include "moq/interop/requirements/scoring.h"

#include <span>
#include <vector>

namespace moq::interop::requirements {

// Maps outcomes produced by the draft 21 evaluators to draft 22 row ids through the generated lineage.
std::vector<Outcome> translate_shared_outcomes(std::span<const Outcome> draft21_outcomes);

}  // namespace moq::interop::requirements
