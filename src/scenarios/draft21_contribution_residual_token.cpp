// Draft-21 contribution rows for authorization tokens and Subgroup timers: the
// publisher's use of Token Aliases, operator-configured credentials, and the reset
// of a Subgroup that cannot be committed. They share the end of the residual slice's
// row order, which is why the Subgroup timer row sits here. See
// draft21_contribution_residual.cpp for the fixture contract these rows share.

#include "draft21_contribution_support.h"
#include "draft21_contribution_residual_internal.h"

#include "moq/interop/wire/draft21/token.h"

namespace moq::interop::scenarios::d21c {
namespace {

using namespace shared;
using namespace residual;

constexpr std::uint64_t kPublishNamespace = 0x6;

// ---- Section 8.9 lines 3336-3337: DELETE only after every USE_ALIAS was answered ----------
// "Senders MUST NOT send DELETE for an alias while any message using USE_ALIAS with that
// alias has not received a response." The runner holds back its answer to each message
// that uses an Alias, then answers it. A DELETE that reaches the runner before the
// answer to an earlier use was even written cannot have waited for it.
enum class TokenAction { Delete, Register, UseAlias };

struct AliasMessage {
    TokenAction action{TokenAction::UseAlias};
    std::uint64_t alias{0};
    transport::StreamId stream{0};
    std::size_t frame_index{0};
    std::size_t event{0};
};

// AUTHORIZATION TOKEN parameters (0x03) of a publisher request message.
std::vector<wire::draft21::Token> tokens_of(std::uint64_t type, const Bytes& body_bytes) {
    std::vector<wire::draft21::Token> result;
    wire::Cursor body(body_bytes);
    if (!read_vi(body)) return result;
    if (type == kPublishNamespace || type == kPublish) {
        auto name_space = read_namespace(body);
        if (!name_space) return result;
        if (type == kPublish) {
            const auto length = read_vi(body);
            if (!length || !read_n(body, static_cast<std::size_t>(*length)) || !read_vi(body)) return result;
        }
    } else if (type != kRequestUpdate) {
        return result;
    }
    const auto count = read_vi(body);
    if (!count) return result;
    std::uint64_t parameter = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto delta = read_vi(body);
        if (!delta) return result;
        parameter += *delta;
        if ((parameter & 1u) == 0u) {
            if (!read_vi(body)) return result;
            continue;
        }
        const auto length = read_vi(body);
        const auto value = length ? read_n(body, static_cast<std::size_t>(*length)) : std::nullopt;
        if (!value) return result;
        if (parameter != 0x03) continue;
        const auto token = wire::draft21::decode_token(*value);
        if (const auto* decoded = std::get_if<wire::draft21::Token>(&token)) result.push_back(*decoded);
    }
    return result;
}

std::vector<AliasMessage> alias_messages(const View& view) {
    std::vector<AliasMessage> result;
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 0u) continue;
        const auto frames = view.frames(record);
        for (std::size_t index = 0; index < frames.size(); ++index) {
            for (const auto& token : tokens_of(frames[index].type, frames[index].body)) {
                if (!token.alias) continue;
                AliasMessage message{TokenAction::UseAlias, *token.alias, id, index, frames[index].event};
                if (token.alias_type == wire::draft21::TokenAliasType::Delete) message.action = TokenAction::Delete;
                else if (token.alias_type == wire::draft21::TokenAliasType::Register) message.action = TokenAction::Register;
                else if (token.alias_type != wire::draft21::TokenAliasType::UseAlias) continue;
                result.push_back(message);
            }
        }
    }
    return result;
}

// The event at which the runner wrote its answer to frame `index` of `stream`, if it did.
// A stream opened by PUBLISH_NAMESPACE has its first frame answered by the base
// definition's acknowledgement, which is recorded apart from the courtesy responses.
std::optional<std::size_t> answer_event(const View& view, transport::StreamId stream, std::size_t index) {
    const auto* record = view.stream(stream);
    if (record) {
        const auto frames = view.frames(*record);
        if (!frames.empty() && frames.front().type == kPublishNamespace) {
            if (index == 0) return view.auto_reply_event(stream);
            --index;
        }
    }
    std::size_t seen = 0;
    for (const auto& write : view.courtesy_writes()) {
        if (write.stream_id != stream) continue;
        if (seen++ == index) return write.event_count;
    }
    return std::nullopt;
}

