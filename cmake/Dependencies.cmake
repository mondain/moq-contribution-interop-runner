include(FetchContent)

set(MOQ_INTEROP_PICOQUIC_REVISION "61fcd56ae0f069a1e98459e7a4965312a5190943")
set(MOQ_INTEROP_PICOTLS_REVISION "bfa67875982afc4c24f21e146cef4747fa189c2f")
set(MOQ_INTEROP_NLOHMANN_JSON_REVISION "65ee68451d8eb2b5f3a30b410476ab83deb3289b")
set(MOQ_INTEROP_CPP_HTTPLIB_REVISION "787a34ad7f01f20922a237d5142aae469828be72")
set(MOQ_INTEROP_GOOGLETEST_REVISION "52eb8108c5bdec04579160ae17225d66034bd723")

FetchContent_Declare(
    picoquic
    GIT_REPOSITORY https://github.com/private-octopus/picoquic.git
    GIT_TAG ${MOQ_INTEROP_PICOQUIC_REVISION}
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
