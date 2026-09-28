get_filename_component(MOQ_INTEROP_ROOT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MOQ_INTEROP_BORINGSSL_REVISION
    "f1c75347daa2ea81a941e953f2263e0a4d970c8d")
set(MOQ_INTEROP_QUICHE_LOCK_FILE
    "${MOQ_INTEROP_ROOT_DIR}/dependencies/quiche-0.24.9.Cargo.lock")

find_program(MOQ_INTEROP_CARGO_EXECUTABLE NAMES cargo REQUIRED)
find_package(Threads REQUIRED)

FetchContent_GetProperties(quiche)
if(NOT quiche_POPULATED)
    FetchContent_Populate(quiche)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DQUICHE_SOURCE_DIR=${quiche_SOURCE_DIR}"
        "-DEXPECTED_QUICHE_REVISION=${MOQ_INTEROP_QUICHE_REVISION}"
        "-DEXPECTED_BORINGSSL_REVISION=${MOQ_INTEROP_BORINGSSL_REVISION}"
        -P "${CMAKE_CURRENT_LIST_DIR}/VerifyQuiche.cmake"
    RESULT_VARIABLE quiche_verification_result
)
if(NOT quiche_verification_result EQUAL 0)
    message(FATAL_ERROR "pinned quiche checkout verification failed")
endif()

set(MOQ_INTEROP_QUICHE_CARGO_TARGET_DIR
    "${PROJECT_BINARY_DIR}/quiche-cargo-target")
set(MOQ_INTEROP_QUICHE_CARGO_HOME
    "${PROJECT_BINARY_DIR}/quiche-cargo-home")
set(MOQ_INTEROP_QUICHE_ARCHIVE
    "${MOQ_INTEROP_QUICHE_CARGO_TARGET_DIR}/release/libquiche.a")
set(MOQ_INTEROP_FETCHED_QUICHE_LOCK_FILE "${quiche_SOURCE_DIR}/Cargo.lock")
set(MOQ_INTEROP_BORINGSSL_INCLUDE_DIR
    "${quiche_SOURCE_DIR}/quiche/deps/boringssl/src/include")

add_custom_command(
    OUTPUT "${MOQ_INTEROP_QUICHE_ARCHIVE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${MOQ_INTEROP_QUICHE_LOCK_FILE}"
        "${MOQ_INTEROP_FETCHED_QUICHE_LOCK_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E env
        "CARGO_HOME=${MOQ_INTEROP_QUICHE_CARGO_HOME}"
        "CARGO_TARGET_DIR=${MOQ_INTEROP_QUICHE_CARGO_TARGET_DIR}"
        "${MOQ_INTEROP_CARGO_EXECUTABLE}" build
        --manifest-path "${quiche_SOURCE_DIR}/Cargo.toml"
        --package quiche
        --release
        --features ffi
        --locked
    DEPENDS "${MOQ_INTEROP_QUICHE_LOCK_FILE}"
    WORKING_DIRECTORY "${quiche_SOURCE_DIR}"
    COMMENT "Building pinned quiche ${MOQ_INTEROP_QUICHE_REVISION}"
    VERBATIM
)
add_custom_target(moq-interop-quiche-build
    DEPENDS "${MOQ_INTEROP_QUICHE_ARCHIVE}")

add_library(moq-interop-quiche-archive STATIC IMPORTED GLOBAL)
set_target_properties(moq-interop-quiche-archive PROPERTIES
    IMPORTED_LOCATION "${MOQ_INTEROP_QUICHE_ARCHIVE}"
)
add_dependencies(moq-interop-quiche-archive moq-interop-quiche-build)

add_library(moq-interop-quiche INTERFACE)
target_include_directories(moq-interop-quiche INTERFACE
    "${quiche_SOURCE_DIR}/quiche/include")
target_link_libraries(moq-interop-quiche INTERFACE
    moq-interop-quiche-archive
    Threads::Threads
    "${CMAKE_DL_LIBS}"
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(moq-interop-quiche INTERFACE m)
endif()
