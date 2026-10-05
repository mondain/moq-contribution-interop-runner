#pragma once

#include "moq/interop/requirements/catalog.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace moq::interop::requirements {

struct DeltaEntry {
    std::string d22;
    std::string d21;
    std::string change;
    std::string note;
    std::vector<std::string> tags;
};

// Reads the `entries` of draft21-to-22-delta.json.
std::vector<DeltaEntry> load_delta_entries(const std::filesystem::path& path);

struct Equivalence {
    std::string d22;
    std::vector<std::string> d21;
    std::string justification;
};

struct LineageInput {
    const RequirementCatalog& draft21;
    const RequirementCatalog& draft22;
    std::vector<DeltaEntry> delta;
    std::vector<Equivalence> equivalences;
    std::set<std::string> filter_building_scenarios;
};

struct RowPair {
    std::string d21;
    std::string d22;
};

struct NamePair {
    std::string d22;
    std::string d21;
};

struct Lineage {
    std::vector<RowPair> shared_rows;
    std::vector<std::string> own_rows;
    std::vector<NamePair> shared_scenarios;
    std::vector<std::string> own_scenarios;
    std::vector<NamePair> shared_evaluators;
    std::vector<std::string> own_evaluators;
};

Lineage build_lineage(const LineageInput& input);
std::string render_lineage_header(const Lineage& lineage);

}  // namespace moq::interop::requirements
