#include "moq/interop/app/version.h"

#include <gtest/gtest.h>

#include <regex>

namespace moq::interop::app {
namespace {

TEST(Version, ReportsBuildAndDependencyRevisions) {
    const BuildInfo info = build_info();

    EXPECT_TRUE(std::regex_match(info.version, std::regex(R"([0-9]+\.[0-9]+\.[0-9]+)")));
    EXPECT_FALSE(info.source_revision.empty());

    for (const char* dependency : {"quiche", "nlohmann_json", "cpp_httplib", "googletest", "sqlite3"}) {
        const auto it = info.dependencies.find(dependency);
        ASSERT_NE(it, info.dependencies.end()) << dependency;
        EXPECT_FALSE(it->second.empty()) << dependency;
    }
}

}  // namespace
}  // namespace moq::interop::app
