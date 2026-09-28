foreach(required IN ITEMS GIT_EXECUTABLE QUICHE_SOURCE_DIR EXPECTED_QUICHE_REVISION
                          EXPECTED_BORINGSSL_REVISION)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${QUICHE_SOURCE_DIR}"
    OUTPUT_VARIABLE actual_quiche_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT actual_quiche_revision STREQUAL EXPECTED_QUICHE_REVISION)
    message(FATAL_ERROR
        "quiche revision mismatch: expected ${EXPECTED_QUICHE_REVISION}, actual ${actual_quiche_revision}")
endif()

set(boringssl_source_dir "${QUICHE_SOURCE_DIR}/quiche/deps/boringssl")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${boringssl_source_dir}"
    OUTPUT_VARIABLE actual_boringssl_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT actual_boringssl_revision STREQUAL EXPECTED_BORINGSSL_REVISION)
    message(FATAL_ERROR
        "BoringSSL revision mismatch: expected ${EXPECTED_BORINGSSL_REVISION}, actual ${actual_boringssl_revision}")
endif()
