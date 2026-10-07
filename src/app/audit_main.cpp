#include "moq/interop/app/draft_traits.h"
#include "moq/interop/app/version.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_evaluators.h"
#include "moq/interop/requirements/execution_audit.h"
#include "moq/interop/requirements/lite_evaluators.h"
#include "moq/interop/storage/run_store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <algorithm>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using moq::interop::requirements::Applicability;
using moq::interop::requirements::Testability;
using Json = nlohmann::json;

struct Options {
    unsigned draft{0};
    std::string format{"text"};
    std::optional<std::filesystem::path> database;
    std::filesystem::path docs =
        std::filesystem::exists(
            std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "docs")
            ? std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "docs"
            : "/usr/share/moq-interop/docs";
    std::filesystem::path requirements =
        std::filesystem::exists(
            std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "requirements")
            ? std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "requirements"
            : "/usr/share/moq-interop/requirements";
};

void usage() {
    std::cerr << "Usage: moq-interop-audit --draft 18|21|22|moq-lite-06 [--format text|json] "
                 "[--docs DIR] [--requirements DIR] [--database PATH]\n";
}

Options parse(int argc, char* argv[]) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag(argv[index]);
        if (++index >= argc) throw std::invalid_argument("missing value for " + std::string(flag));
        const std::string value(argv[index]);
        if (flag == "--draft") {
            if (value == "18") result.draft = 18;
            else if (value == "21") result.draft = 21;
            else if (value == "22") result.draft = 22;
            else if (value == moq::interop::app::draft_text(moq::interop::app::DraftVersion::MoqLite06))
                result.draft = moq::interop::app::draft_number(moq::interop::app::DraftVersion::MoqLite06);
            else throw std::invalid_argument("draft must be 18, 21, 22 or moq-lite-06");
        } else if (flag == "--format") {
            if (value != "text" && value != "json")
                throw std::invalid_argument("format must be text or json");
            result.format = value;
        } else if (flag == "--docs") {
            result.docs = value;
        } else if (flag == "--requirements") {
            result.requirements = value;
        } else if (flag == "--database") {
            result.database = value;
        } else {
            throw std::invalid_argument("unknown option " + std::string(flag));
        }
    }
    if (result.draft == 0) throw std::invalid_argument("--draft is required");
    return result;
}

bool is_lite(unsigned draft) {
    const auto parsed = moq::interop::app::parse_draft(draft);
    return parsed && !moq::interop::app::is_moqt(*parsed);
}

