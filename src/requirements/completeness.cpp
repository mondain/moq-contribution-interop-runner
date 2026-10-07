#include "moq/interop/requirements/completeness.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

namespace moq::interop::requirements {
namespace {

bool required(Strength strength) {
    return strength == Strength::Must || strength == Strength::MustNot;
}

bool listed(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool known_evidence_kind(std::string_view kind) {
    static const std::set<std::string_view> known{
        "transport_established", "local_setup_observed", "local_setup_sent",
        "peer_setup_received", "setup_option_duplicate", "peer_stream_classified",
        "request_observed",
        "initial_response_observed",
        "response_violation", "protocol_violation", "object_observed",
        "peer_close", "peer_closed", "local_close", "harness_limit",
        "namespace_observed", "namespace_response_delivered",
        "publish_observed", "response_delivered", "unsupported_stream",
        "invalid_request_opener", "raw_probe_stimulus", "raw_probe_transport_event",
        // moq-lite-06 session evidence (include/moq/interop/session/lite_session.h)
        "lite_stream_opened", "lite_message", "lite_decode_error"};
    return known.contains(kind);
}
struct BoundContexts {
    std::set<std::string> scenarios;
    std::set<std::string> evaluators;
};

}  // namespace

bool CompletenessReport::complete() const noexcept {
    return required_total == required_covered && unreviewed_total == 0 &&
           std::none_of(findings.begin(), findings.end(),
                        [](const auto& finding) { return finding.blocking; });
}

// Shared by audit_completeness() (staged == false) and audit_completeness_staged().
static CompletenessReport audit_rows(
    const RequirementCatalog& catalog,
    std::span<const ExecutableBinding> bindings,
    std::span<const std::string_view> executable_scenarios,
    bool staged) {
    CompletenessReport report{catalog.draft};
    if (!staged && !catalog.complete) {
        report.findings.push_back({"incomplete_catalog", "",
            "source-keyword catalog is not complete", true});
    }
    std::map<std::string, const Requirement*> rows;
    for (const auto& row : catalog.requirements) {
        rows.emplace(row.id, &row);
    }
    std::set<std::tuple<unsigned, std::string, std::string, std::string>> seen;
    std::map<std::string,BoundContexts> covered;
    for (const auto& binding : bindings) {
        if (binding.draft != catalog.draft) {
            report.findings.push_back({"wrong_draft_binding", binding.requirement_id,
                "binding belongs to a different draft", true});
            continue;
        }
        const auto found = rows.find(binding.requirement_id);
        if (found == rows.end()) {
            report.findings.push_back({"orphan_binding", binding.requirement_id,
                "binding names no catalog row", true});
            continue;
        }
        const auto key = std::tie(binding.draft, binding.requirement_id,
                                  binding.scenario_id, binding.evaluator_id);
        if (!seen.insert(key).second) {
            report.findings.push_back({"duplicate_binding", binding.requirement_id,
                "same executable binding is registered twice", true});
            continue;
        }
        const auto& row = *found->second;
        if (std::find(executable_scenarios.begin(), executable_scenarios.end(),
                      binding.scenario_id) == executable_scenarios.end()) {
            report.findings.push_back({"nonexecutable_scenario", binding.requirement_id,
                "binding scenario is not registered by the runner", true});
            continue;
        }
        if (staged && !row.reviewed) {
            report.findings.push_back({"mismatched_binding", binding.requirement_id,
                "binding names a row whose classification is still unreviewed", true});
            continue;
        }
        if (row.applicability != Applicability::Applicable ||
            row.testability != Testability::Testable ||
            !listed(row.scenarios, binding.scenario_id) ||
            !listed(row.evaluators, binding.evaluator_id)) {
            report.findings.push_back({"mismatched_binding", binding.requirement_id,
                "scenario/evaluator pair is not an applicable testable catalog obligation", true});
            continue;
        }
        if (binding.evidence_kinds.empty() ||
            std::any_of(binding.evidence_kinds.begin(), binding.evidence_kinds.end(),
                [](const auto& kind) { return !known_evidence_kind(kind); })) {
            report.findings.push_back({"missing_evidence_schema", binding.requirement_id,
                "binding declares no usable evidence kind", true});
            continue;
        }
        auto& contexts = covered[binding.requirement_id];
        contexts.scenarios.insert(binding.scenario_id);
        contexts.evaluators.insert(binding.evaluator_id);
    }
    for (const auto& row : catalog.requirements) {
        if (staged && !row.reviewed) {
            ++report.unreviewed_total;
            if (required(row.strength)) ++report.unreviewed_required;
            continue;
        }
        if (row.applicability != Applicability::Applicable ||
            row.testability != Testability::Testable) continue;
        const bool is_required = required(row.strength);
        if (is_required) ++report.required_total;
        else ++report.optional_total;
        const auto found = covered.find(row.id);
        const bool complete_contexts = found != covered.end() &&
            std::all_of(row.scenarios.begin(),row.scenarios.end(),[&](const auto& scenario) {
                return found->second.scenarios.contains(scenario);
            }) &&
            std::all_of(row.evaluators.begin(),row.evaluators.end(),[&](const auto& evaluator) {
                return found->second.evaluators.contains(evaluator);
            });
        if (complete_contexts) {
            if (is_required) ++report.required_covered;
            else ++report.optional_covered;
        } else {
            std::set<std::string> missing_scenarios(row.scenarios.begin(),row.scenarios.end());
            std::set<std::string> missing_evaluators(row.evaluators.begin(),row.evaluators.end());
            if (found != covered.end()) {
                for (const auto& id : found->second.scenarios) missing_scenarios.erase(id);
                for (const auto& id : found->second.evaluators) missing_evaluators.erase(id);
            }
            std::string detail = "not every named scenario and evaluator has an executable binding with evidence";
            for (const auto& id : missing_scenarios) detail += " missing_scenario=" + id;
            for (const auto& id : missing_evaluators) detail += " missing_evaluator=" + id;
            report.findings.push_back({is_required ? "missing_required_evaluator"
                                                   : "missing_optional_evaluator",
                row.id, std::move(detail), is_required && !staged});
        }
    }
    if (report.unreviewed_total != 0) {
        report.findings.push_back({"unreviewed_rows", "",
            std::to_string(report.unreviewed_total) + " catalog rows are unreviewed (" +
                std::to_string(report.unreviewed_required) + " required)", false});
    }
    std::sort(report.findings.begin(), report.findings.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.requirement_id, left.code, left.detail) <
                   std::tie(right.requirement_id, right.code, right.detail);
        });
    return report;
}

CompletenessReport audit_completeness(
    const RequirementCatalog& catalog,
    std::span<const ExecutableBinding> bindings,
    std::span<const std::string_view> executable_scenarios) {
    return audit_rows(catalog, bindings, executable_scenarios, false);
}

CompletenessReport audit_completeness_staged(
    const RequirementCatalog& catalog,
    std::span<const ExecutableBinding> bindings,
    std::span<const std::string_view> executable_scenarios) {
    return audit_rows(catalog, bindings, executable_scenarios, true);
}

}  // namespace moq::interop::requirements
