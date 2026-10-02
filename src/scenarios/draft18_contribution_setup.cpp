#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kOptionPath = 0x01;
constexpr std::uint64_t kOptionAuthorizationToken = 0x03;
constexpr std::uint64_t kOptionMaxTokenCache = 0x04;
constexpr std::uint64_t kOptionAuthority = 0x05;
constexpr std::uint64_t kOptionImplementation = 0x07;
constexpr std::uint64_t kRequestId = 1;
constexpr std::size_t kOversizeTokenBytes = 64;
// Section 10.3.1.3: a registered token costs 16 bytes plus its value.
constexpr std::uint64_t kOversizeTokenCost = 16 + kOversizeTokenBytes;
constexpr std::uint64_t kAlias = 1;

enum class Multiplicity { Valid, Invalid, Indeterminate };

// Section 10.3: a Setup Option Type must not repeat unless its definition
// permits it. Only AUTHORIZATION TOKEN (10.3.1.4) is defined as repeatable;
// an unknown type may define its own repetition rule.
Multiplicity multiplicity(const d18::KeyValuePairs& options) {
    bool unknown_duplicate = false;
    for (std::size_t index = 1; index < options.size(); ++index) {
        const auto type = options[index].type;
        if (type != options[index - 1].type || type == kOptionAuthorizationToken) continue;
        if (type == kOptionPath || type == kOptionMaxTokenCache ||
            type == kOptionAuthority || type == kOptionImplementation)
            return Multiplicity::Invalid;
        unknown_duplicate = true;
    }
    return unknown_duplicate ? Multiplicity::Indeterminate : Multiplicity::Valid;
}

bool has_option(const d18::KeyValuePairs& options, std::uint64_t type) {
    return std::any_of(options.begin(), options.end(),
                       [type](const auto& option) { return option.type == type; });
}

std::uint64_t advertised_token_cache(const d18::KeyValuePairs& options) {
    for (const auto& option : options)
        if (option.type == kOptionMaxTokenCache)
            if (const auto* value = std::get_if<d18::VarIntValue>(&option.value))
                return value->value;
    return 0;  // Section 10.3.1.3 default.
}

Bytes discovery_request(std::optional<d18::Parameters> parameters = std::nullopt) {
    return encode(d18::SubscribeNamespaceMessage{
        kRequestId, d18::TrackNamespace{{text("a")}}, parameters.value_or(d18::Parameters{})});
}

Bytes token_structure(const d18::Token& token) {
    wire::ByteWriter output(kMaximumFrame);
    if (!d18::encode_token(token, output).has_value())
        throw std::invalid_argument("unencodable authorization token");
    return {output.bytes().begin(), output.bytes().end()};
}

RawProbeDefinition observe_setup(const char* id, std::chrono::milliseconds deadline) {
    RawProbeDefinition definition{id, setup_message({}), {}, true, setup_ready, deadline,
        [](const RawProbeTranscript& transcript) { return transcript.peer_setup_received; }, {}};
    return definition;
}

// SETUP carrying `options`, then one valid discovery request. The request's
// typed reply shows the session survived the SETUP.
RawProbeDefinition setup_then_request(const char* id, d18::KeyValuePairs options,
                                      Bytes request, std::chrono::milliseconds deadline) {
    RawProbeDefinition definition{id, setup_message(std::move(options)),
            {{RawProbeChannel::NewBidi, std::move(request), false}}, true, setup_ready,
            deadline, first_response_or_close(0), {}};
    // A publisher that announces its namespace first waits for the acknowledgement
    // before it reads other requests (section 10.15); the answer is not the stimulus.
    definition.acknowledge_publisher_namespace = true;
    return definition;
}

// A typed REQUEST_OK or REQUEST_ERROR proves the publisher kept processing
// after SETUP; closing the session instead is a definite failure.
std::optional<bool> setup_continues(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (!reply.messages.empty())
        return is_typed_response(reply.messages.front()) ? std::optional<bool>{true}
                                                         : std::nullopt;
    if (application_close(transcript)) return false;
    return std::nullopt;
}

std::optional<bool> token_cache_not_fatal(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto control = peer_control(transcript);
    // Only a token that exceeds the publisher's advertised limit exercises
    // the rule; with a large advertised cache nothing is established.
    if (!control || advertised_token_cache(control->setup_options) >= kOversizeTokenCost)
        return std::nullopt;
    const auto close = peer_close(transcript);
    if (close && close->space == transport::CloseErrorSpace::Application &&
        close->code == kCloseAuthTokenCacheOverflow)
        return false;
    const auto reply = write_reply(transcript, 0);
    if (!reply.messages.empty() && is_typed_response(reply.messages.front())) return true;
    return std::nullopt;
}