Spec pending_alias_delete_spec() {
    return spec("d21-publisher-delete-with-pending-alias-uses",
        {{"D21-8-9-MUST-NOT-281", "d21-no-delete-with-unanswered-alias-use"}},
        [](const Fixture&) {
            auto courtesy = accepting_publishes();
            courtesy.update = RawProbeUpdateResponse::HoldAliasUses;
            auto definition = observing(courtesy);
            // MAX_AUTH_TOKEN_CACHE_SIZE (Option 0x04) of 4096 lets the publisher register tokens.
            definition.setup_bytes = bytes_of({0xaf, 0, 0, 3, 4, 0x90, 0});
            definition.publisher_exit_is_evidence = true;
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto messages = alias_messages(view);
            bool compliant_delete = false;
            for (const auto& retirement : messages) {
                if (retirement.action != TokenAction::Delete) continue;
                bool used = false;
                for (const auto& use : messages) {
                    if (use.action != TokenAction::UseAlias || use.alias != retirement.alias ||
                        use.event >= retirement.event) continue;
                    used = true;
                    const auto answer = answer_event(view, use.stream, use.frame_index);
                    // `answer` counts the events seen when the answer was written, so an answer
                    // is after the DELETE only if more events than the DELETE's index preceded it.
                    if (!answer || *answer > retirement.event) return {true, false};
                }
                compliant_delete = compliant_delete || used;
            }
            if (!view.window_ended()) return {false, std::nullopt};
            return {true, compliant_delete ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 5.2 lines 1880-1890: an uncommitted Subgroup is reset (D21-5-2-MUST-130) ------
// "If the Object Forwarding Preference is Subgroup and the value of
// SUBGROUP_DELIVERY_TIMEOUT is not zero, the MOQT implementation MUST start a timer ...
// once it becomes aware that all of the objects on the subgroup have been published
// ... If the timer expires before the underlying transport stream reaches 'all data
// committed' state, the implementation MUST reset the stream."
//
// The subscription asks for SUBGROUP_DELIVERY_TIMEOUT (Parameter 0x06) of 200 ms for
// Group 0 and the runner holds back flow control credit on the publisher's data
// streams: each may carry only 64 bytes, a stream credit that is never raised (RFC 9000
// Section 4.1), so a Subgroup whose first Object is larger than that can never be
// committed. A completed Subgroup then has to be reset when the timer expires, and
// that reset reaches the runner because the stream is still unfinished there. (Merely
// withholding acknowledgements would leave a fully received stream, where a later
// RESET_STREAM is not delivered to the application.) The fixture's Group 0 is complete
// and its first Object is larger than 64 bytes.
constexpr std::uint64_t kSubgroupDeliveryTimeout = 0x06;
constexpr std::uint64_t kSubgroupTimeoutMs = 200;
constexpr std::uint64_t kHeldStreamCredit = 64;

Spec uncommitted_subgroup_spec() {
    return spec("d21-subgroup-completion-withheld-acknowledgments",
        {{"D21-5-2-MUST-130", "d21-uncommitted-subgroup-timeout-reset"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            definition.initial_peer_uni_stream_data = kHeldStreamCredit;
            definition.hold_uni_stream_credit = true;
            auto whole_group = location_pair(0, 0);
            put_vi(whole_group, 0);
            definition.writes.push_back(request_write(subscribe_frame(1, fixture,
                {param_vi(kSubgroupDeliveryTimeout, kSubgroupTimeoutMs), param_u8(0x10, 1),
                 param_lp(0x21, whole_group)})));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (rejected(view, 0)) return {true, std::nullopt};
            const auto alias = alias_of(view, 0);
            if (!alias) return {view.close().has_value(), std::nullopt};
            bool unfinished = false;
            for (const auto& stream : subgroup_streams(view, *alias)) {
                const auto* record = view.stream(stream.id);
                if (!record || record->fin) continue;
                if (record->reset) return {true, true};
                unfinished = true;
            }
            if (!view.window_ended()) return {false, std::nullopt};
            // A stream still open at the end of the window, long after the timer, was never reset.
            return {true, unfinished ? std::optional<bool>{false} : std::nullopt};
        },
        true);
}

// ---- Section 8.9 lines 3270-3271 and 3288-3292: operator-configured credentials -----------
// The runner cannot mint a credential for a Token Type, so these two scenarios send
// what the operator supplied for a Token Type the publisher is configured to understand
// (RunManager options --invalid-auth-token and --expired-auth-token). Without a
// credential nothing is sent and the context ends unscored.
constexpr std::uint64_t kRequestErrorMalformedToken = 0x4;  // Section 12.3, Table 19
constexpr std::uint64_t kRequestErrorNotSupported = 0x3;
constexpr std::uint64_t kRequestErrorExpiredToken = 0x5;
constexpr std::uint64_t kDuplicateAuthTokenAlias = 0x14;    // Section 12.2, Table 18
constexpr std::uint64_t kAuthorization = 0x03;
constexpr std::uint64_t kTrackStatus = 0xd;

Bytes token_status(std::uint64_t request_id, const Fixture& fixture, const Bytes& token) {
    return request_frame(kTrackStatus, request_id, fixture, true, {param_lp(kAuthorization, token)});
}

std::optional<std::uint64_t> request_error_code(const std::vector<Frame>& frames) {
    if (frames.empty() || frames.front().type != kRequestError) return std::nullopt;
    wire::Cursor body(frames.front().body);
    return read_vi(body);
}

bool write_answered(const View& view, std::size_t write) {
    if (!view.write_frames(write).empty()) return true;
    const auto* stream = view.write_stream(write);
    return stream && (stream->fin || stream->reset);
}

Spec invalid_token_spec() {
    return spec("d21-request-well-formed-invalid-token",
        {{"D21-8-9-MUST-270", "d21-invalid-token-message-error"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            // A publisher that cannot serve the request may give up; that is what it did.
            definition.publisher_exit_is_evidence = true;
            // USE_VALUE (Alias Type 3): Token Type and Value, no Alias.
            if (fixture.credentials.invalid)
                definition.writes.push_back(request_write(token_status(1, fixture,
                    token_value(3, std::nullopt, fixture.credentials.invalid->token_type,
                                fixture.credentials.invalid->value))));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (view.write_bytes(0).empty()) return {true, std::nullopt};  // no credential configured
            const auto frames = view.write_frames(0);
            if (frames.empty()) return {view.close().has_value(), std::nullopt};
            // "The receiver of a message containing a well-formed Token structure that is
            // otherwise invalid MUST reject that message with an MALFORMED_AUTH_TOKEN error."
            if (frames.front().type == kRequestOk) return {true, false};
            const auto code = request_error_code(frames);
            if (!code) return {true, std::nullopt};
            // NOT_SUPPORTED says the Token Type is not understood: the precondition is unmet.
            if (*code == kRequestErrorNotSupported) return {true, std::nullopt};
            return {true, *code == kRequestErrorMalformedToken};
        },
        false);
}

RawProbeWrite after_response(Bytes bytes, std::size_t previous) {
    RawProbeWrite write = request_write(std::move(bytes), true);
    write.evidence_ready = [previous](const RawProbeGateInput& input) {
        const View view(input.prior_writes, input.events);
        return view.valid() && write_answered(view, previous);
    };
    return write;
}

// The expired credential is registered under Alias 1 (the message fails, yet "MUST
// register the Token Alias ... even if the message fails"), then used by Alias and
// registered a second time. "If a receiver detects that an authorization token has
// expired, it MUST retain the registered Alias until it is deleted by the sender ...
// Any message that references an expired token with Alias Type USE_ALIAS fails with
// EXPIRED_AUTH_TOKEN", and registering a registered Alias again closes the session
// with DUPLICATE_AUTH_TOKEN_ALIAS.
Spec expired_token_alias_spec() {
    return spec("d21-expired-token-alias-lifetime",
        {{"D21-8-9-MUST-273", "d21-expired-token-alias-retained-until-delete"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            // A publisher that cannot serve the requests may give up; that is what it did.
            definition.publisher_exit_is_evidence = true;
            if (!fixture.credentials.expired) return definition;
            const auto& credential = *fixture.credentials.expired;
            definition.writes.push_back(request_write(token_status(1, fixture,
                token_value(1, 1, credential.token_type, credential.value)), true));
            definition.writes.push_back(after_response(token_status(3, fixture,
                token_value(2, 1, std::nullopt, {})), 0));
            definition.writes.push_back(after_response(token_status(5, fixture,
                token_value(1, 1, credential.token_type, credential.value)), 1));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (view.write_bytes(0).empty()) return {true, std::nullopt};  // no credential configured
            const auto registration = view.write_frames(0);
            // The credential must really be expired: only EXPIRED_AUTH_TOKEN for the
            // registration shows that. A success or any other error (UNAUTHORIZED,
            // NOT_SUPPORTED, ...) leaves the precondition unmet.
            if (!registration.empty()) {
                const auto registration_code = request_error_code(registration);
                if (!registration_code || *registration_code != kRequestErrorExpiredToken) return {true, std::nullopt};
            }
            if (!write_answered(view, 1)) return {view.close().has_value(), std::nullopt};
            const auto use = view.write_frames(1);
            if (use.empty()) return {true, std::nullopt};
            const auto code = request_error_code(use);
            if (!code) return {true, std::nullopt};
            // An Alias the receiver no longer knows (UNKNOWN_AUTH_TOKEN_ALIAS) means it was dropped.
            if (view.unknown_alias_code() && *code == *view.unknown_alias_code()) return {true, false};
            if (*code != kRequestErrorExpiredToken) return {true, false};
            // Still registered, so registering it again before a DELETE is a duplicate.
            const auto* close = view.close() ? &*view.close() : nullptr;
            if (close && close->application && close->code == kDuplicateAuthTokenAlias) return {true, true};
            if (!view.write_frames(2).empty()) return {true, false};
            return {close != nullptr, std::nullopt};
        },
        false);
}

}  // namespace

std::vector<Spec> residual_token_specs() {
    std::vector<Spec> result;
    result.push_back(pending_alias_delete_spec());
    result.push_back(uncommitted_subgroup_spec());
    result.push_back(invalid_token_spec());
    result.push_back(expired_token_alias_spec());
    return result;
}

}  // namespace moq::interop::scenarios::d21c
