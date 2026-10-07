#pragma once

#include "moq/interop/requirements/catalog.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace moq::interop::requirements {

enum class OutcomeState { Pass, Fail, NotRun, NotTestable, NotApplicable };
enum class RunVerdict { Pass, Fail, Incomplete, Error };

struct Outcome {
    std::string requirement_id;
    OutcomeState state;
};

struct ScoreRatio {
    std::uint64_t earned;
    std::uint64_t possible;
};

struct ScoreSummary {
    RunVerdict verdict;
    ScoreRatio required;
    ScoreRatio weighted;
    ScoreRatio coverage;
};

std::uint64_t score_weight(Strength strength);
ScoreSummary score(const RequirementCatalog& catalog, std::span<const Outcome> outcomes);

// Scores an INCOMPLETE catalog (catalog.complete == false). Rows with reviewed == false must have
// exactly one NotRun outcome; they count in the required/weighted/coverage denominators by their
// strength (required only for Must/MustNot) and never earn. Reviewed rows follow the same rules and
// invariants as score(). Verdict: Fail if a required reviewed row failed; otherwise Incomplete
// (never Pass); Error on any invalid input (unknown id, duplicate/missing outcome, wrong state for
// a row kind, complete catalog passed in).
ScoreSummary score_staged(const RequirementCatalog& catalog, std::span<const Outcome> outcomes);

struct StagedCounts {
    std::size_t rows;
    std::size_t reviewed;
    std::size_t unreviewed;
    std::size_t unreviewed_required;
};
StagedCounts staged_counts(const RequirementCatalog& catalog);

}  // namespace moq::interop::requirements
