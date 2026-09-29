cmake_minimum_required(VERSION 3.24)
project(moq_contribution_interop_runner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOQ_INTEROP_BUILD_TESTS "Build unit tests" ON)
option(MOQ_INTEROP_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)

include(cmake/Dependencies.cmake)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Git REQUIRED)
include(cmake/Quiche.cmake)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json cpp_httplib)

set(MOQ_INTEROP_SCHEMA_FILE "${PROJECT_SOURCE_DIR}/src/storage/schema.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MOQ_INTEROP_SCHEMA_FILE}")
file(READ "${MOQ_INTEROP_SCHEMA_FILE}" MOQ_INTEROP_SCHEMA_SQL)
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/moq/interop/storage")
file(CONFIGURE
    OUTPUT "${PROJECT_BINARY_DIR}/generated/moq/interop/storage/schema_sql.h"
    CONTENT [=[#pragma once

namespace moq::interop::storage::detail {
inline constexpr const char kSchemaSql[] = R"moq_schema(@MOQ_INTEROP_SCHEMA_SQL@)moq_schema";
}  // namespace moq::interop::storage::detail
]=]
    @ONLY
)
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

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse "${MOQ_INTEROP_CPP_HTTPLIB_REVISION}^{commit}"
    WORKING_DIRECTORY "${cpp_httplib_SOURCE_DIR}"
    OUTPUT_VARIABLE MOQ_INTEROP_EXPECTED_CPP_HTTPLIB_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${cpp_httplib_SOURCE_DIR}"
    OUTPUT_VARIABLE MOQ_INTEROP_ACTUAL_CPP_HTTPLIB_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT MOQ_INTEROP_ACTUAL_CPP_HTTPLIB_REVISION STREQUAL MOQ_INTEROP_EXPECTED_CPP_HTTPLIB_COMMIT)
    message(FATAL_ERROR "cpp-httplib revision mismatch: pinned ${MOQ_INTEROP_CPP_HTTPLIB_REVISION} resolves to ${MOQ_INTEROP_EXPECTED_CPP_HTTPLIB_COMMIT}, actual ${MOQ_INTEROP_ACTUAL_CPP_HTTPLIB_REVISION}")
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

add_library(moq-interop-wire
    src/wire/common/cursor.cpp
    src/wire/draft18/messages.cpp
    src/wire/draft18/objects.cpp
)
target_link_libraries(moq-interop-wire PUBLIC moq-interop-app)
target_compile_options(moq-interop-wire PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-requirements
    src/requirements/draft_source.cpp
    src/requirements/catalog.cpp
    src/requirements/audit.cpp
    src/requirements/scoring.cpp
)
target_link_libraries(moq-interop-requirements PUBLIC moq-interop-app)
target_link_libraries(moq-interop-requirements PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json)

