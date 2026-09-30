#pragma once

#include "moq/interop/app/version.h"
#include "moq/interop/http/server.h"

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string>

namespace moq::interop::http::detail {

nlohmann::json error_json(const ApiError& error);
nlohmann::json build_json(const app::BuildInfo& build);
nlohmann::json catalog_json(const requirements::RequirementCatalog& catalog);
nlohmann::json requirement_json(const requirements::Requirement& requirement);
nlohmann::json run_json(const storage::RunRecord& run);
nlohmann::json run_summary_json(const storage::RunSummary& run);
nlohmann::json event_json(const storage::EvidenceEvent& event);
nlohmann::json pagination_json(std::size_t limit, std::size_t offset, std::size_t total,
                               const std::optional<std::size_t>& next_offset);

std::string render_run_list(std::span<const storage::RunSummary> runs,
                            const nlohmann::json& completeness);
struct ReportFilters {
    std::string strength;
    std::string outcome;
    std::string section;
    std::string scenario;
};
std::string_view report_styles();
std::string render_run_detail(const storage::RunRecord& run,
                              const requirements::RequirementCatalog& catalog,
                              const ReportFilters& filters);

}  // namespace moq::interop::http::detail
