#include "json.h"
#include "moq/interop/http/result_schema.h"

#include "moq/interop/app/publisher_capabilities.h"

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
        const auto& states = observations[requirement.id];
        // Declared not applicable for this run: neither passed, failed nor outstanding.
        if (states.size() == 1 && states.front() == requirements::OutcomeState::NotApplicable)
            continue;
        summary.applicable = true;
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
    // A staged (moq-lite) catalog: say so before any point, as the run verdict is never pass.
    if (detail::staged_catalog(catalog))
        out << "# staged catalog: " << detail::kStagedNote << '\n';
    if (!run.config.publisher_capabilities.fetch)
        out << "# publisher_capabilities fetch=false (declared: the publisher does not implement FETCH)\n";
    for (std::size_t index = 0; index < run.config.scenario_ids.size(); ++index) {
        const auto& scenario = run.config.scenario_ids[index];
        const auto summary = summarize(run, catalog, scenario);
        // A scenario the publisher's declaration ruled out was never started.
        const auto declared_skip = app::scenario_skip_reason(
            catalog.draft, scenario, run.config.publisher_capabilities);
        const bool error = run.state != storage::RunState::Finalized ||
                           !run.score ||
                           run.score->verdict == requirements::RunVerdict::Error;
        const bool passed = !error && !summary.failed &&
                            !summary.incomplete && summary.applicable;
        const bool skipped = declared_skip.has_value() || (!error && !summary.applicable);
        out << (passed || skipped ? "ok " : "not ok ") << index + 1
            << " - " << escape_name(scenario);
        if (declared_skip) out << " # SKIP " << escape_name(*declared_skip);
        else if (skipped) out << " # SKIP no scored requirements";
        out << '\n';
        const char* result = declared_skip ? "skip" : error ? "error" :
                             summary.failed ? "fail" :
                             summary.incomplete ? "incomplete" :
                             skipped ? "skip" : "pass";
        nlohmann::json diagnostic{
            {"run_id", run.id}, {"draft", detail::catalog_draft_json(catalog.draft)},
            {"scenario_id", scenario}, {"result", result},
            {"scoring_profile", std::any_of(run.events.begin(), run.events.end(), [](const auto& event) {
                return event.kind == "compatibility_error_mapping";
            }) ? "compatibility" : "standards"},
            {"passed", summary.passed}, {"failed", summary.failed_count},
            {"not_run", summary.not_run}};
        if (declared_skip) diagnostic["skip_reason"] = *declared_skip;
        out << "  ---\n  " << diagnostic.dump() << "\n  ...\n";
    }
    return out.str();
}

}  // namespace moq::interop::http
