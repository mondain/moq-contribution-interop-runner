// Draft-18 section 10.2.2 rows that depend on a Token Type: a well-formed but
// otherwise invalid AUTHORIZATION TOKEN (D18-10-2-2-MUST-008) and the Alias of an
// expired token (D18-10-2-2-MUST-010). Draft 18 defines no Token Type (Table 12;
// type 0 is negotiated out of band), so the runner cannot mint either credential.
// The operator supplies one for a Token Type the publisher understands; without it
// a scenario sends nothing and the row stays unscored.
#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

// Section 10.2.2: AUTHORIZATION TOKEN is Parameter Type 0x03.
constexpr std::uint64_t kParameterAuthorizationToken = 0x03;
// Section 15.10.2: REQUEST_ERROR codes.
constexpr std::uint64_t kErrorNotSupported = 0x3;
constexpr std::uint64_t kErrorMalformedAuthToken = 0x4;
constexpr std::uint64_t kErrorExpiredAuthToken = 0x5;
// Section 15.10.1: session termination code.
constexpr std::uint64_t kCloseDuplicateAuthTokenAlias = 0x14;
constexpr std::uint64_t kAlias = 1;

// A discovery request carrying one AUTHORIZATION TOKEN. The runner is the MOQT
// server, so its Request IDs are odd.
Bytes token_request(std::uint64_t id, const Fixture& fixture, d18::Token token) {
    const auto prefix = d18::TrackNamespace{
        fixture.track_namespace.empty() ? Namespace{text("a")} : fixture.track_namespace};
    return encode(d18::SubscribeNamespaceMessage{
        id, prefix, d18::Parameters{{kParameterAuthorizationToken, std::move(token)}}});
}

d18::Token use_value(const Draft18TokenCredential& credential) {
    return {d18::TokenAliasType::UseValue, std::nullopt, credential.token_type, credential.value};
}
d18::Token register_alias(const Draft18TokenCredential& credential) {
    return {d18::TokenAliasType::Register, kAlias, credential.token_type, credential.value};
}
d18::Token use_alias() {
    return {d18::TokenAliasType::UseAlias, kAlias, std::nullopt, {}};
}

bool answered(const Reply& reply) { return !reply.messages.empty() || reply.fin || reply.reset; }

// The first message of the reply to write `index`, as an error code when it is one.
enum class Answer { None, Ok, Error, Other };
struct Outcome {
    Answer kind{Answer::None};
    std::uint64_t code{0};
};
Outcome outcome_of(const Reply& reply) {
    if (reply.messages.empty()) return {};
    if (const auto* error = request_error(reply)) return {Answer::Error, error->error_code};
    if (std::holds_alternative<d18::RequestOkMessage>(reply.messages.front())) return {Answer::Ok, 0};
    return {Answer::Other, 0};
}

// Section 10.2.2: "The receiver of a message containing a well-formed Token structure
// but otherwise invalid AUTHORIZATION TOKEN parameter MUST reject that message with an
// MALFORMED_AUTH_TOKEN error."
std::optional<bool> invalid_token_rejected_as_malformed(const RawProbeTranscript& transcript, bool) {
    if (transcript.writes.empty() || !bounded(transcript)) return std::nullopt;  // no credential configured
    const auto outcome = outcome_of(write_reply(transcript, 0));
    switch (outcome.kind) {
        case Answer::Ok: return false;
        case Answer::Error:
            // NOT_SUPPORTED says the Token Type is not understood: the precondition is unmet.
            if (outcome.code == kErrorNotSupported) return std::nullopt;
            return outcome.code == kErrorMalformedAuthToken;
        // Silence, a reset or a close (the structural KEY_VALUE_FORMATTING_ERROR is a different rule).
        case Answer::None:
        case Answer::Other: return std::nullopt;
    }
    return std::nullopt;
}

// Section 10.2.2: a REGISTER that does not end the Session registers its Alias even if the
// message fails. "If a receiver detects that an authorization token has expired, it MUST
// retain the registered Alias until it is deleted by the sender ... Any message that
// references the token with Alias Type USE_ALIAS fails with EXPIRED_AUTH_TOKEN." Registering
// a registered Alias again closes the Session with DUPLICATE_AUTH_TOKEN_ALIAS, which shows
// whether the Alias is still registered.
std::optional<bool> expired_alias_retained(const RawProbeTranscript& transcript, bool) {
    if (transcript.writes.empty() || !bounded(transcript)) return std::nullopt;  // no credential configured
    // The credential must really be expired: only EXPIRED_AUTH_TOKEN for the registration shows
    // that. Success or any other error (UNAUTHORIZED, NOT_SUPPORTED, ...) leaves the
    // precondition unmet.
    const auto registration = outcome_of(write_reply(transcript, 0));
    if (registration.kind != Answer::Error || registration.code != kErrorExpiredAuthToken) return std::nullopt;
    const auto use = outcome_of(write_reply(transcript, 1));
    switch (use.kind) {
        case Answer::None:
        case Answer::Other: return std::nullopt;
        // Accepted although the credential is expired.
        case Answer::Ok: return false;
        case Answer::Error: break;
    }
    // Draft 18 assigns no REQUEST_ERROR code to UNKNOWN_AUTH_TOKEN_ALIAS; an operator-configured
    // compatibility code names it. Either way an Alias that fails as anything but expired was
    // not retained.
    if (use.code != kErrorExpiredAuthToken) return false;
    const auto close = peer_close(transcript);
    if (close && close->space == transport::CloseErrorSpace::Application &&
        close->code == kCloseDuplicateAuthTokenAlias) return true;
    // The second registration was answered rather than refused as a duplicate: the Alias was forgotten.
    if (answered(write_reply(transcript, 2)) && !application_close(transcript)) return false;
    return std::nullopt;
}

