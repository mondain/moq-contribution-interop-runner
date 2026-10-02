#include "moq/interop/requirements/execution_audit.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace moq::interop::requirements {
namespace {

using Json = nlohmann::json;

std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

Json config_json(const app::RunConfig& config) {
    Json names = Json::array();
    std::string track_name;
    if (config.track_fixture) {
        for (const auto& field : config.track_fixture->namespace_fields)
            names.push_back(hex(field));
        track_name = hex(config.track_fixture->track_name);
    }
    return {{"draft", static_cast<unsigned>(config.draft)},
            {"transport", static_cast<unsigned>(config.transport)},
            {"mode", static_cast<unsigned>(config.mode)},
            {"scenarios", config.scenario_ids},
            {"timeout_ms", config.timeout.count()},
            {"track", {{"namespace_hex", std::move(names)},
                       {"name_hex", track_name}}}};
}

Json build_json(const app::BuildInfo& build) {
    return {{"version", build.version},
            {"source_revision", build.source_revision},
            {"dependencies", build.dependencies}};
}

Json score_json(const ScoreSummary& score) {
    return {{"verdict", static_cast<unsigned>(score.verdict)},
            {"required", {score.required.earned, score.required.possible}},
            {"weighted", {score.weighted.earned, score.weighted.possible}},
            {"coverage", {score.coverage.earned, score.coverage.possible}}};
}

Json compatibility_mappings(const storage::RunRecord& run) {
    std::vector<std::tuple<std::optional<std::string>, std::optional<std::string>, std::string>> mappings;
    for (const auto& event : run.events) {
        if (event.kind == "compatibility_error_mapping")
            mappings.emplace_back(event.scenario_id, event.requirement_id, event.detail);
    }
    std::sort(mappings.begin(), mappings.end());
    Json result = Json::array();
    for (const auto& [scenario, requirement, detail] : mappings)
        result.push_back({{"scenario_id", scenario}, {"requirement_id", requirement}, {"mapping", detail}});
    return result;
}

bool same_score(const ScoreSummary& left, const ScoreSummary& right) {
    return left.verdict == right.verdict &&
           left.required.earned == right.required.earned &&
           left.required.possible == right.required.possible &&
           left.weighted.earned == right.weighted.earned &&
           left.weighted.possible == right.weighted.possible &&
           left.coverage.earned == right.coverage.earned &&
           left.coverage.possible == right.coverage.possible;
}

bool has_evidence(const storage::RunRecord& run,
                  std::string_view scenario_id, std::string_view kind) {
    return std::any_of(run.events.begin(), run.events.end(),
        [&](const auto& event) {
            return event.kind == kind && event.scenario_id &&
                   *event.scenario_id == scenario_id;
        });
}

}  // namespace

bool ExecutionAudit::consistent() const noexcept {
    return findings.empty();
}

std::string canonical_result_sha256(const storage::RunRecord& run) {
    Json outcomes = Json::array();
    for (const auto& outcome : run.outcomes) {
        outcomes.push_back({{"id", outcome.requirement_id},
                            {"state", static_cast<unsigned>(outcome.state)}});
    }
    using EvidenceKey = std::tuple<std::string, std::optional<std::string>,
                                   std::optional<std::string>>;
    std::vector<EvidenceKey> evidence_keys;
    evidence_keys.reserve(run.events.size());
    for (const auto& event : run.events) {
        evidence_keys.emplace_back(event.kind, event.scenario_id,
                                   event.requirement_id);
    }
    std::sort(evidence_keys.begin(), evidence_keys.end());
    Json evidence = Json::array();
    for (const auto& [kind, scenario_id, requirement_id] : evidence_keys)
        evidence.push_back({{"kind", kind}, {"scenario_id", scenario_id},
                            {"requirement_id", requirement_id}});
    const Json document{{"config", config_json(run.config)},
                        {"compatibility_mappings", compatibility_mappings(run)},
                        {"validator", build_json(run.build)},
                        {"state", static_cast<unsigned>(run.state)},
                        {"score", run.score ? score_json(*run.score) : Json(nullptr)},
                        {"outcomes", std::move(outcomes)},
                        {"evidence", std::move(evidence)}};
    const auto serialized = document.dump();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    if (EVP_Digest(serialized.data(), serialized.size(), digest.data(), &length,
                   EVP_sha256(), nullptr) != 1 || length != 32)
        throw std::runtime_error("execution audit SHA-256 failed");
    return hex(std::string_view(
        reinterpret_cast<const char*>(digest.data()), length));
}

