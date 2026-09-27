cmake_minimum_required(VERSION 3.24)
project(moq_contribution_interop_runner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOQ_INTEROP_BUILD_TESTS "Build unit tests" ON)

include(cmake/Dependencies.cmake)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Git REQUIRED)
FetchContent_MakeAvailable(nlohmann_json)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse "${MOQ_INTEROP_NLOHMANN_JSON_REVISION}^{commit}"
    WORKING_DIRECTORY "${nlohmann_json_SOURCE_DIR}"
    OUTPUT_VARIABLE MOQ_INTEROP_EXPECTED_NLOHMANN_JSON_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${nlohmann_json_SOURCE_DIR}"
    OUTPUT_VARIABLE MOQ_INTEROP_ACTUAL_NLOHMANN_JSON_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT MOQ_INTEROP_ACTUAL_NLOHMANN_JSON_REVISION STREQUAL MOQ_INTEROP_EXPECTED_NLOHMANN_JSON_COMMIT)
    message(FATAL_ERROR "nlohmann_json revision mismatch: pinned ${MOQ_INTEROP_NLOHMANN_JSON_REVISION} resolves to ${MOQ_INTEROP_EXPECTED_NLOHMANN_JSON_COMMIT}, actual ${MOQ_INTEROP_ACTUAL_NLOHMANN_JSON_REVISION}")
endif()
set(MOQ_INTEROP_SOURCE_REVISION "" CACHE STRING "Source commit revision")
if(NOT MOQ_INTEROP_SOURCE_REVISION)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE MOQ_INTEROP_SOURCE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

add_library(moq-interop-app INTERFACE)
target_include_directories(moq-interop-app INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(moq-interop-app INTERFACE
    MOQ_INTEROP_VERSION="${PROJECT_VERSION}"
    MOQ_INTEROP_SOURCE_REVISION="${MOQ_INTEROP_SOURCE_REVISION}"
    MOQ_INTEROP_QUICHE_REVISION="${MOQ_INTEROP_QUICHE_REVISION}"
    MOQ_INTEROP_NLOHMANN_JSON_REVISION="${MOQ_INTEROP_NLOHMANN_JSON_REVISION}"
    MOQ_INTEROP_CPP_HTTPLIB_REVISION="${MOQ_INTEROP_CPP_HTTPLIB_REVISION}"
    MOQ_INTEROP_GOOGLETEST_REVISION="${MOQ_INTEROP_GOOGLETEST_REVISION}"
    MOQ_INTEROP_SQLITE3_REVISION="${SQLite3_VERSION}"
)

add_library(moq-interop-requirements
    src/requirements/draft_source.cpp
    src/requirements/catalog.cpp
    src/requirements/audit.cpp
)
target_link_libraries(moq-interop-requirements PUBLIC moq-interop-app)
target_link_libraries(moq-interop-requirements PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json)

add_executable(moq-interop-runner src/app/main.cpp)
target_link_libraries(moq-interop-runner PRIVATE moq-interop-app)
install(TARGETS moq-interop-runner RUNTIME DESTINATION bin)

if(MOQ_INTEROP_BUILD_TESTS)
    enable_testing()
    FetchContent_MakeAvailable(googletest)

    add_executable(moq-interop-unit-tests tests/unit/version_test.cpp)
    target_link_libraries(moq-interop-unit-tests PRIVATE moq-interop-app GTest::gtest_main)
    add_test(NAME version COMMAND moq-interop-unit-tests)

    add_executable(moq-interop-draft-source-tests tests/unit/draft_source_test.cpp)
    target_compile_definitions(moq-interop-draft-source-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft-source-tests PRIVATE moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft-source COMMAND moq-interop-draft-source-tests)

    add_executable(moq-interop-catalog-tests tests/unit/catalog_test.cpp tests/unit/catalog_audit_test.cpp)
    target_compile_definitions(moq-interop-catalog-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-catalog-tests PRIVATE moq-interop-requirements
                          nlohmann_json::nlohmann_json GTest::gtest_main)
    add_test(NAME catalog COMMAND moq-interop-catalog-tests)

    add_executable(moq-interop-draft18-catalog-part-01-tests
                   tests/unit/draft18_catalog_part_01_test.cpp)
    target_compile_definitions(moq-interop-draft18-catalog-part-01-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-catalog-part-01-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft18-catalog-part-01 COMMAND moq-interop-draft18-catalog-part-01-tests)

    add_executable(moq-interop-draft18-catalog-part-02-tests
                   tests/unit/draft18_catalog_part_02_test.cpp)
    target_compile_definitions(moq-interop-draft18-catalog-part-02-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-catalog-part-02-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft18-catalog-part-02 COMMAND moq-interop-draft18-catalog-part-02-tests)
endif()
