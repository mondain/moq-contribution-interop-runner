#pragma once

#include "moq/interop/requirements/catalog.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::requirements {

struct ExecutableBinding {
    unsigned draft;
    std::string requirement_id;
    std::string scenario_id;
    std::string evaluator_id;
    std::vector<std::string> evidence_kinds;
};

struct CompletenessFinding {
    std::string code;
    std::string requirement_id;
    std::string detail;
    bool blocking;
};

struct CompletenessReport {
    unsigned draft;
    std::size_t required_total{0};
    std::size_t required_covered{0};
    std::size_t optional_total{0};
    std::size_t optional_covered{0};
    std::vector<CompletenessFinding> findings;

    [[nodiscard]] bool complete() const noexcept;
};

CompletenessReport audit_completeness(
    const RequirementCatalog& catalog,
    std::span<const ExecutableBinding> bindings,
    std::span<const std::string_view> executable_scenarios);

}  // namespace moq::interop::requirements
