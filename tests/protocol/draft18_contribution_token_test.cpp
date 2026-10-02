// Draft 18 section 10.2.2 rows that need an operator-supplied credential:
// D18-10-2-2-MUST-008 (well-formed but invalid token -> MALFORMED_AUTH_TOKEN) and
// D18-10-2-2-MUST-010 (expired token's Alias is retained; USE_ALIAS -> EXPIRED_AUTH_TOKEN).
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

constexpr const char* kInvalid = "receive-well-formed-token-with-invalid-known-type-value";
constexpr const char* kExpired = "register-token-expire-then-use-alias-before-delete";
// Section 15.10.2: REQUEST_ERROR codes.
constexpr std::uint64_t kUnauthorized = 0x1;
constexpr std::uint64_t kNotSupported = 0x3;
constexpr std::uint64_t kMalformedAuthToken = 0x4;
constexpr std::uint64_t kExpiredAuthToken = 0x5;
constexpr std::uint64_t kDoesNotExist = 0x10;
constexpr std::uint64_t kDuplicateAuthTokenAlias = 0x14;  // section 15.10.1 (session close)

Draft18TokenCredentials credentials() {
    return {Draft18TokenCredential{4, text("bad")}, Draft18TokenCredential{4, text("old")}};
}
std::vector<Draft18ContributionProbe> configured() {
    return draft18_contribution_probes(milliseconds{80}, {text("n")}, text("t"), credentials());
}
std::vector<Draft18ContributionProbe> unconfigured() {
    return draft18_contribution_probes(milliseconds{80}, {text("n")}, text("t"));
}

struct Decoded {
    std::uint64_t request_id{0};
    std::vector<Bytes> prefix;
    d18::Token token;
};
Decoded decode_token_request(const Bytes& bytes) {
    Decoded result;
    const auto message = decode_request(bytes);
    EXPECT_TRUE(message);
    if (!message) return result;
    const auto* request = std::get_if<d18::SubscribeNamespaceMessage>(&*message);
    EXPECT_NE(request, nullptr);
    if (!request) return result;
    result.request_id = request->request_id;
    result.prefix = request->track_namespace_prefix.fields;
    EXPECT_EQ(request->parameters.size(), 1u);
    if (request->parameters.size() == 1) {
        EXPECT_EQ(request->parameters.front().type, 0x03u);
        result.token = std::get<d18::Token>(request->parameters.front().value);
    }
    return result;
}

TEST(Draft18ContributionToken, ScenariosAreRegisteredAndBoundToTheCatalogNames) {
    for (const auto& all : {configured(), unconfigured()}) {
        const auto& invalid = probe(all, kInvalid, "D18-10-2-2-MUST-008");
        const auto& expired = probe(all, kExpired, "D18-10-2-2-MUST-010");
        EXPECT_EQ(invalid.evaluator_id, "request-error-malformed-auth-token");
        EXPECT_EQ(expired.evaluator_id, "expired-alias-rejected-as-expired-not-unknown");
    }
    for (const auto* id : {kInvalid, kExpired}) {
        EXPECT_TRUE(draft18_contribution_scenario(id)) << id;
        EXPECT_TRUE(draft18_contribution_requires_track(id)) << id;
        EXPECT_TRUE(app::scenario_requires_track(18, id)) << id;
        const auto executable = app::executable_scenarios(18);
        EXPECT_NE(std::find(executable.begin(), executable.end(), id), executable.end()) << id;
    }
}

TEST(Draft18ContributionToken, WithoutACredentialNothingIsSentAndNothingIsScored) {
    const auto all = unconfigured();
    for (const auto& [scenario, requirement] : {std::pair{kInvalid, "D18-10-2-2-MUST-008"},
                                                std::pair{kExpired, "D18-10-2-2-MUST-010"}}) {
        const auto& p = probe(all, scenario, requirement);
        EXPECT_TRUE(p.definition.writes.empty()) << scenario;
        const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
        });
        EXPECT_TRUE(transcript.complete) << scenario;
        EXPECT_TRUE(transcript.writes.empty()) << scenario;
        EXPECT_EQ(evaluate_draft18_contribution_probe(transcript, p), std::nullopt) << scenario;
    }
}

TEST(Draft18ContributionToken, OnlyTheMatchingCredentialIsConfigured) {
    Draft18TokenCredentials only_expired{std::nullopt, Draft18TokenCredential{4, text("old")}};
    const auto all = draft18_contribution_probes(milliseconds{80}, {text("n")}, text("t"), only_expired);
    EXPECT_TRUE(probe(all, kInvalid, "D18-10-2-2-MUST-008").definition.writes.empty());
    EXPECT_EQ(probe(all, kExpired, "D18-10-2-2-MUST-010").definition.writes.size(), 3u);
}

