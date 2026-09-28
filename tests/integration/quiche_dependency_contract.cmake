foreach(required IN ITEMS GIT_EXECUTABLE QUICHE_SOURCE_DIR PROJECT_LOCK_FILE
                          FETCHED_LOCK_FILE VERIFY_SCRIPT QUICHE_REVISION
                          BORINGSSL_REVISION)
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

file(SHA256 "${PROJECT_LOCK_FILE}" project_lock_sha256)
file(SHA256 "${FETCHED_LOCK_FILE}" fetched_lock_sha256)
if(NOT project_lock_sha256 STREQUAL fetched_lock_sha256)
    message(FATAL_ERROR "Cargo changed or ignored the project-owned lockfile")
endif()
