// The golden executable-id lists (tests/golden/executable-ids-dNN.txt) pin which scenario ids the
// moqxr adapter command-line golden files must cover. They must equal the registry exactly.
#include "moq/interop/app/scenario_registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << path;
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

void expect_golden_matches_registry(unsigned draft) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    const auto golden =
        read_lines(root / "tests" / "golden" / ("executable-ids-d" + std::to_string(draft) + ".txt"));
    std::vector<std::string> registry;
    for (const auto id : moq::interop::app::executable_scenarios(draft)) registry.emplace_back(id);
    std::sort(registry.begin(), registry.end());
    EXPECT_TRUE(std::is_sorted(golden.begin(), golden.end())) << "golden list is not sorted";
    EXPECT_FALSE(golden.empty());
    EXPECT_EQ(golden, registry);
}

TEST(ExecutableIdsGolden, Draft18) { expect_golden_matches_registry(18); }
TEST(ExecutableIdsGolden, Draft21) { expect_golden_matches_registry(21); }

}  // namespace
