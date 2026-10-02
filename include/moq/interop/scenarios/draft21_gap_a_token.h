#pragma once

// Draft-21 completeness-gap slice A: Authorization Token Alias handling by the
// publisher (Section 8.9 and 9.1.4). Every probe sends a sequence of
// TRACK_STATUS requests, each carrying one AUTHORIZATION TOKEN parameter, and
// compares the publisher's responses.
//
// Oracle. The draft assigns no REQUEST_ERROR code to UNKNOWN_AUTH_TOKEN_ALIAS
// (0x17 is only a session termination code), so alias state is inferred from
// differential responses: a control request names an Alias that was never
// registered, and a registered or retired Alias is compared with it. The
// optional compatibility mapping (--unknown-auth-token-alias-compat-code) turns
// the unassigned code into an exact check.
//
// Prerequisite. Aliases need MAX_AUTH_TOKEN_CACHE_SIZE (Section 9.1.3); the
// publisher's SETUP must advertise enough room or no context starts.

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

enum class Draft21TokenAspect {
    // Section 8.9: DELETE retires the Alias and its Token Value.
    DeleteRetiresAlias,
    // Section 8.9: a registered Alias stays associated with its Token Value.
    RegisterAssociates,
    // Section 8.9: a deleted Alias is unknown and its message is rejected.
    DeletedAliasRejected,
    // Section 8.9: the Alias is registered even if the message fails for
    // authorization (UNAUTHORIZED) or for another reason.
    RegisterRetainedAfterUnauthorized,
    RegisterRetainedAfterOtherError,
    // Section 9.1.4: an oversized SETUP REGISTER is treated as USE_VALUE.
    SetupRegisterFallsBack,
};

struct Draft21TokenProbe {
    std::string requirement_id;
    std::string evaluator_id;
    Draft21TokenAspect aspect;
    RawProbeDefinition definition;
};

std::vector<Draft21TokenProbe> draft21_gap_a_token_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});

std::optional<bool> evaluate_draft21_gap_a_token_probe(
    const RawProbeTranscript& transcript, const Draft21TokenProbe& probe);

}  // namespace moq::interop::scenarios
