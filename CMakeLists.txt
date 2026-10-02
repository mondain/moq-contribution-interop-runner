cmake_minimum_required(VERSION 3.24)
project(moq_contribution_interop_runner VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOQ_INTEROP_BUILD_TESTS "Build unit tests" ON)
option(MOQ_INTEROP_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)

include(cmake/Dependencies.cmake)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Git REQUIRED)
include(cmake/Picoquic.cmake)
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
    MOQ_INTEROP_PICOQUIC_REVISION="${MOQ_INTEROP_PICOQUIC_REVISION}"
    MOQ_INTEROP_PICOTLS_REVISION="${MOQ_INTEROP_PICOTLS_REVISION}"
    MOQ_INTEROP_NLOHMANN_JSON_REVISION="${MOQ_INTEROP_NLOHMANN_JSON_REVISION}"
    MOQ_INTEROP_CPP_HTTPLIB_REVISION="${MOQ_INTEROP_CPP_HTTPLIB_REVISION}"
    MOQ_INTEROP_GOOGLETEST_REVISION="${MOQ_INTEROP_GOOGLETEST_REVISION}"
    MOQ_INTEROP_SQLITE3_REVISION="${SQLite3_VERSION}"
)

add_library(moq-interop-publisher-driver src/app/publisher_driver.cpp)
target_link_libraries(moq-interop-publisher-driver PUBLIC moq-interop-app)
target_link_libraries(moq-interop-publisher-driver PRIVATE
    nlohmann_json::nlohmann_json OpenSSL::Crypto)

add_library(moq-interop-wire
    src/wire/common/cursor.cpp
    src/wire/draft18/messages.cpp
    src/wire/draft18/objects.cpp
    src/wire/draft21/message_types.cpp
    src/wire/draft21/control.cpp
    src/wire/draft21/goaway.cpp
    src/wire/draft21/key_values.cpp
    src/wire/draft21/objects.cpp
    src/wire/draft21/location_filter.cpp
    src/wire/draft21/publish.cpp
    src/wire/draft21/publish_done.cpp
    src/wire/draft21/publisher_request.cpp
    src/wire/draft21/request_frame.cpp
    src/wire/draft21/request_error.cpp
    src/wire/draft21/request_ok.cpp
    src/wire/draft21/successful_response.cpp
    src/wire/draft21/setup.cpp
    src/wire/draft21/token.cpp
)
target_link_libraries(moq-interop-wire PUBLIC moq-interop-app)
target_compile_options(moq-interop-wire PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-requirements
    src/requirements/draft_source.cpp
    src/requirements/catalog.cpp
    src/requirements/audit.cpp
    src/requirements/completeness.cpp
    src/requirements/scoring.cpp
)
target_link_libraries(moq-interop-requirements PUBLIC moq-interop-app)
target_link_libraries(moq-interop-requirements PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json)

