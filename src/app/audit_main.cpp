#include "moq/interop/app/version.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft22_evaluators.h"
#include "moq/interop/requirements/execution_audit.h"
#include "moq/interop/storage/run_store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <algorithm>
#include <iostream>
#include <optional>
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
    std::cerr << "Usage: moq-interop-audit --draft 18|21|22 [--format text|json] "
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
            else throw std::invalid_argument("draft must be 18, 21 or 22");
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
    // audit_execution over stored draft 22 runs is not built yet; refuse rather than audit nothing.
    if (result.draft == 22 && result.database)
        throw std::invalid_argument("stored draft 22 runs are not supported yet");
    return result;
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
        const auto source = moq::interop::requirements::load_draft_source(
            options.draft, options.docs,
            options.requirements / "draft-digests.json");
        const auto catalog =
            moq::interop::requirements::RequirementCatalog::load(
                source, options.requirements /
                    ("draft" + std::to_string(options.draft) + ".json"),
                // Draft 22 may still be complete:false; the audit loads it anyway and the gate
                // reports it (incomplete_catalog, exit 1) instead of refusing to run.
                options.draft == 22
                    ? moq::interop::requirements::CatalogLoadMode::AllowIncomplete
                    : moq::interop::requirements::CatalogLoadMode::RequireComplete);
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
        std::cerr << "moq-interop-audit: " << error.what() << '\n';
        usage();
        return 2;
    }
}
