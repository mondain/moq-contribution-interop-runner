#pragma once

#include "moq/interop/requirements/catalog.h"

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

}  // namespace moq::interop::requirements
