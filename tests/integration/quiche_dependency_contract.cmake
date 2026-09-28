foreach(required IN ITEMS GIT_EXECUTABLE QUICHE_SOURCE_DIR PROJECT_LOCK_FILE
                          FETCHED_LOCK_FILE VERIFY_SCRIPT QUICHE_REVISION
                          BORINGSSL_REVISION CARGO_EXECUTABLE CARGO_HOME
                          CARGO_TARGET_DIR REAL_SOURCE_DIR FIXTURE_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

function(run_verifier expected_quiche expected_boringssl expected_success)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DQUICHE_SOURCE_DIR=${QUICHE_SOURCE_DIR}"
            "-DEXPECTED_QUICHE_REVISION=${expected_quiche}"
            "-DEXPECTED_BORINGSSL_REVISION=${expected_boringssl}"
            -P "${VERIFY_SCRIPT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(expected_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "valid dependency checkout was rejected: ${output}${error}")
    endif()
    if(NOT expected_success AND result EQUAL 0)
        message(FATAL_ERROR "mismatched dependency checkout was accepted")
    endif()
endfunction()

run_verifier("${QUICHE_REVISION}" "${BORINGSSL_REVISION}" TRUE)
run_verifier("0000000000000000000000000000000000000000"
             "${BORINGSSL_REVISION}" FALSE)
run_verifier("${QUICHE_REVISION}"
             "0000000000000000000000000000000000000000" FALSE)

file(SHA256 "${PROJECT_LOCK_FILE}" project_lock_before)
file(SHA256 "${FETCHED_LOCK_FILE}" fetched_lock_before)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "CARGO_HOME=${CARGO_HOME}"
        "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
        "${CARGO_EXECUTABLE}" build
        --manifest-path "${QUICHE_SOURCE_DIR}/Cargo.toml"
        --package quiche
        --release
        --features ffi
        --locked
    WORKING_DIRECTORY "${QUICHE_SOURCE_DIR}"
    RESULT_VARIABLE cargo_result
    OUTPUT_VARIABLE cargo_output
    ERROR_VARIABLE cargo_error
)
if(NOT cargo_result EQUAL 0)
    message(FATAL_ERROR "second locked Cargo build failed: ${cargo_output}${cargo_error}")
endif()
file(SHA256 "${PROJECT_LOCK_FILE}" project_lock_after)
file(SHA256 "${FETCHED_LOCK_FILE}" fetched_lock_after)
if(NOT project_lock_before STREQUAL project_lock_after OR
   NOT fetched_lock_before STREQUAL fetched_lock_after OR
   NOT project_lock_after STREQUAL fetched_lock_after)
    message(FATAL_ERROR "second locked Cargo build changed or ignored a lockfile")
endif()

function(run_checked)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result
                    OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "fixture command failed: ${ARGN}\n${output}${error}")
    endif()
endfunction()

function(make_checkout destination)
    file(REMOVE_RECURSE "${destination}")
    run_checked("${GIT_EXECUTABLE}" clone --local --no-hardlinks
                "${QUICHE_SOURCE_DIR}" "${destination}")
    file(REMOVE_RECURSE "${destination}/quiche/deps/boringssl")
    run_checked("${GIT_EXECUTABLE}" clone --local --no-hardlinks
                "${QUICHE_SOURCE_DIR}/quiche/deps/boringssl"
                "${destination}/quiche/deps/boringssl")
endfunction()

function(expect_project_configure_failure name checkout)
    set(source_dir "${FIXTURE_ROOT}/${name}-source")
    set(build_dir "${FIXTURE_ROOT}/${name}-build")
    file(REMOVE_RECURSE "${source_dir}" "${build_dir}")
    file(MAKE_DIRECTORY "${source_dir}")
    file(WRITE "${source_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(quiche_pin_fixture LANGUAGES CXX)\n"
        "include(\"${REAL_SOURCE_DIR}/cmake/Dependencies.cmake\")\n"
        "find_package(Git REQUIRED)\n"
        "include(\"${REAL_SOURCE_DIR}/cmake/Quiche.cmake\")\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}"
            "-DFETCHCONTENT_SOURCE_DIR_QUICHE=${checkout}"
            "-DMOQ_INTEROP_CARGO_EXECUTABLE=${CARGO_EXECUTABLE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    if(result EQUAL 0)
        message(FATAL_ERROR "real project configure accepted ${name} mismatch")
    endif()
    string(CONCAT configure_log "${output}" "${error}")
    if(NOT configure_log MATCHES "pinned quiche checkout verification failed")
        message(FATAL_ERROR
            "${name} fixture failed outside the project pin gate: ${configure_log}")
    endif()
endfunction()

file(REMOVE_RECURSE "${FIXTURE_ROOT}")
file(MAKE_DIRECTORY "${FIXTURE_ROOT}")

set(main_mismatch "${FIXTURE_ROOT}/main-mismatch-quiche")
make_checkout("${main_mismatch}")
run_checked("${GIT_EXECUTABLE}" -C "${main_mismatch}" -c user.name=Interop
            -c user.email=interop.invalid commit --allow-empty -m main-mismatch)
expect_project_configure_failure(main "${main_mismatch}")

set(boringssl_mismatch "${FIXTURE_ROOT}/boringssl-mismatch-quiche")
make_checkout("${boringssl_mismatch}")
run_checked("${GIT_EXECUTABLE}" -C
            "${boringssl_mismatch}/quiche/deps/boringssl"
            -c user.name=Interop -c user.email=interop.invalid
            commit --allow-empty -m boringssl-mismatch)
expect_project_configure_failure(boringssl "${boringssl_mismatch}")
