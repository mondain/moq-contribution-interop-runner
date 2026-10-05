#include "moq/interop/requirements/carry_forward.h"

#include <filesystem>
#include <iostream>
#include <string>

using namespace moq::interop::requirements;

int main(int argc, char** argv) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    const auto docs = root / "docs";
    const auto requirements = root / "requirements";
    const auto digests = requirements / "draft-digests.json";
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool force = argc > 2 && std::string(argv[2]) == "--force";
    if (mode != "generate" && mode != "merge") {
        std::cerr << "usage: moq-interop-catalog-carry generate [--force] | merge\n";
        return 2;
    }
    try {
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
        std::cout << "identical=" << counts[0] << " moved=" << counts[1]
                  << " reworded=" << counts[2] << " new=" << counts[3]
                  << " removed=" << result.removed.size() << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
