#pragma once

#include "moq/interop/requirements/completeness.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::requirements {

// The shared half of draft 22's bindings: every draft 21 binding whose row, scenario and evaluator all have a
// shared draft 22 counterpart (lineage_data::kSharedRows, kSharedScenarios, kSharedEvaluators), translated with
// draft = 22, ids d21 -> d22 and evidence kinds unchanged. A draft 21 id with several draft 22 counterparts yields
// one binding per counterpart; one-to-many rows (several draft 21 rows onto one draft 22 row) collapse to one
// binding per (row, scenario, evaluator), never doubled. Deterministic: sorted by (row, scenario, evaluator).
std::vector<ExecutableBinding> draft22_shared_bindings();

// A draft 21 binding that was NOT translated and why. Reasons, in priority order:
//   "own_row"       its row's draft 22 successor is an own row (kDraft21AncestorsOfOwnRows22);
//   "dropped_row"   its row has no draft 22 successor at all;
//   "own_scenario"  its scenario has no shared draft 22 counterpart;
//   "own_evaluator" its evaluator has no shared draft 22 counterpart.
struct UntranslatedBinding {
    ExecutableBinding draft21;
    std::string reason;
};
std::vector<UntranslatedBinding> draft22_untranslated_bindings();

// The translation behind the two functions above, over any draft 21 binding list (tests use synthetic lists to
// exercise the one-to-many collapse that the real bindings do not currently reach).
struct Draft22Translation {
    std::vector<ExecutableBinding> shared;
    std::vector<UntranslatedBinding> untranslated;
};
Draft22Translation translate_draft21_bindings(std::span<const ExecutableBinding> draft21);

// Shared plus own bindings. The own half arrives with the own-row table; until then this equals the shared half.
std::vector<ExecutableBinding> draft22_executable_bindings();

// Draft 21 rows whose draft 22 successor (requirements/draft21-to-22-delta.json) is an own row of
// lineage_data::kOwnRows22, so their draft 21 bindings are replaced by own bindings rather than translated.
// Sorted; a test checks it against the delta file.
inline constexpr std::array<std::string_view, 8> kDraft21AncestorsOfOwnRows22{{
    "D21-2-3-1-SHOULD-NOT-025",
    "D21-3-3-1-MUST-NOT-057",
    "D21-4-2-MAY-088",
    "D21-4-3-1-MAY-101",
    "D21-6-3-MAY-145",
    "D21-9-20-10-MAY-430",
    "D21-9-20-10-MAY-431",
    "D21-9-20-10-MUST-432",
}};

}  // namespace moq::interop::requirements
