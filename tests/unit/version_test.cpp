#include "moq/interop/app/version.h"

#include <gtest/gtest.h>

#include <regex>

namespace moq::interop::app {
namespace {

TEST(Version, ReportsBuildAndDependencyRevisions) {
    const BuildInfo info = build_info();

    EXPECT_TRUE(std::regex_match(info.version, std::regex(R"([0-9]+\.[0-9]+\.[0-9]+)")));
    EXPECT_FALSE(info.source_revision.empty());
    EXPECT_EQ(info.dependencies.count("quiche"), 0U);
    EXPECT_EQ(info.dependencies.at("picoquic"),
              "61fcd56ae0f069a1e98459e7a4965312a5190943");
    EXPECT_EQ(info.dependencies.at("picotls"),
              "bfa67875982afc4c24f21e146cef4747fa189c2f");

    for (const char* dependency : {"picoquic", "picotls",
                                   "nlohmann_json", "cpp_httplib",
                                   "googletest", "sqlite3"}) {
        const auto it = info.dependencies.find(dependency);
        ASSERT_NE(it, info.dependencies.end()) << dependency;
        EXPECT_FALSE(it->second.empty()) << dependency;
    }
}

}  // namespace
}  // namespace moq::interop::app