add_library(moq-interop-storage src/storage/sqlite_run_store.cpp)
target_include_directories(moq-interop-storage PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(moq-interop-storage PUBLIC moq-interop-app moq-interop-requirements
                      SQLite::SQLite3)

add_library(moq-interop-http
    src/http/server.cpp
    src/http/json.cpp
    src/http/report.cpp
)
target_link_libraries(moq-interop-http PUBLIC moq-interop-app moq-interop-requirements
                      moq-interop-storage)
target_link_libraries(moq-interop-http PRIVATE httplib::httplib nlohmann_json::nlohmann_json)

add_library(moq-interop-transport
    src/transport/quiche_connection.cpp
    src/transport/quiche_native_listener.cpp
)
target_link_libraries(moq-interop-transport PUBLIC moq-interop-app)
target_link_libraries(moq-interop-transport PRIVATE moq-interop-quiche)
target_include_directories(moq-interop-transport PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_include_directories(moq-interop-transport SYSTEM PRIVATE
    "${MOQ_INTEROP_BORINGSSL_INCLUDE_DIR}")
target_compile_options(moq-interop-transport PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-session
    src/session/publisher_session.cpp
    src/session/draft18_dispatch.cpp
)
target_link_libraries(moq-interop-session PUBLIC
    moq-interop-app moq-interop-wire)
target_include_directories(moq-interop-session PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_compile_options(moq-interop-session PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-scenarios
    src/scenarios/engine.cpp
    src/scenarios/draft18.cpp
    src/scenarios/action_dispatcher.cpp
    src/scenarios/run_controller.cpp)
target_link_libraries(moq-interop-scenarios PUBLIC moq-interop-session)
target_compile_options(moq-interop-scenarios PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-draft18-evaluators
    src/requirements/draft18_evaluators.cpp)
target_link_libraries(moq-interop-draft18-evaluators PUBLIC
    moq-interop-requirements moq-interop-session)
target_compile_options(moq-interop-draft18-evaluators PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_executable(moq-interop-runner src/app/main.cpp)
target_compile_definitions(moq-interop-runner PRIVATE
    MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(moq-interop-runner PRIVATE moq-interop-http
                      moq-interop-quiche)
install(TARGETS moq-interop-runner RUNTIME DESTINATION bin)
install(DIRECTORY docs/ DESTINATION share/moq-interop/docs
        FILES_MATCHING PATTERN "*.txt")
install(DIRECTORY requirements/ DESTINATION share/moq-interop/requirements
        FILES_MATCHING PATTERN "*.json" PATTERN "parts" EXCLUDE)

if(MOQ_INTEROP_BUILD_TESTS)
    enable_testing()
    FetchContent_MakeAvailable(googletest)

    add_executable(moq-interop-quiche-link-tests
                   tests/integration/quiche_link_test.cpp)
    target_link_libraries(moq-interop-quiche-link-tests PRIVATE
                          moq-interop-quiche)
    target_compile_options(moq-interop-quiche-link-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME quiche-link COMMAND moq-interop-quiche-link-tests)
    add_test(NAME quiche-dependency-contract
        COMMAND "${CMAKE_COMMAND}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DQUICHE_SOURCE_DIR=${quiche_SOURCE_DIR}"
            "-DPROJECT_LOCK_FILE=${MOQ_INTEROP_QUICHE_LOCK_FILE}"
            "-DFETCHED_LOCK_FILE=${MOQ_INTEROP_FETCHED_QUICHE_LOCK_FILE}"
            "-DVERIFY_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/VerifyQuiche.cmake"
            "-DQUICHE_REVISION=${MOQ_INTEROP_QUICHE_REVISION}"
            "-DBORINGSSL_REVISION=${MOQ_INTEROP_BORINGSSL_REVISION}"
            "-DCARGO_EXECUTABLE=${MOQ_INTEROP_CARGO_EXECUTABLE}"
            "-DCARGO_HOME=${MOQ_INTEROP_QUICHE_CARGO_HOME}"
            "-DCARGO_TARGET_DIR=${MOQ_INTEROP_QUICHE_CARGO_TARGET_DIR}"
            "-DREAL_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DFIXTURE_ROOT=${PROJECT_BINARY_DIR}/quiche-contract-fixtures"
            -P "${PROJECT_SOURCE_DIR}/tests/integration/quiche_dependency_contract.cmake"
    )

    add_executable(moq-interop-session-transport-header-tests
                   tests/transport/session_transport_header_test.cpp)
    target_link_libraries(moq-interop-session-transport-header-tests PRIVATE
                          moq-interop-app GTest::gtest_main)
    target_compile_options(moq-interop-session-transport-header-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME session-transport-header
             COMMAND moq-interop-session-transport-header-tests)

    add_executable(moq-interop-quiche-connection-tests
                   tests/transport/quiche_connection_test.cpp)
    target_include_directories(moq-interop-quiche-connection-tests PRIVATE
                               "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(moq-interop-quiche-connection-tests PRIVATE
                          moq-interop-transport moq-interop-quiche
                          GTest::gtest_main)
    target_compile_options(moq-interop-quiche-connection-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME quiche-connection COMMAND moq-interop-quiche-connection-tests)

    add_executable(moq-interop-native-quic-tests
                   tests/integration/native_quic_test.cpp
                   tests/support/quiche_client.cpp)
    target_include_directories(moq-interop-native-quic-tests PRIVATE
                               "${PROJECT_SOURCE_DIR}/src"
                               "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(moq-interop-native-quic-tests PRIVATE
                          moq-interop-transport moq-interop-quiche
                          moq-interop-scenarios
                          GTest::gtest_main)
    target_compile_options(moq-interop-native-quic-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME native-quic COMMAND moq-interop-native-quic-tests)

    add_executable(moq-interop-draft18-session-tests
                   tests/protocol/draft18_session_test.cpp)
    target_link_libraries(moq-interop-draft18-session-tests PRIVATE
                          moq-interop-session GTest::gtest_main)
    target_compile_options(moq-interop-draft18-session-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME draft18-session COMMAND moq-interop-draft18-session-tests)

    add_executable(moq-interop-draft18-scenarios-tests
                   tests/protocol/draft18_scenarios_test.cpp)
    target_link_libraries(moq-interop-draft18-scenarios-tests PRIVATE
                          moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft18-scenarios COMMAND moq-interop-draft18-scenarios-tests)

    add_executable(moq-interop-draft18-action-dispatcher-tests
                   tests/protocol/draft18_action_dispatcher_test.cpp)
    target_link_libraries(moq-interop-draft18-action-dispatcher-tests PRIVATE
                          moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft18-action-dispatcher
             COMMAND moq-interop-draft18-action-dispatcher-tests)

    add_executable(moq-interop-draft18-run-controller-tests
                   tests/protocol/draft18_run_controller_test.cpp)
    target_link_libraries(moq-interop-draft18-run-controller-tests PRIVATE
                          moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft18-run-controller
             COMMAND moq-interop-draft18-run-controller-tests)

    add_executable(moq-interop-draft18-evaluators-tests
                   tests/protocol/draft18_evaluators_test.cpp)
    target_compile_definitions(moq-interop-draft18-evaluators-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-evaluators-tests PRIVATE
                          moq-interop-draft18-evaluators GTest::gtest_main)
    add_test(NAME draft18-evaluators COMMAND moq-interop-draft18-evaluators-tests)

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

    add_executable(moq-interop-scoring-tests tests/unit/scoring_test.cpp)
    target_link_libraries(moq-interop-scoring-tests PRIVATE moq-interop-requirements
                          GTest::gtest_main)
    add_test(NAME scoring COMMAND moq-interop-scoring-tests)

    add_executable(moq-interop-run-store-tests tests/unit/run_store_test.cpp)
    target_link_libraries(moq-interop-run-store-tests PRIVATE moq-interop-storage
                          GTest::gtest_main)
    add_test(NAME run-store COMMAND moq-interop-run-store-tests)

    add_executable(moq-interop-http-api-tests tests/integration/http_api_test.cpp)
    target_compile_definitions(moq-interop-http-api-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-http-api-tests PRIVATE moq-interop-http
                          httplib::httplib nlohmann_json::nlohmann_json GTest::gtest_main)
    add_test(NAME http-api COMMAND moq-interop-http-api-tests)

    add_executable(moq-interop-cursor-tests tests/golden/cursor_test.cpp)
    target_link_libraries(moq-interop-cursor-tests PRIVATE moq-interop-wire GTest::gtest_main)
    target_compile_options(moq-interop-cursor-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME cursor COMMAND moq-interop-cursor-tests)

    add_executable(moq-interop-draft18-messages-tests
                   tests/golden/draft18_messages_test.cpp)
    target_link_libraries(moq-interop-draft18-messages-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    target_compile_options(moq-interop-draft18-messages-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME draft18-messages COMMAND moq-interop-draft18-messages-tests)

    add_executable(moq-interop-draft18-objects-tests
                   tests/golden/draft18_objects_test.cpp)
    target_link_libraries(moq-interop-draft18-objects-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    target_compile_options(moq-interop-draft18-objects-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME draft18-objects COMMAND moq-interop-draft18-objects-tests)

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

    add_executable(moq-interop-draft18-catalog-part-03-tests
                   tests/unit/draft18_catalog_part_03_test.cpp)
    target_compile_definitions(moq-interop-draft18-catalog-part-03-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-catalog-part-03-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft18-catalog-part-03 COMMAND moq-interop-draft18-catalog-part-03-tests)

    add_executable(moq-interop-draft18-catalog-part-04-tests
                   tests/unit/draft18_catalog_part_04_test.cpp)
    target_compile_definitions(moq-interop-draft18-catalog-part-04-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-catalog-part-04-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft18-catalog-part-04 COMMAND moq-interop-draft18-catalog-part-04-tests)

    add_executable(moq-interop-draft18-catalog-part-05-tests
                   tests/unit/draft18_catalog_part_05_test.cpp)
    target_compile_definitions(moq-interop-draft18-catalog-part-05-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-catalog-part-05-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft18-catalog-part-05 COMMAND moq-interop-draft18-catalog-part-05-tests)

    add_executable(moq-interop-draft18-catalog-part-06-tests
                   tests/unit/draft18_catalog_part_06_test.cpp)
    target_compile_definitions(moq-interop-draft18-catalog-part-06-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-catalog-part-06-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft18-catalog-part-06 COMMAND moq-interop-draft18-catalog-part-06-tests)

    add_executable(moq-interop-draft21-catalog-part-01-tests
                   tests/unit/draft21_catalog_part_01_test.cpp)
    target_compile_definitions(moq-interop-draft21-catalog-part-01-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft21-catalog-part-01-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft21-catalog-part-01 COMMAND moq-interop-draft21-catalog-part-01-tests)

    add_executable(moq-interop-draft21-catalog-part-02-tests
                   tests/unit/draft21_catalog_part_02_test.cpp)
    target_compile_definitions(moq-interop-draft21-catalog-part-02-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft21-catalog-part-02-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft21-catalog-part-02 COMMAND moq-interop-draft21-catalog-part-02-tests)

    add_executable(moq-interop-draft21-catalog-part-03-tests
                   tests/unit/draft21_catalog_part_03_test.cpp)
    target_compile_definitions(moq-interop-draft21-catalog-part-03-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft21-catalog-part-03-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft21-catalog-part-03 COMMAND moq-interop-draft21-catalog-part-03-tests)

    add_executable(moq-interop-draft21-catalog-part-04-tests
                   tests/unit/draft21_catalog_part_04_test.cpp)
    target_compile_definitions(moq-interop-draft21-catalog-part-04-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft21-catalog-part-04-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft21-catalog-part-04 COMMAND moq-interop-draft21-catalog-part-04-tests)

    add_executable(moq-interop-draft21-catalog-part-05-tests
                   tests/unit/draft21_catalog_part_05_test.cpp)
    target_compile_definitions(moq-interop-draft21-catalog-part-05-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft21-catalog-part-05-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft21-catalog-part-05 COMMAND moq-interop-draft21-catalog-part-05-tests)

    add_executable(moq-interop-draft21-catalog-part-06-tests
                   tests/unit/draft21_catalog_part_06_test.cpp)
    target_compile_definitions(moq-interop-draft21-catalog-part-06-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft21-catalog-part-06-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft21-catalog-part-06 COMMAND moq-interop-draft21-catalog-part-06-tests)

    add_executable(moq-interop-draft21-catalog-part-07-tests
                   tests/unit/draft21_catalog_part_07_test.cpp)
    target_compile_definitions(moq-interop-draft21-catalog-part-07-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft21-catalog-part-07-tests PRIVATE
                          moq-interop-requirements GTest::gtest_main)
    add_test(NAME draft21-catalog-part-07 COMMAND moq-interop-draft21-catalog-part-07-tests)
endif()

if(MOQ_INTEROP_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MOQT interop fuzz targets require Clang and libFuzzer")
    endif()

    add_executable(moq-interop-cursor-fuzz
        tests/fuzz/cursor_fuzz.cpp
        src/wire/common/cursor.cpp
    )
    target_link_libraries(moq-interop-cursor-fuzz PRIVATE moq-interop-app)
    target_compile_options(moq-interop-cursor-fuzz PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
    target_link_options(moq-interop-cursor-fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )

    add_executable(moq-interop-draft18-message-fuzz
        tests/fuzz/draft18_message_fuzz.cpp
        src/wire/common/cursor.cpp
        src/wire/draft18/messages.cpp
    )
    target_link_libraries(moq-interop-draft18-message-fuzz PRIVATE moq-interop-app)
    target_compile_options(moq-interop-draft18-message-fuzz PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
    target_link_options(moq-interop-draft18-message-fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )

    add_executable(moq-interop-draft18-object-fuzz
        tests/fuzz/draft18_object_fuzz.cpp
        src/wire/common/cursor.cpp
        src/wire/draft18/messages.cpp
        src/wire/draft18/objects.cpp
    )
    target_link_libraries(moq-interop-draft18-object-fuzz PRIVATE moq-interop-app)
    target_compile_options(moq-interop-draft18-object-fuzz PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
    target_link_options(moq-interop-draft18-object-fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
endif()
