cmake_minimum_required(VERSION 3.24)
project(moq_contribution_interop_runner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOQ_INTEROP_BUILD_TESTS "Build unit tests" ON)

include(cmake/Dependencies.cmake)

find_package(SQLite3 REQUIRED)
set(MOQ_INTEROP_SOURCE_REVISION "" CACHE STRING "Source commit revision")
if(NOT MOQ_INTEROP_SOURCE_REVISION)
    find_package(Git REQUIRED)
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

add_executable(moq-interop-runner src/app/main.cpp)
target_link_libraries(moq-interop-runner PRIVATE moq-interop-app)
install(TARGETS moq-interop-runner RUNTIME DESTINATION bin)

if(MOQ_INTEROP_BUILD_TESTS)
    enable_testing()
    FetchContent_MakeAvailable(googletest)

    add_executable(moq-interop-unit-tests tests/unit/version_test.cpp)
    target_link_libraries(moq-interop-unit-tests PRIVATE moq-interop-app GTest::gtest_main)
    add_test(NAME version COMMAND moq-interop-unit-tests)
endif()
