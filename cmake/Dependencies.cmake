include(FetchContent)

set(MOQ_INTEROP_QUICHE_REVISION "bbfe6205b8af2e6fadbb6d7818de463fbe123342")
set(MOQ_INTEROP_NLOHMANN_JSON_REVISION "65ee68451d8eb2b5f3a30b410476ab83deb3289b")
set(MOQ_INTEROP_CPP_HTTPLIB_REVISION "787a34ad7f01f20922a237d5142aae469828be72")
set(MOQ_INTEROP_GOOGLETEST_REVISION "52eb8108c5bdec04579160ae17225d66034bd723")

FetchContent_Declare(
    quiche
    GIT_REPOSITORY https://github.com/cloudflare/quiche.git
    GIT_TAG ${MOQ_INTEROP_QUICHE_REVISION}
    GIT_SUBMODULES quiche/deps/boringssl
    GIT_SUBMODULES_RECURSE FALSE
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG ${MOQ_INTEROP_NLOHMANN_JSON_REVISION}
)
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG ${MOQ_INTEROP_CPP_HTTPLIB_REVISION}
)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG ${MOQ_INTEROP_GOOGLETEST_REVISION}
)
