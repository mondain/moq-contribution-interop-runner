#include "moq/interop/requirements/carry_forward.h"
#include "moq/interop/requirements/lineage.h"
#include "moq/interop/requirements/lineage_policy.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace moq::interop::requirements;

int main(int argc, char** argv) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    const auto docs = root / "docs";
    const auto requirements = root / "requirements";
    const auto digests = requirements / "draft-digests.json";
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool force = argc > 2 && std::string(argv[2]) == "--force";
    if (mode != "generate" && mode != "merge" && mode != "lineage") {
        std::cerr << "usage: moq-interop-catalog-carry generate [--force] | merge | lineage"
                     "   (generate writes an unreviewed baseline for a fresh draft; after hand review use only merge)\n";
        return 2;
    }
    try {
        if (mode == "lineage") {
            // Derived file: always overwritten, never hand-edited (a test regenerates it and compares).
            const auto source21 = load_draft_source(21, docs, digests);
            const auto source22 = load_draft_source(22, docs, digests);
            const auto catalog21 = RequirementCatalog::load(
                source21, requirements / "draft21.json", CatalogLoadMode::RequireComplete);
            const auto catalog22 = RequirementCatalog::load(
                source22, requirements / "draft22.json", CatalogLoadMode::AllowIncomplete);
            const auto lineage = build_lineage({catalog21, catalog22,
                load_delta_entries(requirements / "draft21-to-22-delta.json"),
                draft22_equivalences(), draft22_filter_building_scenarios()});
            const auto output = root / "include/moq/interop/requirements/draft22_lineage_data.h";
            std::ofstream file(output, std::ios::binary | std::ios::trunc);
            file << render_lineage_header(lineage);
            file.close();
            if (!file) throw std::runtime_error("cannot write " + output.string());
            std::cout << "wrote " << output.string() << "\n"
                      << "shared rows=" << lineage.shared_rows.size() << " own rows=" << lineage.own_rows.size()
                      << "\nshared scenarios=" << lineage.shared_scenarios.size()
                      << " own scenarios=" << lineage.own_scenarios.size()
                      << "\nshared evaluators=" << lineage.shared_evaluators.size()
                      << " own evaluators=" << lineage.own_evaluators.size() << "\n";
            return 0;
        }
        const auto new_source = load_draft_source(22, docs, digests);
        if (mode == "merge") {
            merge_partitions(new_source, requirements);
            return 0;
        }
        const auto old_source = load_draft_source(21, docs, digests);
        const auto old_catalog = RequirementCatalog::load(old_source, requirements / "draft21.json");
        const auto result = carry_forward(old_source, old_catalog, new_source);
        write_catalog_outputs(result, old_catalog, new_source,
                              diff_wire_blocks(old_source, new_source),
                              EmitOptions{requirements, 1200, force});
        std::size_t counts[5] = {};
        for (const auto& match : result.matches) {
            ++counts[static_cast<std::size_t>(match.change)];
        }
        std::cout << "occurrences: identical=" << counts[0] << " moved=" << counts[1]
                  << " reworded=" << counts[2] << " new=" << counts[3]
                  << "; draft 21 rows removed=" << result.removed.size() << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