add_library(moq-interop-storage src/storage/sqlite_run_store.cpp)
target_include_directories(moq-interop-storage PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(moq-interop-storage PUBLIC moq-interop-app moq-interop-requirements
                      SQLite::SQLite3)

add_library(moq-interop-execution-audit
    src/requirements/execution_audit.cpp)
target_link_libraries(moq-interop-execution-audit PUBLIC
    moq-interop-requirements moq-interop-storage)
target_link_libraries(moq-interop-execution-audit PRIVATE
    OpenSSL::Crypto nlohmann_json::nlohmann_json)

set(MOQ_INTEROP_HTTP_SOURCES
    src/http/server.cpp
    src/http/json.cpp
    src/http/result_schema.cpp
    src/http/tap.cpp
    src/http/report.cpp
    src/http/report_styles.cpp
)
add_library(moq-interop-http ${MOQ_INTEROP_HTTP_SOURCES})
target_link_libraries(moq-interop-http PUBLIC moq-interop-app moq-interop-requirements moq-interop-runs
                      moq-interop-storage moq-interop-execution-audit)
target_link_libraries(moq-interop-http PRIVATE httplib::httplib nlohmann_json::nlohmann_json)

add_library(moq-interop-transport
    src/transport/picoquic_connection.cpp
    src/transport/picoquic_native_listener.cpp
    src/transport/webtransport_connect.cpp
    src/transport/webtransport_session.cpp
    src/transport/webtransport_listener.cpp
)
target_include_directories(moq-interop-transport PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(moq-interop-transport PUBLIC
    moq-interop-app moq-interop-picoquic)
target_compile_options(moq-interop-transport PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-session
    src/session/publisher_session.cpp
    src/session/draft18_dispatch.cpp
    src/session/draft21_request_ids.cpp
    src/session/draft21_control_state.cpp
    src/session/draft21_publish_open.cpp
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
    src/scenarios/run_controller.cpp
    src/scenarios/raw_probe.cpp
    src/scenarios/raw_probe_courtesy.cpp
    src/scenarios/raw_probe_liveness.cpp
    src/scenarios/request_probe.cpp
    src/scenarios/draft18_request.cpp
    src/scenarios/draft18_response.cpp
    src/scenarios/fetch_probe.cpp
    src/scenarios/fetch_first_object.cpp
    src/scenarios/fetch_group_order.cpp
    src/scenarios/request_goaway.cpp
    src/scenarios/immutable_repeat.cpp
    src/scenarios/object_repeat.cpp
    src/scenarios/fetch_response.cpp
    src/scenarios/request_response.cpp
    src/scenarios/range_filter.cpp
    src/scenarios/subscription_cancel.cpp
    src/scenarios/discovery_overlap.cpp
    src/scenarios/draft18_peer_close.cpp
    src/scenarios/draft21_request.cpp
    src/scenarios/draft21_response.cpp
    src/scenarios/draft21_peer_close.cpp
    src/scenarios/draft21_close.cpp
    src/scenarios/draft18_close.cpp
    src/scenarios/draft21_announcement.cpp
    # draft-18 gap-A probes
    src/scenarios/draft18_gap_a.cpp
    src/scenarios/draft18_gap_a_common.cpp
    src/scenarios/draft18_gap_a_setup.cpp
    src/scenarios/draft18_gap_a_discovery.cpp
    src/scenarios/draft18_gap_a_object.cpp
    src/scenarios/draft18_gap_b.cpp
    src/scenarios/draft21_gap_a.cpp
    src/scenarios/draft21_gap_a_token.cpp
    src/scenarios/draft21_contribution.cpp
    src/scenarios/draft21_contribution_support.cpp
    src/scenarios/draft21_contribution_session.cpp
    src/scenarios/draft21_contribution_objects.cpp
    src/scenarios/draft21_contribution_d21b.cpp
    src/scenarios/draft21_contribution_residual.cpp
    src/scenarios/draft21_contribution_residual_wire.cpp
    src/scenarios/draft21_contribution_residual_subscription.cpp
    src/scenarios/draft21_contribution_residual_publisher.cpp
    src/scenarios/draft21_contribution_residual_token.cpp)
target_link_libraries(moq-interop-scenarios PUBLIC moq-interop-session)
# Draft-18 publisher-contribution probe families (gap-closing block).
target_sources(moq-interop-scenarios PRIVATE
    src/scenarios/draft18_contribution.cpp
    src/scenarios/draft18_contribution_setup.cpp
    src/scenarios/draft18_contribution_subscribe.cpp
    src/scenarios/draft18_contribution_publisher.cpp
    src/scenarios/draft18_contribution_objects.cpp
    src/scenarios/draft18_contribution_goaway.cpp
    src/scenarios/draft18_contribution_uri.cpp
    src/scenarios/draft18_contribution_closure.cpp
    src/scenarios/draft18_contribution_exchange.cpp
    src/scenarios/draft18_contribution_token.cpp)
# Draft-18 slice A: publisher-originated request, Key-Value-Pair, closure and Property probes.
target_sources(moq-interop-scenarios PRIVATE
    src/scenarios/draft18_contribution_origination.cpp)
target_compile_options(moq-interop-scenarios PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-draft18-evaluators
    src/requirements/draft18_evaluators.cpp)
target_link_libraries(moq-interop-draft18-evaluators PUBLIC
    moq-interop-requirements moq-interop-session moq-interop-scenarios)
target_compile_options(moq-interop-draft18-evaluators PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-draft21-evaluators
    src/requirements/draft21_evaluators.cpp
    src/requirements/draft21_gap_a.cpp)
target_link_libraries(moq-interop-draft21-evaluators PUBLIC
    moq-interop-requirements moq-interop-scenarios)
target_compile_options(moq-interop-draft21-evaluators PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(moq-interop-runs src/app/native_run_manager.cpp)
target_link_libraries(moq-interop-runs PUBLIC
    moq-interop-transport moq-interop-scenarios moq-interop-publisher-driver
    moq-interop-draft18-evaluators moq-interop-draft21-evaluators
    moq-interop-storage)
target_compile_options(moq-interop-runs PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>)

add_executable(moq-interop-runner src/app/main.cpp)
target_compile_definitions(moq-interop-runner PRIVATE
    MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(moq-interop-runner PRIVATE moq-interop-http
                      moq-interop-transport)
install(TARGETS moq-interop-runner RUNTIME DESTINATION bin)
add_executable(moq-interop-audit src/app/audit_main.cpp)
target_compile_definitions(moq-interop-audit PRIVATE
    MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(moq-interop-audit PRIVATE
    moq-interop-requirements moq-interop-draft18-evaluators
    moq-interop-draft21-evaluators moq-interop-execution-audit
    nlohmann_json::nlohmann_json)
install(TARGETS moq-interop-audit RUNTIME DESTINATION bin)
install(DIRECTORY docs/ DESTINATION share/moq-interop/docs
        FILES_MATCHING PATTERN "*.txt")
install(DIRECTORY requirements/ DESTINATION share/moq-interop/requirements
        FILES_MATCHING PATTERN "*.json" PATTERN "parts" EXCLUDE)

if(MOQ_INTEROP_BUILD_TESTS)
    enable_testing()
    FetchContent_MakeAvailable(googletest)

    add_executable(moq-interop-webtransport-connect-tests
                   tests/transport/webtransport_connect_test.cpp)
    target_include_directories(moq-interop-webtransport-connect-tests PRIVATE
                               "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(moq-interop-webtransport-connect-tests PRIVATE
                          moq-interop-transport GTest::gtest_main)
    add_test(NAME webtransport-connect COMMAND moq-interop-webtransport-connect-tests)
    add_executable(moq-interop-webtransport-h3-admission-tests
                   tests/integration/webtransport_h3_admission_test.cpp)
    target_link_libraries(moq-interop-webtransport-h3-admission-tests PRIVATE
                          moq-interop-picoquic GTest::gtest_main)
    add_test(NAME webtransport-h3-admission
             COMMAND moq-interop-webtransport-h3-admission-tests)
    add_executable(moq-interop-webtransport-scheme-tests
                   tests/integration/webtransport_scheme_test.cpp)
    target_link_libraries(moq-interop-webtransport-scheme-tests PRIVATE
                          moq-interop-picoquic GTest::gtest_main)
    add_test(NAME webtransport-scheme COMMAND moq-interop-webtransport-scheme-tests)
    add_executable(moq-interop-webtransport-session-tests
                   tests/transport/webtransport_session_test.cpp)
    target_include_directories(moq-interop-webtransport-session-tests PRIVATE
                               "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(moq-interop-webtransport-session-tests PRIVATE
                          moq-interop-transport GTest::gtest_main)
    add_test(NAME webtransport-session COMMAND moq-interop-webtransport-session-tests)
    add_executable(moq-interop-webtransport-listener-tests
                   tests/transport/webtransport_listener_test.cpp)
    target_compile_definitions(moq-interop-webtransport-listener-tests PRIVATE
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa")
    target_link_libraries(moq-interop-webtransport-listener-tests PRIVATE
                          moq-interop-transport GTest::gtest_main)
    add_test(NAME webtransport-listener COMMAND moq-interop-webtransport-listener-tests)

    add_executable(moq-interop-picoquic-link-tests
                   tests/integration/picoquic_link_test.cpp)
    target_link_libraries(moq-interop-picoquic-link-tests PRIVATE
                          moq-interop-picoquic)
    add_test(NAME picoquic-link COMMAND moq-interop-picoquic-link-tests)
    add_library(moq-interop-test-client tests/support/picoquic_client.cpp)
    target_include_directories(moq-interop-test-client PUBLIC "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(moq-interop-test-client PUBLIC moq-interop-picoquic)
    target_compile_options(moq-interop-test-client PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>)
    add_executable(moq-interop-picoquic-peer tests/support/picoquic_peer_main.cpp)
    target_link_libraries(moq-interop-picoquic-peer PRIVATE moq-interop-test-client)
    add_executable(moq-interop-picoquic-native-tests
                   tests/transport/picoquic_native_listener_test.cpp)
    target_compile_definitions(moq-interop-picoquic-native-tests PRIVATE
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa"
        PICOQUIC_TEST_PEER_PATH="$<TARGET_FILE:moq-interop-picoquic-peer>")
    target_link_libraries(moq-interop-picoquic-native-tests PRIVATE
        moq-interop-app moq-interop-transport GTest::gtest_main)
    add_dependencies(moq-interop-picoquic-native-tests moq-interop-picoquic-peer)
    add_test(NAME picoquic-native-listener COMMAND moq-interop-picoquic-native-tests)
    foreach(draft IN ITEMS 18 21)
        add_test(NAME draft${draft}-native
                 COMMAND bash "${PROJECT_SOURCE_DIR}/tests/e2e/native-peer.sh"
                         "$<TARGET_FILE:moq-interop-runner>"
                         "$<TARGET_FILE:moq-interop-picoquic-peer>" ${draft})
    endforeach()
    set_tests_properties(draft18-native draft21-native
                         PROPERTIES RESOURCE_LOCK "native-test-ports")
    add_executable(moq-interop-native-quic-tests tests/integration/native_quic_test.cpp)
    target_link_libraries(moq-interop-native-quic-tests PRIVATE
        moq-interop-test-client moq-interop-runs moq-interop-http
        httplib::httplib nlohmann_json::nlohmann_json GTest::gtest_main)
    target_compile_definitions(moq-interop-native-quic-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_compile_options(moq-interop-native-quic-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>)
    add_test(NAME native-quic COMMAND moq-interop-native-quic-tests)
    # Draft-18 publisher-contribution probes through the real run manager.
    add_executable(moq-interop-draft18-contribution-live-tests
                   tests/integration/draft18_contribution_live_test.cpp)
    target_link_libraries(moq-interop-draft18-contribution-live-tests PRIVATE
        moq-interop-test-client moq-interop-runs GTest::gtest_main)
    target_compile_definitions(moq-interop-draft18-contribution-live-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa")
    add_test(NAME draft18-contribution-live COMMAND moq-interop-draft18-contribution-live-tests)
    set_tests_properties(draft18-contribution-live PROPERTIES RESOURCE_LOCK "native-test-ports")

    add_executable(moq-interop-draft21-gap-a-live-tests
                   tests/integration/draft21_gap_a_live_test.cpp)
    target_link_libraries(moq-interop-draft21-gap-a-live-tests PRIVATE
        moq-interop-test-client moq-interop-runs moq-interop-http
        httplib::httplib nlohmann_json::nlohmann_json GTest::gtest_main)
    target_compile_definitions(moq-interop-draft21-gap-a-live-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa")
    add_test(NAME draft21-gap-a-live COMMAND moq-interop-draft21-gap-a-live-tests)
    add_executable(moq-interop-draft21-d21b-live-tests
                   tests/integration/draft21_d21b_live_test.cpp)
    target_link_libraries(moq-interop-draft21-d21b-live-tests PRIVATE
        moq-interop-test-client moq-interop-runs moq-interop-http
        httplib::httplib nlohmann_json::nlohmann_json GTest::gtest_main)
    target_compile_definitions(moq-interop-draft21-d21b-live-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa")
    add_test(NAME draft21-d21b-live COMMAND moq-interop-draft21-d21b-live-tests)
    add_executable(moq-interop-raw-family-driver-peer tests/support/raw_family_driver_peer.cpp)
    target_link_libraries(moq-interop-raw-family-driver-peer PRIVATE
        moq-interop-test-client nlohmann_json::nlohmann_json)
    add_executable(moq-interop-raw-family-driver-tests tests/integration/raw_family_driver_test.cpp)
    target_link_libraries(moq-interop-raw-family-driver-tests PRIVATE
        moq-interop-runs nlohmann_json::nlohmann_json GTest::gtest_main)
    target_compile_definitions(moq-interop-raw-family-driver-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa"
        PICOQUIC_FAMILY_DRIVER_PATH="$<TARGET_FILE:moq-interop-raw-family-driver-peer>")
    add_dependencies(moq-interop-raw-family-driver-tests moq-interop-raw-family-driver-peer)
    target_compile_options(moq-interop-raw-family-driver-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>)
    add_test(NAME raw-family-driver COMMAND moq-interop-raw-family-driver-tests)
    add_test(NAME production-link
             COMMAND bash "${PROJECT_SOURCE_DIR}/tests/integration/production-link.sh"
                     "$<TARGET_FILE:moq-interop-runner>")
    add_test(NAME picoquic-dependency-contract
        COMMAND "${CMAKE_COMMAND}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DPICOQUIC_SOURCE_DIR=${picoquic_SOURCE_DIR}"
            "-DPICOTLS_SOURCE_DIR=${picotls_SOURCE_DIR}"
            "-DEXPECTED_PICOQUIC_REVISION=${MOQ_INTEROP_PICOQUIC_REVISION}"
            "-DEXPECTED_PICOTLS_REVISION=${MOQ_INTEROP_PICOTLS_REVISION}"
            "-DWEBTRANSPORT_PATCHES=${MOQ_INTEROP_PICOQUIC_WT_PATCHES}"
            -P "${PROJECT_SOURCE_DIR}/tests/integration/picoquic_dependency_contract.cmake")
    add_test(NAME picoquic-dependency-rejects-mismatch
        COMMAND "${CMAKE_COMMAND}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DPICOQUIC_SOURCE_DIR=${picoquic_SOURCE_DIR}"
            "-DPICOTLS_SOURCE_DIR=${picotls_SOURCE_DIR}"
            "-DEXPECTED_PICOQUIC_REVISION=0000000000000000000000000000000000000000"
            "-DEXPECTED_PICOTLS_REVISION=${MOQ_INTEROP_PICOTLS_REVISION}"
            "-DWEBTRANSPORT_PATCHES=${MOQ_INTEROP_PICOQUIC_WT_PATCHES}"
            -P "${PROJECT_SOURCE_DIR}/tests/integration/picoquic_dependency_contract.cmake")
    set_tests_properties(picoquic-dependency-rejects-mismatch PROPERTIES
                         WILL_FAIL TRUE)


    add_executable(moq-interop-session-transport-header-tests
                   tests/transport/session_transport_header_test.cpp)
    target_link_libraries(moq-interop-session-transport-header-tests PRIVATE
                          moq-interop-app GTest::gtest_main)
    target_compile_options(moq-interop-session-transport-header-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME session-transport-header
             COMMAND moq-interop-session-transport-header-tests)


    # draft-18 gap-A probe tests
    foreach(gap_a_test IN ITEMS setup discovery object registry)
        add_executable(moq-interop-draft18-gap-a-${gap_a_test}-tests
            tests/protocol/draft18_gap_a_${gap_a_test}_test.cpp)
        target_include_directories(moq-interop-draft18-gap-a-${gap_a_test}-tests PRIVATE
            "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(moq-interop-draft18-gap-a-${gap_a_test}-tests PRIVATE
            moq-interop-scenarios GTest::gtest_main)
        add_test(NAME draft18-gap-a-${gap_a_test}
                 COMMAND moq-interop-draft18-gap-a-${gap_a_test}-tests)
    endforeach()

    add_executable(moq-interop-draft18-gap-b-tests
        tests/protocol/draft18_gap_b_test.cpp)
    target_include_directories(moq-interop-draft18-gap-b-tests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(moq-interop-draft18-gap-b-tests PRIVATE
        moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft18-gap-b COMMAND moq-interop-draft18-gap-b-tests)

    add_executable(moq-interop-draft18-gap-a-native-tests
        tests/integration/draft18_gap_a_native_test.cpp)
    target_link_libraries(moq-interop-draft18-gap-a-native-tests PRIVATE
        moq-interop-test-client moq-interop-runs GTest::gtest_main)
    target_compile_definitions(moq-interop-draft18-gap-a-native-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa")
    add_test(NAME draft18-gap-a-native COMMAND moq-interop-draft18-gap-a-native-tests)
    set_tests_properties(draft18-gap-a-native PROPERTIES RESOURCE_LOCK "native-test-ports")

    add_executable(moq-interop-draft18-gap-b-native-tests
        tests/integration/draft18_gap_b_native_test.cpp)
    target_link_libraries(moq-interop-draft18-gap-b-native-tests PRIVATE
        moq-interop-test-client moq-interop-runs GTest::gtest_main)
    target_compile_definitions(moq-interop-draft18-gap-b-native-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa")
    add_test(NAME draft18-gap-b-native COMMAND moq-interop-draft18-gap-b-native-tests)
    set_tests_properties(draft18-gap-b-native PROPERTIES RESOURCE_LOCK "native-test-ports")

    add_executable(moq-interop-publisher-capability-tagging-tests
                   tests/protocol/publisher_capability_tagging_test.cpp)
    target_link_libraries(moq-interop-publisher-capability-tagging-tests PRIVATE
        moq-interop-runs GTest::gtest_main)
    add_test(NAME publisher-capability-tagging
             COMMAND moq-interop-publisher-capability-tagging-tests)

    add_executable(moq-interop-fetch-probe-tests tests/protocol/fetch_probe_test.cpp)
    target_link_libraries(moq-interop-fetch-probe-tests PRIVATE moq-interop-scenarios moq-interop-requirements moq-interop-draft18-evaluators moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-fetch-probe-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME fetch-probe COMMAND moq-interop-fetch-probe-tests)

    add_executable(moq-interop-request-goaway-tests tests/protocol/request_goaway_test.cpp)
    target_link_libraries(moq-interop-request-goaway-tests PRIVATE moq-interop-scenarios moq-interop-requirements moq-interop-draft18-evaluators moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-request-goaway-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME request-goaway COMMAND moq-interop-request-goaway-tests)

    foreach(profile IN ITEMS immutable-repeat object-repeat)
        string(REPLACE "-" "_" profile_source "${profile}")
        add_executable(moq-interop-${profile}-tests tests/protocol/${profile_source}_test.cpp)
        target_link_libraries(moq-interop-${profile}-tests PRIVATE
            moq-interop-scenarios moq-interop-requirements
            moq-interop-draft18-evaluators moq-interop-draft21-evaluators GTest::gtest_main)
        target_compile_definitions(moq-interop-${profile}-tests PRIVATE
            MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
        add_test(NAME ${profile} COMMAND moq-interop-${profile}-tests)
    endforeach()

    add_executable(moq-interop-fetch-group-order-tests tests/protocol/fetch_group_order_test.cpp)
    target_link_libraries(moq-interop-fetch-group-order-tests PRIVATE moq-interop-scenarios moq-interop-requirements moq-interop-draft18-evaluators moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-fetch-group-order-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME fetch-group-order COMMAND moq-interop-fetch-group-order-tests)

    add_executable(moq-interop-fetch-first-object-tests tests/protocol/fetch_first_object_test.cpp)
    target_link_libraries(moq-interop-fetch-first-object-tests PRIVATE moq-interop-scenarios moq-interop-requirements moq-interop-draft18-evaluators moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-fetch-first-object-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME fetch-first-object COMMAND moq-interop-fetch-first-object-tests)

    add_executable(moq-interop-draft21-objects-tests tests/protocol/draft21_objects_test.cpp)
    target_link_libraries(moq-interop-draft21-objects-tests PRIVATE moq-interop-wire GTest::gtest_main)
    target_compile_options(moq-interop-draft21-objects-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>)
    add_test(NAME draft21-objects COMMAND moq-interop-draft21-objects-tests)

    add_executable(moq-interop-range-filter-tests tests/protocol/range_filter_test.cpp)
    target_link_libraries(moq-interop-range-filter-tests PRIVATE moq-interop-scenarios moq-interop-requirements moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-range-filter-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME range-filter COMMAND moq-interop-range-filter-tests)

    add_executable(moq-interop-request-response-tests tests/protocol/request_response_test.cpp)
    target_link_libraries(moq-interop-request-response-tests PRIVATE moq-interop-scenarios moq-interop-requirements moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-request-response-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME request-response COMMAND moq-interop-request-response-tests)

    add_executable(moq-interop-fetch-response-tests tests/protocol/fetch_response_test.cpp)
    target_link_libraries(moq-interop-fetch-response-tests PRIVATE moq-interop-scenarios moq-interop-requirements moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-fetch-response-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME fetch-response COMMAND moq-interop-fetch-response-tests)

    add_executable(moq-interop-subscription-cancel-tests tests/protocol/subscription_cancel_test.cpp)
    target_link_libraries(moq-interop-subscription-cancel-tests PRIVATE moq-interop-scenarios
        moq-interop-requirements moq-interop-draft18-evaluators moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-subscription-cancel-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME subscription-cancel COMMAND moq-interop-subscription-cancel-tests)
    add_executable(moq-interop-discovery-overlap-tests tests/protocol/discovery_overlap_test.cpp)
    target_link_libraries(moq-interop-discovery-overlap-tests PRIVATE moq-interop-scenarios
        moq-interop-requirements moq-interop-draft18-evaluators moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-discovery-overlap-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME discovery-overlap COMMAND moq-interop-discovery-overlap-tests)

    add_executable(moq-interop-draft18-session-tests
                   tests/protocol/draft18_session_test.cpp)
    target_link_libraries(moq-interop-draft18-session-tests PRIVATE
                          moq-interop-session GTest::gtest_main)
    target_compile_options(moq-interop-draft18-session-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME draft18-session COMMAND moq-interop-draft18-session-tests)

    add_executable(moq-interop-request-probe-tests tests/protocol/request_probe_test.cpp)
    target_link_libraries(moq-interop-request-probe-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME request-probe COMMAND moq-interop-request-probe-tests)

    add_executable(moq-interop-draft18-request-tests tests/protocol/draft18_request_test.cpp)
    target_link_libraries(moq-interop-draft18-request-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft18-request COMMAND moq-interop-draft18-request-tests)

    add_executable(moq-interop-draft18-response-tests tests/protocol/draft18_response_test.cpp)
    target_link_libraries(moq-interop-draft18-response-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft18-response COMMAND moq-interop-draft18-response-tests)

    # Draft-18 publisher-contribution probe families (gap-closing block).
    add_executable(moq-interop-draft18-contribution-tests tests/protocol/draft18_contribution_test.cpp
        tests/protocol/draft18_contribution_subscribe_test.cpp
        tests/protocol/draft18_contribution_publisher_test.cpp
        tests/protocol/draft18_contribution_objects_test.cpp
        tests/protocol/draft18_contribution_goaway_test.cpp
        tests/protocol/draft18_contribution_uri_test.cpp
        tests/protocol/draft18_contribution_closure_test.cpp
        tests/protocol/draft18_contribution_exchange_test.cpp
        tests/protocol/draft18_contribution_token_test.cpp)
    target_link_libraries(moq-interop-draft18-contribution-tests PRIVATE moq-interop-scenarios
        moq-interop-requirements moq-interop-draft18-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-draft18-contribution-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME draft18-contribution COMMAND moq-interop-draft18-contribution-tests)

    add_executable(moq-interop-raw-probe-auto-accept-tests tests/protocol/raw_probe_auto_accept_test.cpp)
    target_link_libraries(moq-interop-raw-probe-auto-accept-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe-auto-accept COMMAND moq-interop-raw-probe-auto-accept-tests)
    add_executable(moq-interop-raw-probe-default-announce-tests tests/protocol/raw_probe_default_announce_test.cpp)
    target_link_libraries(moq-interop-raw-probe-default-announce-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe-default-announce COMMAND moq-interop-raw-probe-default-announce-tests)

    add_executable(moq-interop-draft18-origination-tests tests/protocol/draft18_contribution_origination_test.cpp
        tests/protocol/draft18_contribution_empty_host_test.cpp)
    target_link_libraries(moq-interop-draft18-origination-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft18-origination COMMAND moq-interop-draft18-origination-tests)
    add_executable(moq-interop-raw-probe-peer-stop-tests tests/protocol/raw_probe_peer_stop_test.cpp)
    target_link_libraries(moq-interop-raw-probe-peer-stop-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe-peer-stop COMMAND moq-interop-raw-probe-peer-stop-tests)

    add_executable(moq-interop-raw-probe-alternate-tests tests/protocol/raw_probe_alternate_test.cpp)
    target_link_libraries(moq-interop-raw-probe-alternate-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe-alternate COMMAND moq-interop-raw-probe-alternate-tests)

    add_executable(moq-interop-draft18-peer-close-tests tests/protocol/draft18_peer_close_test.cpp)
    target_link_libraries(moq-interop-draft18-peer-close-tests PRIVATE moq-interop-scenarios moq-interop-requirements GTest::gtest_main)
    target_compile_definitions(moq-interop-draft18-peer-close-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME draft18-peer-close COMMAND moq-interop-draft18-peer-close-tests)

    add_executable(moq-interop-draft21-request-tests tests/protocol/draft21_request_test.cpp)
    target_link_libraries(moq-interop-draft21-request-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft21-request COMMAND moq-interop-draft21-request-tests)

    add_executable(moq-interop-draft21-response-tests tests/protocol/draft21_response_test.cpp)
    target_link_libraries(moq-interop-draft21-response-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft21-response COMMAND moq-interop-draft21-response-tests)

    add_executable(moq-interop-draft21-peer-close-tests tests/protocol/draft21_peer_close_test.cpp)
    target_link_libraries(moq-interop-draft21-peer-close-tests PRIVATE moq-interop-scenarios moq-interop-requirements GTest::gtest_main)
    target_compile_definitions(moq-interop-draft21-peer-close-tests PRIVATE MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME draft21-peer-close COMMAND moq-interop-draft21-peer-close-tests)

    add_executable(moq-interop-draft21-close-tests tests/protocol/draft21_close_test.cpp)
    target_link_libraries(moq-interop-draft21-close-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft21-close COMMAND moq-interop-draft21-close-tests)

    add_executable(moq-interop-raw-probe-tests tests/protocol/raw_probe_test.cpp)
    target_link_libraries(moq-interop-raw-probe-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe COMMAND moq-interop-raw-probe-tests)

    add_executable(moq-interop-raw-probe-liveness-tests tests/protocol/raw_probe_liveness_test.cpp)
    target_link_libraries(moq-interop-raw-probe-liveness-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe-liveness COMMAND moq-interop-raw-probe-liveness-tests)

    add_executable(moq-interop-raw-probe-close-reaction-tests tests/protocol/raw_probe_close_reaction_test.cpp)
    target_link_libraries(moq-interop-raw-probe-close-reaction-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe-close-reaction COMMAND moq-interop-raw-probe-close-reaction-tests)

    add_executable(moq-interop-raw-probe-courtesy-tests tests/protocol/raw_probe_courtesy_test.cpp)
    target_link_libraries(moq-interop-raw-probe-courtesy-tests PRIVATE moq-interop-scenarios GTest::gtest_main)
    add_test(NAME raw-probe-courtesy COMMAND moq-interop-raw-probe-courtesy-tests)

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

    add_executable(moq-interop-draft21-announcement-tests
                   tests/protocol/draft21_announcement_test.cpp)
    target_link_libraries(moq-interop-draft21-announcement-tests PRIVATE
                          moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft21-announcement
             COMMAND moq-interop-draft21-announcement-tests)

    add_executable(moq-interop-draft18-evaluators-tests
                   tests/protocol/draft18_evaluators_test.cpp)
    target_compile_definitions(moq-interop-draft18-evaluators-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(moq-interop-draft18-evaluators-tests PRIVATE
                          moq-interop-draft18-evaluators GTest::gtest_main)
    add_test(NAME draft18-evaluators COMMAND moq-interop-draft18-evaluators-tests)

    add_executable(moq-interop-draft21-evaluators-tests
                   tests/protocol/draft21_evaluators_test.cpp)
    target_compile_definitions(moq-interop-draft21-evaluators-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(moq-interop-draft21-evaluators-tests PRIVATE
        moq-interop-draft21-evaluators GTest::gtest_main)
    add_test(NAME draft21-evaluators
             COMMAND moq-interop-draft21-evaluators-tests)

    # Draft-21 completeness-gap slice A.
    add_executable(moq-interop-draft21-gap-a-tests
                   tests/protocol/draft21_gap_a_test.cpp)
    target_compile_definitions(moq-interop-draft21-gap-a-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(moq-interop-draft21-gap-a-tests PRIVATE
        moq-interop-scenarios moq-interop-requirements
        moq-interop-draft21-evaluators GTest::gtest_main)
    add_test(NAME draft21-gap-a COMMAND moq-interop-draft21-gap-a-tests)

    add_executable(moq-interop-draft21-gap-a-controller-tests
                   tests/protocol/draft21_gap_a_controller_test.cpp)
    target_link_libraries(moq-interop-draft21-gap-a-controller-tests PRIVATE
                          moq-interop-scenarios GTest::gtest_main)
    add_test(NAME draft21-gap-a-controller
             COMMAND moq-interop-draft21-gap-a-controller-tests)

    add_executable(moq-interop-draft21-gap-a-probe-tests
                   tests/protocol/draft21_gap_a_probe_test.cpp)
    target_compile_definitions(moq-interop-draft21-gap-a-probe-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(moq-interop-draft21-gap-a-probe-tests PRIVATE
        moq-interop-scenarios moq-interop-requirements
        moq-interop-draft21-evaluators GTest::gtest_main)
    add_test(NAME draft21-gap-a-probe COMMAND moq-interop-draft21-gap-a-probe-tests)

    add_executable(moq-interop-draft21-gap-a-token-tests
                   tests/protocol/draft21_gap_a_token_test.cpp)
    target_compile_definitions(moq-interop-draft21-gap-a-token-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(moq-interop-draft21-gap-a-token-tests PRIVATE
        moq-interop-scenarios moq-interop-requirements moq-interop-draft21-evaluators
        GTest::gtest_main)
    add_test(NAME draft21-gap-a-token COMMAND moq-interop-draft21-gap-a-token-tests)

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

    add_executable(moq-interop-completeness-tests
                   tests/unit/completeness_test.cpp)
    target_compile_definitions(moq-interop-completeness-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(moq-interop-completeness-tests PRIVATE
        moq-interop-requirements moq-interop-draft18-evaluators
        moq-interop-draft21-evaluators GTest::gtest_main)
    add_test(NAME completeness COMMAND moq-interop-completeness-tests)

    add_executable(moq-interop-execution-audit-tests
                   tests/integration/execution_audit_test.cpp)
    target_link_libraries(moq-interop-execution-audit-tests PRIVATE
        moq-interop-execution-audit GTest::gtest_main)
    add_test(NAME execution-audit COMMAND moq-interop-execution-audit-tests)
    add_test(NAME audit-cli-smoke
             COMMAND bash "${PROJECT_SOURCE_DIR}/tests/e2e/audit-cli-smoke.sh"
                     "$<TARGET_FILE:moq-interop-runner>"
                     "$<TARGET_FILE:moq-interop-audit>")
    set_tests_properties(audit-cli-smoke PROPERTIES RESOURCE_LOCK "audit-cli-port")
    add_test(NAME release-audit-check-contract
             COMMAND bash "${PROJECT_SOURCE_DIR}/tests/e2e/release-audit-contract.sh"
                     --check-only)

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

    add_executable(moq-interop-result-export-tests
                   tests/integration/result_export_test.cpp)
    target_link_libraries(moq-interop-result-export-tests PRIVATE
        moq-interop-http nlohmann_json::nlohmann_json GTest::gtest_main)
    add_test(NAME result-export COMMAND moq-interop-result-export-tests)

    add_executable(moq-interop-report-accessibility-tests
                   tests/integration/report_accessibility_test.cpp)
    target_include_directories(moq-interop-report-accessibility-tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/http")
    target_link_libraries(moq-interop-report-accessibility-tests PRIVATE
        moq-interop-http GTest::gtest_main)
    add_test(NAME report-accessibility COMMAND moq-interop-report-accessibility-tests)

    add_executable(moq-interop-publisher-driver-tests
                   tests/unit/publisher_driver_test.cpp)
    target_compile_definitions(moq-interop-publisher-driver-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(moq-interop-publisher-driver-tests PRIVATE
        moq-interop-publisher-driver nlohmann_json::nlohmann_json
        GTest::gtest_main)
    add_test(NAME publisher-driver COMMAND moq-interop-publisher-driver-tests)
    add_test(NAME moqxr-adapter-contract
             COMMAND bash "${PROJECT_SOURCE_DIR}/tests/e2e/moqxr-adapter-contract.sh")

    add_executable(moq-interop-webtransport-run-api-tests
                   tests/integration/webtransport_run_api_test.cpp)
    target_compile_definitions(moq-interop-webtransport-run-api-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PICOQUIC_TEST_CERT_DIR="${picoquic_SOURCE_DIR}/certs/ecdsa")
    target_link_libraries(moq-interop-webtransport-run-api-tests PRIVATE
                          moq-interop-http moq-interop-execution-audit
                          httplib::httplib
                          nlohmann_json::nlohmann_json GTest::gtest_main)
    add_test(NAME webtransport-run-api COMMAND moq-interop-webtransport-run-api-tests)

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

    add_executable(moq-interop-draft21-setup-tests
                   tests/golden/draft21_setup_test.cpp)
    target_link_libraries(moq-interop-draft21-setup-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    target_compile_options(moq-interop-draft21-setup-tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME draft21-setup COMMAND moq-interop-draft21-setup-tests)

    add_executable(moq-interop-draft21-message-types-tests
                   tests/golden/draft21_message_types_test.cpp)
    target_link_libraries(moq-interop-draft21-message-types-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-message-types
             COMMAND moq-interop-draft21-message-types-tests)

    add_executable(moq-interop-draft21-goaway-tests
                   tests/golden/draft21_goaway_test.cpp)
    target_link_libraries(moq-interop-draft21-goaway-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-goaway COMMAND moq-interop-draft21-goaway-tests)

    add_executable(moq-interop-draft21-control-tests
                   tests/golden/draft21_control_test.cpp)
    target_link_libraries(moq-interop-draft21-control-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-control COMMAND moq-interop-draft21-control-tests)

    add_executable(moq-interop-draft21-request-frame-tests
                   tests/golden/draft21_request_frame_test.cpp)
    target_link_libraries(moq-interop-draft21-request-frame-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-request-frame
             COMMAND moq-interop-draft21-request-frame-tests)

    add_executable(moq-interop-draft21-key-values-tests
                   tests/golden/draft21_key_values_test.cpp)
    target_link_libraries(moq-interop-draft21-key-values-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-key-values
             COMMAND moq-interop-draft21-key-values-tests)

    add_executable(moq-interop-draft21-location-filter-tests
                   tests/golden/draft21_location_filter_test.cpp)
    target_link_libraries(moq-interop-draft21-location-filter-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-location-filter
             COMMAND moq-interop-draft21-location-filter-tests)

    add_executable(moq-interop-draft21-publish-tests
                   tests/golden/draft21_publish_test.cpp)
    target_link_libraries(moq-interop-draft21-publish-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-publish COMMAND moq-interop-draft21-publish-tests)

    add_executable(moq-interop-draft21-token-tests
                   tests/golden/draft21_token_test.cpp)
    target_link_libraries(moq-interop-draft21-token-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-token COMMAND moq-interop-draft21-token-tests)

    add_executable(moq-interop-draft21-request-error-tests
                   tests/golden/draft21_request_error_test.cpp)
    target_link_libraries(moq-interop-draft21-request-error-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-request-error
             COMMAND moq-interop-draft21-request-error-tests)

    add_executable(moq-interop-draft21-request-ok-tests
                   tests/golden/draft21_request_ok_test.cpp)
    target_link_libraries(moq-interop-draft21-request-ok-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-request-ok
             COMMAND moq-interop-draft21-request-ok-tests)

    add_executable(moq-interop-draft21-publisher-request-tests
                   tests/golden/draft21_publisher_request_test.cpp)
    target_link_libraries(moq-interop-draft21-publisher-request-tests PRIVATE
                          moq-interop-wire GTest::gtest_main)
    add_test(NAME draft21-publisher-request
             COMMAND moq-interop-draft21-publisher-request-tests)

    add_executable(moq-interop-draft21-request-ids-tests
                   tests/protocol/draft21_request_ids_test.cpp)
    target_link_libraries(moq-interop-draft21-request-ids-tests PRIVATE
                          moq-interop-session GTest::gtest_main)
    add_test(NAME draft21-request-ids
             COMMAND moq-interop-draft21-request-ids-tests)

    add_executable(moq-interop-draft21-control-state-tests
                   tests/protocol/draft21_control_state_test.cpp)
    target_link_libraries(moq-interop-draft21-control-state-tests PRIVATE
                          moq-interop-session GTest::gtest_main)
    add_test(NAME draft21-control-state
             COMMAND moq-interop-draft21-control-state-tests)

    add_executable(moq-interop-draft21-publish-open-tests
                   tests/protocol/draft21_publish_open_test.cpp)
    target_link_libraries(moq-interop-draft21-publish-open-tests PRIVATE
                          moq-interop-session GTest::gtest_main)
    add_test(NAME draft21-publish-open
             COMMAND moq-interop-draft21-publish-open-tests)

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
    # Draft-21 contribution profiles (session and object behavior).
    add_executable(moq-interop-draft21-contribution-tests
        tests/protocol/draft21_contribution_test.cpp
        tests/protocol/draft21_contribution_objects_test.cpp
        tests/protocol/draft21_contribution_d21b_test.cpp
        tests/protocol/draft21_contribution_residual_test.cpp)
    target_link_libraries(moq-interop-draft21-contribution-tests PRIVATE
        moq-interop-scenarios moq-interop-requirements moq-interop-draft21-evaluators GTest::gtest_main)
    target_compile_definitions(moq-interop-draft21-contribution-tests PRIVATE
        MOQ_INTEROP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME draft21-contribution COMMAND moq-interop-draft21-contribution-tests)
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

    add_executable(moq-interop-webtransport-stream-fuzz
        tests/fuzz/webtransport_stream_fuzz.cpp
    )
    target_include_directories(moq-interop-webtransport-stream-fuzz PRIVATE src)
    target_link_libraries(moq-interop-webtransport-stream-fuzz PRIVATE
        moq-interop-transport)
    target_compile_options(moq-interop-webtransport-stream-fuzz PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
    target_link_options(moq-interop-webtransport-stream-fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
endif()
