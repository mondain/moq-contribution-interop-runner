#pragma once

#include "moq/interop/requirements/completeness.h"
#include "moq/interop/storage/run_store.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::requirements {

struct ExecutionFinding {
    std::string code;
    app::RunId run_id;
    std::string requirement_id;
    std::string detail;
};

struct ExecutionAudit {
    std::size_t run_count{0};
    std::size_t scored_rows{0};
    std::vector<ExecutionFinding> findings;

    [[nodiscard]] bool consistent() const noexcept;
};

std::string canonical_result_sha256(const storage::RunRecord& run);

ExecutionAudit audit_execution(
    const RequirementCatalog& catalog,
    std::span<const ExecutableBinding> bindings,
    std::span<const storage::RunRecord> runs);

}  // namespace moq::interop::requirements