std::optional<Draft18TokenCredential> credential_of(const d18::Token& token) {
    if (!token.token_type) return std::nullopt;
    return Draft18TokenCredential{*token.token_type, token.token_value};
}

RawProbeWrite after_answer(Bytes bytes, std::size_t previous) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), false};
    write.evidence_ready = [previous](const RawProbeGateInput& input) {
        return answered(gate_reply(input, previous));
    };
    return write;
}

}  // namespace

Draft18TokenCredentials token_credentials(const RawProbeTranscript& transcript) {
    Draft18TokenCredentials result;
    for (const auto& write : transcript.writes) {
        if (write.write.bytes.size() > kMaximumFrame) continue;
        const auto message = [&]() -> std::optional<d18::Message> {
            wire::Cursor cursor(write.write.bytes);
            auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
            if (auto* value = std::get_if<d18::Message>(&decoded)) return std::move(*value);
            return std::nullopt;
        }();
        const auto* request = message ? std::get_if<d18::SubscribeNamespaceMessage>(&*message) : nullptr;
        if (!request) continue;
        for (const auto& parameter : request->parameters) {
            if (parameter.type != kParameterAuthorizationToken) continue;
            const auto* token = std::get_if<d18::Token>(&parameter.value);
            const auto credential = token ? credential_of(*token) : std::nullopt;
            if (!credential) continue;
            // A USE_VALUE token carries the invalid credential, a REGISTER token the expired one.
            if (token->alias_type == d18::TokenAliasType::UseValue && !result.invalid) result.invalid = credential;
            else if (token->alias_type == d18::TokenAliasType::Register && !result.expired) result.expired = credential;
        }
    }
    return result;
}

std::vector<Draft18ContributionProbe> token_probes(std::chrono::milliseconds deadline, const Fixture& fixture,
                                                   const Draft18TokenCredentials& credentials) {
    std::vector<Draft18ContributionProbe> result;
    {
        RawProbeDefinition definition{"receive-well-formed-token-with-invalid-known-type-value", setup_message({}),
            {}, true, setup_ready, deadline, {}, {}};
        if (credentials.invalid)
            definition.writes.push_back({RawProbeChannel::NewBidi,
                token_request(1, fixture, use_value(*credentials.invalid)), false});
        // A publisher that cannot serve the request may give up; that is what it did.
        definition.publisher_exit_is_evidence = true;
        const bool sends = !definition.writes.empty();
        definition.response_ready = [sends](const RawProbeTranscript& transcript) {
            return !sends || answered(write_reply(transcript, 0)) || peer_close(transcript).has_value();
        };
        result.push_back(make_probe("D18-10-2-2-MUST-008", "request-error-malformed-auth-token",
            std::move(definition), invalid_token_rejected_as_malformed, true));
    }
    {
        RawProbeDefinition definition{"register-token-expire-then-use-alias-before-delete", setup_message({}),
            {}, true, setup_ready, deadline, {}, {}};
        if (credentials.expired) {
            definition.writes.push_back({RawProbeChannel::NewBidi,
                token_request(1, fixture, register_alias(*credentials.expired)), false});
            definition.writes.push_back(after_answer(token_request(3, fixture, use_alias()), 0));
            definition.writes.push_back(after_answer(
                token_request(5, fixture, register_alias(*credentials.expired)), 1));
        }
        definition.publisher_exit_is_evidence = true;
        const bool sends = !definition.writes.empty();
        definition.response_ready = [sends](const RawProbeTranscript& transcript) {
            if (!sends) return true;
            if (peer_close(transcript)) return true;
            const auto registration = outcome_of(write_reply(transcript, 0));
            if (registration.kind != Answer::None &&
                (registration.kind != Answer::Error || registration.code != kErrorExpiredAuthToken)) return true;
            const auto use = outcome_of(write_reply(transcript, 1));
            if (use.kind != Answer::None && (use.kind != Answer::Error || use.code != kErrorExpiredAuthToken))
                return true;
            return answered(write_reply(transcript, 2));
        };
        result.push_back(make_probe("D18-10-2-2-MUST-010", "expired-alias-rejected-as-expired-not-unknown",
            std::move(definition), expired_alias_retained, true));
    }
    return result;
}

}  // namespace moq::interop::scenarios::contribution