// Section 10.3.1.4 treats the oversize REGISTER as USE_VALUE, so its alias is
// not registered. Section 10.2.2 requires rejecting a USE_ALIAS for it.
std::optional<bool> setup_token_alias_not_registered(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto control = peer_control(transcript);
    if (!control || advertised_token_cache(control->setup_options) >= kOversizeTokenCost)
        return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (reply.messages.empty()) return application_close(transcript) ? std::optional<bool>{false}
                                                                     : std::nullopt;
    const auto* error = request_error(reply);
    if (!error) return false;  // an alias that was never registered cannot succeed
    // Draft 18 names no REQUEST_ERROR code for UNKNOWN_AUTH_TOKEN_ALIAS, so the
    // code is compared only against an explicitly configured compatibility value.
    const auto expected = transcript.unknown_auth_token_alias_compatibility_code;
    if (!expected) return std::nullopt;
    return error->error_code == *expected;
}

d18::KeyValuePairs unknown_options(bool duplicate, bool grease_pair) {
    d18::KeyValuePairs options;
    options.push_back(odd_option(kGreaseOdd, text("interop-unknown")));
    if (duplicate) options.push_back(odd_option(kGreaseOdd, text("interop-duplicate")));
    if (grease_pair) {
        options.push_back(even_option(kGreaseEven, 7));
        options.push_back(even_option(kGreaseEven, 9));
    }
    return options;
}

d18::KeyValuePairs oversize_register_option() {
    d18::Token token{d18::TokenAliasType::Register, kAlias, 0, Bytes(kOversizeTokenBytes, std::byte{'k'})};
    return {odd_option(kOptionAuthorizationToken, token_structure(token))};
}

}  // namespace

std::vector<Draft18ContributionProbe> setup_probes(std::chrono::milliseconds deadline) {
    std::vector<Draft18ContributionProbe> result;
    const auto setup_observation = [](bool authority) -> Observe {
        return [authority](const RawProbeTranscript& transcript, bool webtransport)
                   -> std::optional<bool> {
            if (!webtransport || !bounded(transcript)) return std::nullopt;
            const auto control = peer_control(transcript);
            if (!control) return std::nullopt;
            return !has_option(control->setup_options, authority ? kOptionAuthority : kOptionPath);
        };
    };
    result.push_back(make_probe("D18-10-3-MUST-NOT-001",
        "setup-option-types-unique-except-defined-repeatable-options",
        observe_setup("observe-publisher-setup-options", deadline),
        [](const RawProbeTranscript& transcript, bool) -> std::optional<bool> {
            if (!bounded(transcript)) return std::nullopt;
            const auto control = peer_control(transcript);
            if (!control) return std::nullopt;
            switch (multiplicity(control->setup_options)) {
                case Multiplicity::Invalid: return false;
                case Multiplicity::Valid: return true;
                case Multiplicity::Indeterminate: return std::nullopt;
            }
            return std::nullopt;
        }));
    result.push_back(make_probe("D18-10-3-1-1-MUST-NOT-002", "setup-omits-authority",
        observe_setup("observe-webtransport-publisher-setup", deadline), setup_observation(true)));
    result.push_back(make_probe("D18-10-3-1-2-MUST-NOT-002", "setup-omits-path",
        observe_setup("observe-webtransport-publisher-setup", deadline), setup_observation(false)));

    const char* continues = "setup-continues-with-unknown-options-ignored";
    result.push_back(make_probe("D18-10-3-MUST-003", continues,
        setup_then_request("receive-setup-with-duplicate-unknown-options", unknown_options(true, false),
                           discovery_request(), deadline), setup_continues));
    for (const auto& [requirement, evaluator] : {
             std::pair{"D18-14-MUST-001", continues}, std::pair{"D18-14-MUST-008", continues},
             std::pair{"D18-15-4-MUST-001", continues},
             std::pair{"D18-14-MUST-NOT-001", "unknown-extensible-value-alone-does-not-close-session"}})
        result.push_back(make_probe(requirement, evaluator,
            setup_then_request("setup-unknown-grease-options-and-duplicates",
                               unknown_options(true, true), discovery_request(), deadline),
            setup_continues));

    result.push_back(make_probe("D18-10-3-1-4-MUST-NOT-001",
        "no-auth-token-cache-overflow-session-error",
        setup_then_request("receive-setup-token-register-exceeding-cache-limit",
                           oversize_register_option(), discovery_request(), deadline),
        token_cache_not_fatal));
    d18::Parameters use_alias{{kOptionAuthorizationToken,
        d18::Token{d18::TokenAliasType::UseAlias, kAlias, std::nullopt, {}}}};
    result.push_back(make_probe("D18-10-3-1-4-MUST-001",
        "setup-token-processed-as-value-without-alias-registration",
        setup_then_request("receive-oversize-setup-register-then-use-its-alias",
                           oversize_register_option(), discovery_request(use_alias), deadline),
        setup_token_alias_not_registered));
    return result;
}

}  // namespace moq::interop::scenarios::contribution
