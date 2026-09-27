#pragma once

#include <map>
#include <string>

namespace moq::interop::app {

struct BuildInfo {
    std::string version;
    std::string source_revision;
    std::map<std::string, std::string> dependencies;
};

inline BuildInfo build_info() {
    return {
        MOQ_INTEROP_VERSION,
        MOQ_INTEROP_SOURCE_REVISION,
        {
            {"quiche", MOQ_INTEROP_QUICHE_REVISION},
            {"nlohmann_json", MOQ_INTEROP_NLOHMANN_JSON_REVISION},
            {"cpp_httplib", MOQ_INTEROP_CPP_HTTPLIB_REVISION},
            {"googletest", MOQ_INTEROP_GOOGLETEST_REVISION},
            {"sqlite3", MOQ_INTEROP_SQLITE3_REVISION},
        },
    };
}

}  // namespace moq::interop::app