ExecutionAudit audit_execution(
    const RequirementCatalog& catalog,
    std::span<const ExecutableBinding> bindings,
    std::span<const storage::RunRecord> runs) {
    ExecutionAudit audit;
    audit.run_count = runs.size();
    std::map<std::string, std::pair<std::string, app::RunId>> repeated;
    for (const auto& run : runs) {
        if (static_cast<unsigned>(run.config.draft) != catalog.draft) {
            audit.findings.push_back({"wrong_draft_run", run.id, "",
                "run draft differs from audited catalog"});
            continue;
        }
        if (run.state != storage::RunState::Finalized) {
            audit.findings.push_back({"unfinished_run", run.id, "",
                "run has no finalized outcome"});
            continue;
        }
        if (!run.score) {
            audit.findings.push_back({"missing_score", run.id, "",
                "finalized run has no score"});
        } else {
            if (run.score->verdict == RunVerdict::Error) {
                audit.findings.push_back({"run_error", run.id, "",
                    "harness or publisher startup error prevents verification"});
            }
            if (!same_score(*run.score, score(catalog, run.outcomes))) {
                audit.findings.push_back({"stored_score_mismatch", run.id, "",
                    "stored score does not match catalog and row outcomes"});
            }
        }
        for (const auto& outcome : run.outcomes) {
            if (outcome.state != OutcomeState::Pass &&
                outcome.state != OutcomeState::Fail) continue;
            ++audit.scored_rows;
            bool bound = false;
            bool evidenced = false;
            for (const auto& binding : bindings) {
                if (binding.draft != catalog.draft ||
                    binding.requirement_id != outcome.requirement_id ||
                    std::find(run.config.scenario_ids.begin(),
                              run.config.scenario_ids.end(),
                              binding.scenario_id) == run.config.scenario_ids.end())
                    continue;
                bound = true;
                if (std::all_of(binding.evidence_kinds.begin(),
                                binding.evidence_kinds.end(),
                                [&](const auto& kind) {
                                    return has_evidence(run, binding.scenario_id,
                                                        kind);
                                })) {
                    evidenced = true;
                    break;
                }
            }
            if (!bound) {
                audit.findings.push_back({"scored_without_binding", run.id,
                    outcome.requirement_id,
                    "scored row has no registered evaluator in this scenario"});
            } else if (outcome.state == OutcomeState::Pass && !evidenced) {
                audit.findings.push_back({"missing_evaluator_evidence", run.id,
                    outcome.requirement_id,
                    "passed row lacks complete declared evidence for every "
                    "selected evaluator binding"});
            }
        }
        const Json group_json{{"config", config_json(run.config)},
                              {"compatibility_mappings", compatibility_mappings(run)},
                              {"validator", build_json(run.build)}};
        const auto group = group_json.dump();
        const auto digest = canonical_result_sha256(run);
        const auto [found, inserted] = repeated.emplace(
            group, std::pair{digest, run.id});
        if (!inserted && found->second.first != digest) {
            audit.findings.push_back({"nondeterministic_result", run.id, "",
                "result differs from repeat run " + found->second.second});
        }
    }
    std::sort(audit.findings.begin(), audit.findings.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.run_id, left.requirement_id, left.code,
                            left.detail) <
                   std::tie(right.run_id, right.requirement_id, right.code,
                            right.detail);
        });
    return audit;
}

}  // namespace moq::interop::requirements