// Section 10.2.2: USE_VALUE (Alias Type 0x3) carries Token Type and Value without an Alias.
TEST(Draft18ContributionToken, InvalidCredentialIsSentAsUseValueOnADiscoveryRequest) {
    const auto all = configured();
    const auto& p = probe(all, kInvalid, "D18-10-2-2-MUST-008");
    ASSERT_EQ(p.definition.writes.size(), 1u);
    const auto request = decode_token_request(p.definition.writes.front().bytes);
    EXPECT_EQ(request.request_id, 1u);
    EXPECT_EQ(request.prefix, (std::vector<Bytes>{text("n")}));
    EXPECT_EQ(request.token.alias_type, d18::TokenAliasType::UseValue);
    EXPECT_EQ(request.token.alias, std::nullopt);
    EXPECT_EQ(request.token.token_type, std::optional<std::uint64_t>{4});
    EXPECT_EQ(request.token.token_value, text("bad"));
}

std::optional<bool> invalid_outcome(const std::optional<Bytes>& response, bool close = false) {
    const auto all = configured();
    const auto& p = probe(all, kInvalid, "D18-10-2-2-MUST-008");
    const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("reply", v.sent(1) && response, [&] { v.data(1, *response, true); });
        v.when("close", v.sent(1) && close, [&] {
            v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x16, {}});
        });
    });
    // The evaluators hold credential-free profiles; the credential is recovered from the transcript.
    const auto bare = unconfigured();
    const auto result = evaluate_draft18_contribution_probe(transcript, p);
    EXPECT_EQ(evaluate_draft18_contribution_probe(transcript, probe(bare, kInvalid, "D18-10-2-2-MUST-008")), result);
    return result;
}

// Pass: MALFORMED_AUTH_TOKEN. Fail: acceptance or any other rejection.
TEST(Draft18ContributionToken, WellFormedInvalidTokenMustBeRejectedWithMalformedAuthToken) {
    EXPECT_EQ(invalid_outcome(error(kMalformedAuthToken)), std::optional<bool>{true});
    EXPECT_EQ(invalid_outcome(ok()), std::optional<bool>{false});
    EXPECT_EQ(invalid_outcome(error(kUnauthorized)), std::optional<bool>{false});
    EXPECT_EQ(invalid_outcome(error(kDoesNotExist)), std::optional<bool>{false});
    EXPECT_EQ(invalid_outcome(error(kExpiredAuthToken)), std::optional<bool>{false});
}

// Unscored: Token Type not understood, a different message, silence, a session close.
TEST(Draft18ContributionToken, InvalidTokenIsUnscoredWithoutConclusiveWireEvidence) {
    EXPECT_EQ(invalid_outcome(error(kNotSupported)), std::nullopt);
    EXPECT_EQ(invalid_outcome(subscribe_ok()), std::nullopt);
    EXPECT_EQ(invalid_outcome(std::nullopt), std::nullopt);
    // KEY_VALUE_FORMATTING_ERROR-style closure is the structural error, not the message-level rejection.
    EXPECT_EQ(invalid_outcome(std::nullopt, true), std::nullopt);
}

// Section 10.2.2: REGISTER (0x1) Alias, Type, Value; USE_ALIAS (0x2) Alias only; the Alias
// cannot be registered again without a DELETE.
TEST(Draft18ContributionToken, ExpiredCredentialIsRegisteredUsedByAliasAndRegisteredAgain) {
    const auto all = configured();
    const auto& p = probe(all, kExpired, "D18-10-2-2-MUST-010");
    ASSERT_EQ(p.definition.writes.size(), 3u);
    const auto registration = decode_token_request(p.definition.writes[0].bytes);
    EXPECT_EQ(registration.request_id, 1u);
    EXPECT_EQ(registration.prefix, (std::vector<Bytes>{text("n")}));
    EXPECT_EQ(registration.token.alias_type, d18::TokenAliasType::Register);
    EXPECT_EQ(registration.token.alias, std::optional<std::uint64_t>{1});
    EXPECT_EQ(registration.token.token_type, std::optional<std::uint64_t>{4});
    EXPECT_EQ(registration.token.token_value, text("old"));
    const auto use = decode_token_request(p.definition.writes[1].bytes);
    EXPECT_EQ(use.request_id, 3u);
    EXPECT_EQ(use.token.alias_type, d18::TokenAliasType::UseAlias);
    EXPECT_EQ(use.token.alias, std::optional<std::uint64_t>{1});
    EXPECT_EQ(use.token.token_type, std::nullopt);
    EXPECT_TRUE(use.token.token_value.empty());
    const auto again = decode_token_request(p.definition.writes[2].bytes);
    EXPECT_EQ(again.request_id, 5u);
    EXPECT_EQ(again.token.alias_type, d18::TokenAliasType::Register);
    EXPECT_EQ(again.token.alias, std::optional<std::uint64_t>{1});
    EXPECT_EQ(again.token.token_value, text("old"));
    // Each write waits for the previous request to be answered.
    EXPECT_TRUE(static_cast<bool>(p.definition.writes[1].evidence_ready));
    EXPECT_TRUE(static_cast<bool>(p.definition.writes[2].evidence_ready));
}