// The staged audit of the moq-lite-06 catalog: incomplete by design, never a pass. The L1d executable
// bindings (lite_executable_bindings) cover the reviewed Applicable+Testable rows over the executable lite
// scenarios; an uncovered reviewed row would be a non-blocking "missing evaluator" finding, and the exit status
// reflects only real catalog errors and blocking binding findings.
int audit_lite(const Options& options) {
    using namespace moq::interop;
    if (options.database)
        throw std::invalid_argument("--database does not apply to moq-lite-06");
    const std::string name(app::draft_text(app::DraftVersion::MoqLite06));
    const auto source = requirements::load_draft_source(
        options.draft, options.docs, options.requirements / "draft-digests.json");
    const auto catalog = requirements::RequirementCatalog::load(
        source, options.requirements / (name + ".json"),
        requirements::CatalogLoadMode::AllowIncomplete);
    const auto source_audit = requirements::audit_normative_occurrences_staged(source, catalog);
    const auto bindings = requirements::lite_executable_bindings();
    const auto report = requirements::audit_completeness_staged(
        catalog, bindings, app::executable_scenarios(options.draft));
    const auto counts = requirements::staged_counts(catalog);
    std::set<std::string> planned;
    for (const auto& row : catalog.requirements)
        planned.insert(row.scenarios.begin(), row.scenarios.end());
    const bool blocking = std::any_of(report.findings.begin(), report.findings.end(),
        [](const auto& finding) { return finding.blocking; });
    const bool ok = source_audit.ok() && !blocking;
    const std::string verdict = "STAGED: incomplete catalog (not a pass)";
    if (options.format == "text") {
        std::cout << "Draft " << name << " source " << source.sha256 << '\n'
                  << "Rows: " << counts.rows << '\n'
                  << "Reviewed: " << counts.reviewed << '\n'
                  << "Unreviewed: " << counts.unreviewed << '\n'
                  << "Unreviewed required (MUST/MUST NOT): " << counts.unreviewed_required << '\n'
                  << "Required applicable testable (reviewed rows): " << report.required_total << '\n'
                  << "Planned scenarios: " << planned.size() << '\n'
                  << "Required coverage (reviewed rows): " << report.required_covered << " of "
                  << report.required_total << '\n'
                  << "Optional coverage (reviewed rows): " << report.optional_covered << " of "
                  << report.optional_total << '\n'
                  << "Source-keyword audit: " << (source_audit.ok() ? "complete" : "failed") << '\n'
                  << "Findings: " << report.findings.size() << '\n';
        for (const auto& finding : report.findings) {
            std::cout << "  [" << (finding.blocking ? "blocking" : "non-blocking") << "] "
                      << finding.code;
            if (!finding.requirement_id.empty()) std::cout << ' ' << finding.requirement_id;
            std::cout << ": " << finding.detail << '\n';
        }
        std::cout << verdict << '\n';
    } else {
        Json findings = Json::array();
        for (const auto& finding : report.findings) {
            findings.push_back({{"code", finding.code},
                                {"requirement_id", finding.requirement_id},
                                {"detail", finding.detail},
                                {"blocking", finding.blocking}});
        }
        const Json output = {
            {"schema_version", 1}, {"draft", name},
            {"source_sha256", source.sha256},
            {"source_audit", {{"complete", source_audit.ok()},
                              {"missing_count", source_audit.missing.size()},
                              {"multiply_classified_count", source_audit.multiply_classified.size()},
                              {"errors", source_audit.errors}}},
            {"rows", counts.rows}, {"reviewed", counts.reviewed},
            {"unreviewed", counts.unreviewed},
            {"unreviewed_required", counts.unreviewed_required},
            {"required_applicable_testable", report.required_total},
            {"required_covered", report.required_covered},
            {"optional_applicable_testable", report.optional_total},
            {"optional_covered", report.optional_covered},
            {"planned_scenarios", planned.size()},
            {"staged", true}, {"complete", false}, {"verdict", verdict},
            {"findings", std::move(findings)}};
        std::cout << output.dump(2) << '\n';
    }
    return ok ? 0 : 1;
}

