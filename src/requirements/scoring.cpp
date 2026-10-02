#include "moq/interop/requirements/scoring.h"

#include <cstdint>
#include <map>

namespace moq::interop::requirements {
namespace {

struct Observations {
    bool pass = false;
    bool fail = false;
    unsigned not_run = 0;
    unsigned not_testable = 0;
    unsigned not_applicable = 0;
};

constexpr ScoreSummary error_summary() {
    return {RunVerdict::Error, {0, 0}, {0, 0}, {0, 0}};
}

bool is_scored(const Requirement& requirement) {
    return requirement.applicability == Applicability::Applicable &&
           requirement.testability == Testability::Testable;
}

bool is_not_testable(const Requirement& requirement) {
    return requirement.applicability == Applicability::Applicable &&
           requirement.testability == Testability::NotTestable;
}

bool is_not_applicable(const Requirement& requirement) {
    return (requirement.applicability == Applicability::NotApplicable ||
            requirement.applicability == Applicability::Informative) &&
           requirement.testability == Testability::NotApplicable;
}

std::uint64_t weight(Strength strength) {
    switch (strength) {
        case Strength::Must:
        case Strength::MustNot:
            return 10;
        case Strength::Should:
        case Strength::ShouldNot:
            return 3;
        case Strength::May:
            return 1;
    }
    return 0;
}

bool is_required(Strength strength) {
    return strength == Strength::Must || strength == Strength::MustNot;
}

bool record(Observations& observations, OutcomeState state) {
    switch (state) {
        case OutcomeState::Pass:
            observations.pass = true;
            return true;
        case OutcomeState::Fail:
            observations.fail = true;
            return true;
        case OutcomeState::NotRun:
            ++observations.not_run;
            return true;
        case OutcomeState::NotTestable:
            ++observations.not_testable;
            return true;
        case OutcomeState::NotApplicable:
            ++observations.not_applicable;
            return true;
    }
    return false;
}

bool valid_scored_observations(const Observations& observations) {
    if (observations.not_testable != 0 || observations.not_applicable != 0) return false;
    if (observations.not_run != 0) {
        return observations.not_run == 1 && !observations.pass && !observations.fail;
    }
    return observations.pass || observations.fail;
}

bool exactly_not_testable(const Observations& observations) {
    return !observations.pass && !observations.fail && observations.not_run == 0 &&
           observations.not_testable == 1 && observations.not_applicable == 0;
}

bool exactly_not_applicable(const Observations& observations) {
    return !observations.pass && !observations.fail && observations.not_run == 0 &&
           observations.not_testable == 0 && observations.not_applicable == 1;
}

}  // namespace

std::uint64_t score_weight(Strength strength) { return weight(strength); }

ScoreSummary score(const RequirementCatalog& catalog, std::span<const Outcome> outcomes) {
    if (!catalog.complete) return error_summary();

    std::map<std::string, const Requirement*> requirements;
    for (const auto& requirement : catalog.requirements) {
        if (requirement.id.empty() || weight(requirement.strength) == 0 ||
            (!is_scored(requirement) && !is_not_testable(requirement) &&
             !is_not_applicable(requirement)) ||
            !requirements.emplace(requirement.id, &requirement).second) {
            return error_summary();
        }
    }

    std::map<std::string, Observations> observed;
    for (const auto& outcome : outcomes) {
        if (!requirements.contains(outcome.requirement_id) ||
            !record(observed[outcome.requirement_id], outcome.state)) {
            return error_summary();
        }
    }

    ScoreSummary summary{RunVerdict::Pass, {0, 0}, {0, 0}, {0, 0}};
    bool required_failed = false;
    bool incomplete = false;

    for (const auto& requirement : catalog.requirements) {
        const auto found = observed.find(requirement.id);
        if (found == observed.end()) return error_summary();
        const auto& observations = found->second;

        if (is_not_testable(requirement)) {
            if (!exactly_not_testable(observations)) return error_summary();
            continue;
        }
        if (is_not_applicable(requirement)) {
            if (!exactly_not_applicable(observations)) return error_summary();
            continue;
        }
        // A scored row the run declared not applicable (for example, every scenario it
        // names needs a feature the publisher declared it does not implement) leaves the
        // required, weighted and coverage denominators and cannot make the run incomplete.
        if (exactly_not_applicable(observations)) continue;
        if (!valid_scored_observations(observations)) return error_summary();

        const auto requirement_weight = weight(requirement.strength);
        const bool required = is_required(requirement.strength);
        summary.weighted.possible += requirement_weight;
        summary.coverage.possible += requirement_weight;
        if (required) summary.required.possible += requirement_weight;

        if (observations.not_run != 0) {
            incomplete = true;
            continue;
        }

        summary.coverage.earned += requirement_weight;
        if (observations.fail) {
            if (required) required_failed = true;
            continue;
        }

        summary.weighted.earned += requirement_weight;
        if (required) summary.required.earned += requirement_weight;
    }

    if (required_failed) {
        summary.verdict = RunVerdict::Fail;
    } else if (incomplete) {
        summary.verdict = RunVerdict::Incomplete;
    }
    return summary;
}

}  // namespace moq::interop::requirements
