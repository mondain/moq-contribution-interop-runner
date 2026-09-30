#include "moq/interop/http/result_schema.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace moq::interop::http {
namespace {

std::string escape_name(std::string_view name) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char byte : name) {
        if (byte < 0x20 || byte > 0x7e || byte == '#' || byte == '%' ||
            byte == '\\') {
            result.push_back('%');
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 15]);
        } else {
            result.push_back(static_cast<char>(byte));
        }
    }
    return result;
}

struct ScenarioResult {
    bool applicable = false;
    bool failed = false;
    bool incomplete = false;
    std::size_t passed = 0;
    std::size_t failed_count = 0;
    std::size_t not_run = 0;
};

ScenarioResult summarize(const storage::RunRecord& run,
                         const requirements::RequirementCatalog& catalog,
                         std::string_view scenario) {
    std::map<std::string, std::vector<requirements::OutcomeState>> observations;
    for (const auto& outcome : run.outcomes)
        observations[outcome.requirement_id].push_back(outcome.state);
    ScenarioResult summary;
    for (const auto& requirement : catalog.requirements) {
        if (requirement.applicability != requirements::Applicability::Applicable ||
            requirement.testability != requirements::Testability::Testable ||
            std::find(requirement.scenarios.begin(), requirement.scenarios.end(),
                      scenario) == requirement.scenarios.end())
            continue;
        summary.applicable = true;
        const auto& states = observations[requirement.id];
        if (std::find(states.begin(), states.end(),
                      requirements::OutcomeState::Fail) != states.end()) {
            summary.failed = true;
            ++summary.failed_count;
        } else if (std::find(states.begin(), states.end(),
                             requirements::OutcomeState::Pass) != states.end()) {
            ++summary.passed;
        } else {
            summary.incomplete = true;
            ++summary.not_run;
        }
    }
    return summary;
}

}  // namespace

std::string serialize_tap14(const storage::RunRecord& run,
                            const requirements::RequirementCatalog& catalog) {
    if (static_cast<unsigned>(run.config.draft) != catalog.draft)
        throw std::invalid_argument("run and catalog drafts differ");
    std::ostringstream out;
    out << "TAP version 14\n";
    if (run.config.scenario_ids.empty())
        return "TAP version 14\n1..0 # SKIP no scenarios selected\n";
    out << "1.." << run.config.scenario_ids.size() << '\n';
    for (std::size_t index = 0; index < run.config.scenario_ids.size(); ++index) {
        const auto& scenario = run.config.scenario_ids[index];
        const auto summary = summarize(run, catalog, scenario);
        const bool error = run.state != storage::RunState::Finalized ||
                           !run.score ||
                           run.score->verdict == requirements::RunVerdict::Error;
        const bool passed = !error && !summary.failed &&
                            !summary.incomplete && summary.applicable;
        const bool skipped = !error && !summary.applicable;
        out << (passed || skipped ? "ok " : "not ok ") << index + 1
            << " - " << escape_name(scenario);
        if (skipped) out << " # SKIP no scored requirements";
        out << '\n';
        const char* result = error ? "error" :
                             summary.failed ? "fail" :
                             summary.incomplete ? "incomplete" :
                             skipped ? "skip" : "pass";
        const nlohmann::json diagnostic{
            {"run_id", run.id}, {"draft", catalog.draft},
            {"scenario_id", scenario}, {"result", result},
            {"passed", summary.passed}, {"failed", summary.failed_count},
            {"not_run", summary.not_run}};
        out << "  ---\n  " << diagnostic.dump() << "\n  ...\n";
    }
    return out.str();
}

}  // namespace moq::interop::http