const char* classification(const moq::interop::requirements::Requirement& row) {
    if (row.applicability == Applicability::Informative) return "informative";
    if (row.applicability == Applicability::NotApplicable) return "not_applicable";
    if (row.testability == Testability::NotTestable) return "not_testable";
    return nullptr;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse(argc, argv);
        if (is_lite(options.draft)) return audit_lite(options);
        const auto source = moq::interop::requirements::load_draft_source(
            options.draft, options.docs,
            options.requirements / "draft-digests.json");
        const auto catalog =
            moq::interop::requirements::RequirementCatalog::load(
                source, options.requirements /
                    ("draft" + std::to_string(options.draft) + ".json"),
                moq::interop::requirements::CatalogLoadMode::RequireComplete);
        const auto source_audit =
            moq::interop::requirements::audit_normative_occurrences(source, catalog);
        const auto bindings = [&] {
            switch (options.draft) {
                case 18: return moq::interop::requirements::draft18_executable_bindings();
                case 21: return moq::interop::requirements::draft21_executable_bindings();
                case 22: return moq::interop::requirements::draft22_executable_bindings();
                default: throw std::invalid_argument("draft must be 18, 21 or 22");
            }
        }();
        const auto report =
            moq::interop::requirements::audit_completeness(
                catalog, bindings,
                moq::interop::app::executable_scenarios(options.draft));
        const bool static_complete = source_audit.ok() && report.complete();
        std::optional<moq::interop::requirements::ExecutionAudit> execution;
        std::vector<moq::interop::storage::RunRecord> runs;
        if (options.database) {
            if (!std::filesystem::is_regular_file(*options.database))
                throw std::invalid_argument("run database does not exist");
            moq::interop::storage::SqliteRunStore store(
                *options.database, moq::interop::app::build_info());
            for (std::size_t offset = 0;; offset += 100) {
                const auto page = store.list({100, offset});
                for (const auto& summary : page.items) {
                    if (static_cast<unsigned>(summary.config.draft) == options.draft)
                        runs.push_back(store.load(summary.id));
                }
                if (!page.next_offset) break;
            }
            std::sort(runs.begin(), runs.end(),
                [](const auto& left, const auto& right) {
                    return left.id < right.id;
                });
            execution = moq::interop::requirements::audit_execution(
                catalog, bindings, runs);
        }
        if (options.format == "text") {
            std::cout << "Draft " << options.draft << " source " << source.sha256 << '\n'
                      << "Required executable coverage: " << report.required_covered
                      << '/' << report.required_total << '\n'
                      << "Optional executable coverage: " << report.optional_covered
                      << '/' << report.optional_total << '\n'
                      << "Source-keyword audit: "
                      << (source_audit.ok() ? "complete" : "failed") << '\n'
                      << "Static gate: " << (static_complete ? "PASS" : "FAIL")
                      << " (" << report.findings.size() << " findings)\n";
            if (execution) {
                std::cout << "Execution audit: "
                          << (execution->consistent() ? "consistent" : "findings")
                          << " (" << execution->run_count << " runs, "
                          << execution->scored_rows << " scored rows, "
                          << execution->findings.size() << " findings)\n";
            }
        } else {
            Json findings = Json::array();
            for (const auto& finding : report.findings) {
                findings.push_back({{"code", finding.code},
                                    {"requirement_id", finding.requirement_id},
                                    {"detail", finding.detail},
                                    {"blocking", finding.blocking}});
            }
            Json residual = Json::array();
            for (const auto& row : catalog.requirements) {
                const char* state = classification(row);
                if (!state) continue;
                residual.push_back({{"requirement_id", row.id},
                                    {"classification", state},
                                    {"reason", row.rationale},
                                    {"section", row.source.section},
                                    {"first_line", row.source.first_line}});
            }
            const auto build = moq::interop::app::build_info();
            Json execution_json = nullptr;
            if (execution) {
                Json dynamic_findings = Json::array();
                for (const auto& finding : execution->findings) {
                    dynamic_findings.push_back({{"code", finding.code},
                        {"run_id", finding.run_id},
                        {"requirement_id", finding.requirement_id},
                        {"detail", finding.detail}});
                }
                Json run_digests = Json::array();
                for (const auto& run : runs) {
                    run_digests.push_back({{"run_id", run.id},
                        {"transport", run.config.transport ==
                             moq::interop::app::TransportKind::WebTransport
                             ? "webtransport" : "native-quic"},
                        {"canonical_sha256",
                         moq::interop::requirements::canonical_result_sha256(run)}});
                }
                execution_json = {{"consistent", execution->consistent()},
                    {"run_count", execution->run_count},
                    {"scored_rows", execution->scored_rows},
                    {"findings", std::move(dynamic_findings)},
                    {"runs", std::move(run_digests)}};
            }
            const Json output = {
                {"schema_version", 1}, {"draft", options.draft},
                {"source_sha256", source.sha256},
                {"source_revision", build.source_revision},
                {"static_complete", static_complete},
                {"source_audit", {{"complete", source_audit.ok()},
                                  {"missing_count", source_audit.missing.size()},
                                  {"multiply_classified_count",
                                   source_audit.multiply_classified.size()},
                                  {"errors", source_audit.errors}}},
                {"executable_coverage", {{"required_covered", report.required_covered},
                                         {"required_total", report.required_total},
                                         {"optional_covered", report.optional_covered},
                                         {"optional_total", report.optional_total}}},
                {"findings", std::move(findings)},
                {"classified_residuals", std::move(residual)},
                {"execution_audit", std::move(execution_json)}};
            std::cout << output.dump(2) << '\n';
        }
        return static_complete && (!execution || execution->consistent()) ? 0 : 1;
    } catch (const std::exception& error) {
        // A loader or argument error exits 2 with the usage text, for every draft (unchanged for 18/21/22).
        std::cerr << "moq-interop-audit: " << error.what() << '\n';
        usage();
        return 2;
    }
}