struct ExpiredScript {
    std::optional<Bytes> registration = error(kExpiredAuthToken);
    std::optional<Bytes> use = error(kExpiredAuthToken);
    std::optional<Bytes> second;
    std::optional<std::uint64_t> close_code = kDuplicateAuthTokenAlias;
    std::optional<std::uint64_t> unknown_alias_code;
};

std::optional<bool> expired_outcome(const ExpiredScript& script) {
    const auto all = configured();
    const auto& p = probe(all, kExpired, "D18-10-2-2-MUST-010");
    auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("r0", v.sent(1) && script.registration, [&] { v.data(1, *script.registration, true); });
        v.when("r1", v.sent(5) && script.use, [&] { v.data(5, *script.use, true); });
        v.when("r2", v.sent(9) && script.second, [&] { v.data(9, *script.second, true); });
        v.when("close", v.sent(9) && script.close_code, [&] {
            v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, *script.close_code, {}});
        });
    });
    transcript.unknown_auth_token_alias_compatibility_code = script.unknown_alias_code;
    const auto bare = unconfigured();
    const auto result = evaluate_draft18_contribution_probe(transcript, p);
    EXPECT_EQ(evaluate_draft18_contribution_probe(transcript, probe(bare, kExpired, "D18-10-2-2-MUST-010")), result);
    return result;
}

// Pass: the registration is EXPIRED_AUTH_TOKEN, the Alias then fails with EXPIRED_AUTH_TOKEN and
// registering it again is a DUPLICATE_AUTH_TOKEN_ALIAS session error (the Alias was retained).
TEST(Draft18ContributionToken, RetainedExpiredAliasPasses) {
    EXPECT_EQ(expired_outcome({}), std::optional<bool>{true});
}

TEST(Draft18ContributionToken, ForgottenExpiredAliasFails) {
    // The Alias is no longer known: any other error code, or the configured UNKNOWN_AUTH_TOKEN_ALIAS code.
    ExpiredScript other;
    other.use = error(kUnauthorized);
    other.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(other), std::optional<bool>{false});
    ExpiredScript unknown;
    unknown.use = error(0x17);
    unknown.unknown_alias_code = 0x17;
    unknown.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(unknown), std::optional<bool>{false});
    // The USE_ALIAS was accepted although the credential is expired.
    ExpiredScript accepted;
    accepted.use = ok();
    accepted.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(accepted), std::optional<bool>{false});
    // Registering the Alias again was answered instead of closing the Session: it had been dropped.
    ExpiredScript reregistered;
    reregistered.second = error(kExpiredAuthToken);
    reregistered.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(reregistered), std::optional<bool>{false});
}

TEST(Draft18ContributionToken, ExpiredAliasIsUnscoredWhenThePreconditionIsNotShown) {
    // The registration must itself fail as expired: success, NOT_SUPPORTED (registration unsupported
    // or the Token Type unknown) and any other rejection leave the credential unproven.
    for (const auto& registration : {ok(), error(kNotSupported), error(kUnauthorized), error(kMalformedAuthToken)}) {
        ExpiredScript script;
        script.registration = registration;
        EXPECT_EQ(expired_outcome(script), std::nullopt);
    }
    // No conclusion from silence: no duplicate-registration close and no reply to it.
    ExpiredScript quiet;
    quiet.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(quiet), std::nullopt);
    // A different session close is not DUPLICATE_AUTH_TOKEN_ALIAS and not evidence either way.
    ExpiredScript other_close;
    other_close.close_code = 0x3;
    EXPECT_EQ(expired_outcome(other_close), std::nullopt);
    // Nothing answered at all.
    ExpiredScript silent;
    silent.registration = std::nullopt;
    silent.use = std::nullopt;
    silent.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(silent), std::nullopt);
    // The USE_ALIAS answer is not a REQUEST_ERROR.
    ExpiredScript odd;
    odd.use = subscribe_ok();
    odd.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(odd), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
