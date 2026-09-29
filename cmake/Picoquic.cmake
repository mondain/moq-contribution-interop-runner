set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "Fetch pinned picotls" FORCE)
set(PICOQUIC_FETCH_PTLS_TAG "${MOQ_INTEROP_PICOTLS_REVISION}" CACHE STRING
    "Pinned picotls revision" FORCE)
set(picoquic_BUILD_TESTS OFF CACHE BOOL "Build picoquic tests" FORCE)
set(BUILD_PICO_SIM OFF CACHE BOOL "Build picoquic simulator" FORCE)
set(BUILD_DEMO OFF CACHE BOOL "Build picoquic demo" FORCE)
set(BUILD_HTTP ON CACHE BOOL "Build H3zero" FORCE)
set(BUILD_LOGLIB ON CACHE BOOL "Build picoquic logging library" FORCE)
set(BUILD_PQBENCH OFF CACHE BOOL "Build picoquic benchmark" FORCE)
set(BUILD_LOGREADER OFF CACHE BOOL "Build picoquic log reader" FORCE)

FetchContent_GetProperties(picoquic)
if(NOT picoquic_POPULATED)
    FetchContent_Populate(picoquic)
endif()
set(MOQ_INTEROP_PICOQUIC_WT_PATCH
    "${PROJECT_SOURCE_DIR}/cmake/patches/picoquic-webtransport-strict.patch")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero --reverse --check
            "${MOQ_INTEROP_PICOQUIC_WT_PATCH}"
    WORKING_DIRECTORY "${picoquic_SOURCE_DIR}"
    RESULT_VARIABLE wt_patch_already_applied
    OUTPUT_QUIET ERROR_QUIET)
if(NOT wt_patch_already_applied EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero --check
                "${MOQ_INTEROP_PICOQUIC_WT_PATCH}"
        WORKING_DIRECTORY "${picoquic_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero
                "${MOQ_INTEROP_PICOQUIC_WT_PATCH}"
        WORKING_DIRECTORY "${picoquic_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
add_subdirectory("${picoquic_SOURCE_DIR}" "${picoquic_BINARY_DIR}")
FetchContent_GetProperties(picotls)

foreach(dependency picoquic picotls)
    string(TOUPPER "${dependency}" dependency_upper)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${${dependency}_SOURCE_DIR}"
        OUTPUT_VARIABLE actual_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT actual_revision STREQUAL MOQ_INTEROP_${dependency_upper}_REVISION)
        message(FATAL_ERROR
            "${dependency} revision mismatch: expected ${MOQ_INTEROP_${dependency_upper}_REVISION}, actual ${actual_revision}")
    endif()
endforeach()

add_library(moq-interop-picoquic INTERFACE)
target_link_libraries(moq-interop-picoquic INTERFACE
    picoquic::picoquic-core picoquic::picohttp-core)
