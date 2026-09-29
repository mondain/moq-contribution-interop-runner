foreach(required GIT_EXECUTABLE PICOQUIC_SOURCE_DIR PICOTLS_SOURCE_DIR
                 EXPECTED_PICOQUIC_REVISION EXPECTED_PICOTLS_REVISION
                 WEBTRANSPORT_PATCH)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "missing ${required}")
    endif()
endforeach()

function(require_revision source expected name)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${source}"
        OUTPUT_VARIABLE actual
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${name} revision mismatch: expected ${expected}, actual ${actual}")
    endif()
endfunction()

require_revision("${PICOQUIC_SOURCE_DIR}" "${EXPECTED_PICOQUIC_REVISION}" picoquic)
require_revision("${PICOTLS_SOURCE_DIR}" "${EXPECTED_PICOTLS_REVISION}" picotls)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero --reverse --check
            "${WEBTRANSPORT_PATCH}"
    WORKING_DIRECTORY "${PICOQUIC_SOURCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
