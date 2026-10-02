#include "moq/interop/scenarios/draft21_gap_a_token.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/token.h"

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {

namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;

constexpr std::size_t kMaximumBytes = 65546;
constexpr std::size_t kMaximumEvents = 4096;
constexpr std::size_t kMaximumStreams = 64;
constexpr std::uint64_t kRequestOk = 0x07;
constexpr std::uint64_t kRequestError = 0x05;
constexpr std::uint64_t kTrackStatus = 0x0d;
constexpr std::uint64_t kUnauthorized = 0x1;                  // Section 12.3
constexpr std::uint64_t kAuthTokenCacheOverflow = 0x13;       // Section 12.2
constexpr std::uint64_t kDuplicateAuthTokenAlias = 0x14;      // Section 12.2
constexpr std::uint64_t kUnknownAuthTokenAliasClose = 0x17;   // Section 12.2
// Token value sizes. The cost of a registration is 16 plus its value size.
constexpr std::size_t kSmallValue = 1;
constexpr std::size_t kOversizedValue = 8192;
constexpr std::uint64_t kAliasA = 1;       // registered or retired by the probe
constexpr std::uint64_t kAliasControl = 2; // never registered

struct Fixture {
    Namespace ns;
    Bytes name;
};

void integer(Bytes& output, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64 capacity");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}

Bytes frame(std::uint64_t type, const Bytes& body) {
    if (body.size() > 65535) throw std::invalid_argument("token probe request too large");
    Bytes result;
    integer(result, type);
    result.push_back(static_cast<std::byte>(body.size() >> 8u));
    result.push_back(static_cast<std::byte>(body.size() & 255u));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

enum class Use { Register, Delete, UseAlias, UseValue };

// Section 8.9, Figure 3. Token Type 0 is negotiated out of band; no
// understanding of the value is needed because only the Alias is observed.
Bytes token(Use use, std::uint64_t alias, std::size_t value_size) {
    Bytes result;
    const Bytes value(value_size, std::byte{'k'});
    switch (use) {
    case Use::Register:
        integer(result, 1); integer(result, alias); integer(result, 0);
        result.insert(result.end(), value.begin(), value.end());
        break;
    case Use::Delete: integer(result, 0); integer(result, alias); break;
    case Use::UseAlias: integer(result, 2); integer(result, alias); break;
    case Use::UseValue:
        integer(result, 3); integer(result, 0);
        result.insert(result.end(), value.begin(), value.end());
        break;
    }
    return result;
}

// Section 9.13: TRACK_STATUS has the SUBSCRIBE layout. The only parameter is
// AUTHORIZATION TOKEN (0x03, length-prefixed).
Bytes track_status(std::uint64_t request_id, const Fixture& fixture, const Bytes& token_bytes,
                   bool missing_track) {
    Bytes body;
    integer(body, request_id);
    integer(body, fixture.ns.size());
    for (const auto& field : fixture.ns) {
        integer(body, field.size());
        body.insert(body.end(), field.begin(), field.end());
    }
    auto name = fixture.name;
    if (missing_track) name.push_back(std::byte{0xff});
    integer(body, name.size());
    body.insert(body.end(), name.begin(), name.end());
    integer(body, 1);
    integer(body, 3);
    integer(body, token_bytes.size());
    body.insert(body.end(), token_bytes.begin(), token_bytes.end());
    return frame(kTrackStatus, body);
}

std::optional<std::uint64_t> number(wire::Cursor& cursor) {
    const auto decoded = wire::read_vi64(cursor);
    if (const auto* value = std::get_if<std::uint64_t>(&decoded)) return *value;
    return {};
}

// MAX_AUTH_TOKEN_CACHE_SIZE (Section 9.1.3) from a decoded SETUP; the default
// is 0, which prohibits Aliases.
std::optional<std::uint64_t> peer_cache_size(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = d21::decode_setup(cursor);
    const auto* setup = std::get_if<d21::SetupMessage>(&decoded);
    if (!setup) return {};
    for (const auto& option : setup->options)
        if (option.type == 4)
            if (const auto* value = std::get_if<std::uint64_t>(&option.value)) return *value;
    return 0;
}

// ------------------------------------------------------------- observation

struct StreamData {
    Bytes bytes;
    std::size_t first_event{0};
    bool fin{false};
    bool reset{false};
};

struct Signature {
    enum class Kind { Pending, Reset, Malformed, Ok, Error } kind{Kind::Pending};
    std::uint64_t code{0};
    bool operator==(const Signature& other) const { return kind == other.kind && code == other.code; }
    bool complete() const { return kind == Kind::Ok || kind == Kind::Error; }
};

struct Observed {
    std::map<transport::StreamId, StreamData> streams;
    std::optional<std::uint64_t> close_code;  // peer application close
    bool bounded{true};
};

Observed observe_events(std::span<const transport::TransportEvent> events) {
    Observed result;
    if (events.size() > kMaximumEvents) { result.bounded = false; return result; }
    std::size_t total = 0;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
            if (close->error_space == transport::CloseErrorSpace::Application)
                result.close_code = close->error_code;
            break;
        }
        if (std::holds_alternative<transport::LocalCloseEvent>(event) ||
            std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
            std::holds_alternative<transport::TransportErrorEvent>(event) ||
            std::holds_alternative<transport::EventQueueOverflowEvent>(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (data->data.size() > kMaximumBytes - total ||
                (!result.streams.contains(data->stream_id) && result.streams.size() >= kMaximumStreams)) {
                result.bounded = false;
                return result;
            }
            total += data->data.size();
            auto [found, inserted] = result.streams.try_emplace(data->stream_id);
            if (inserted) found->second.first_event = index;
            if (found->second.fin || found->second.reset) continue;
            found->second.bytes.insert(found->second.bytes.end(), data->data.begin(), data->data.end());
            found->second.fin = data->fin;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if (!result.streams.contains(reset->stream_id) && result.streams.size() >= kMaximumStreams) {
                result.bounded = false;
                return result;
            }
            auto [found, inserted] = result.streams.try_emplace(reset->stream_id);
            if (inserted) found->second.first_event = index;
            found->second.reset = true;
        }
    }
    return result;
}

// A complete first message on a response direction: Type (vi64), Length (16),
// then that many body bytes (Section 9).
struct FirstMessage {
    std::uint64_t type{0};
    Bytes body;
};
std::optional<FirstMessage> first_message(const Bytes& bytes) {
    wire::Cursor cursor(bytes);
    const auto type = number(cursor);
    if (!type) return {};
    const auto length = wire::read_bytes(cursor, 2);
    const auto* prefix = std::get_if<std::span<const std::byte>>(&length);
    if (!prefix) return {};
    const auto size = (static_cast<std::size_t>(std::to_integer<unsigned>((*prefix)[0])) << 8u) |
                      std::to_integer<unsigned>((*prefix)[1]);
    const auto body = wire::read_bytes(cursor, size);
    const auto* span = std::get_if<std::span<const std::byte>>(&body);
    if (!span) return {};
    return FirstMessage{*type, Bytes(span->begin(), span->end())};
}

// Request-stream response signature for one request.
Signature signature_of(const RawProbeTranscript& t, const Observed& observed, std::size_t write_index) {
    Signature result;
    if (write_index >= t.writes.size()) return result;
    const auto& write = t.writes[write_index];
    if (!write.stream_id || !write.delivery_event_count) return result;
    const auto found = observed.streams.find(*write.stream_id);
    if (found == observed.streams.end() || found->second.first_event < *write.delivery_event_count)
        return result;
    const auto message = first_message(found->second.bytes);
    if (!message) {
        if (found->second.fin) result.kind = Signature::Kind::Malformed;
        else if (found->second.reset) result.kind = Signature::Kind::Reset;
        return result;
    }
    if (message->type == kRequestOk) {
        result.kind = Signature::Kind::Ok;
    } else if (message->type == kRequestError) {
        wire::Cursor error(message->body);
        const auto code = number(error);
        if (!code) { result.kind = Signature::Kind::Malformed; return result; }
        result.kind = Signature::Kind::Error;
        result.code = *code;
    } else {
        result.kind = Signature::Kind::Malformed;
    }
    return result;
}

enum class State { Pending, Inconclusive, Pass, Fail };

struct Responses {
    std::vector<Signature> signatures;
    std::optional<std::uint64_t> close_code;
    bool bounded{true};
};

Responses read_responses(const RawProbeTranscript& t) {
    Responses result;
    const auto observed = observe_events(t.events);
    result.bounded = observed.bounded;
    result.close_code = observed.close_code;
    for (std::size_t index = 0; index < t.writes.size(); ++index)
        result.signatures.push_back(signature_of(t, observed, index));
    return result;
}

bool all_complete(const Responses& responses, std::initializer_list<std::size_t> indexes) {
    return std::all_of(indexes.begin(), indexes.end(), [&](std::size_t index) {
        return index < responses.signatures.size() && responses.signatures[index].complete();
    });
}

bool pending_or_unusable(const Responses& responses, std::initializer_list<std::size_t> indexes,
                         State& state) {
    if (all_complete(responses, indexes)) return false;
    for (const auto index : indexes) {
        if (index < responses.signatures.size() &&
            (responses.signatures[index].kind == Signature::Kind::Malformed ||
             responses.signatures[index].kind == Signature::Kind::Reset)) {
            state = State::Inconclusive;
            return true;
        }
    }
    // A closed session never produces the missing responses.
    state = responses.close_code ? State::Inconclusive : State::Pending;
    return true;
}

State evaluate(Draft21TokenAspect aspect, const RawProbeTranscript& t,
               std::optional<std::uint64_t> compat) {
    if (t.writes.empty()) return State::Pending;
    const auto responses = read_responses(t);
    if (!responses.bounded) return State::Inconclusive;
    const auto& s = responses.signatures;
    State pending = State::Pending;
    const auto unknown_by_mapping = [&](const Signature& signature) {
        return compat && signature.kind == Signature::Kind::Error && signature.code == *compat;
    };
    switch (aspect) {
    case Draft21TokenAspect::RegisterAssociates: {
        // [0] REGISTER A, [1] USE_ALIAS A, [2] USE_VALUE, [3] USE_ALIAS control.
        if (responses.close_code == kDuplicateAuthTokenAlias) return State::Fail;
        if (pending_or_unusable(responses, {1, 2, 3}, pending)) return pending;
        if (unknown_by_mapping(s[1])) return State::Fail;
        if (!(s[1] == s[2])) return State::Fail;
        return s[1] == s[3] ? State::Inconclusive : State::Pass;
    }
    case Draft21TokenAspect::DeleteRetiresAlias: {
        // [0] REGISTER A, [1] USE_ALIAS A, [2] USE_ALIAS control, [3] DELETE A,
        // [4] USE_ALIAS A again, [5] REGISTER A again.
        if (responses.close_code == kDuplicateAuthTokenAlias) return State::Fail;
        if (pending_or_unusable(responses, {1, 2, 3, 4}, pending)) return pending;
        // Before deletion the Alias must be distinguishable from an unknown one.
        if (s[1] == s[2] && !unknown_by_mapping(s[2])) return State::Inconclusive;
        if (!(s[4] == s[2])) return State::Fail;
        if (!all_complete(responses, {5})) {
            pending = responses.close_code ? State::Inconclusive : State::Pending;
            return pending;
        }
        return State::Pass;
    }
    case Draft21TokenAspect::DeletedAliasRejected: {
        // [0] REGISTER A, [1] DELETE A, [2] USE_ALIAS A.
        if (pending_or_unusable(responses, {2}, pending)) return pending;
        if (!compat) return State::Inconclusive;
        return unknown_by_mapping(s[2]) ? State::Pass : State::Fail;
    }
    case Draft21TokenAspect::RegisterRetainedAfterUnauthorized:
    case Draft21TokenAspect::RegisterRetainedAfterOtherError: {
        // [0] REGISTER A in a request that fails, [1] USE_ALIAS A, [2] control.
        if (pending_or_unusable(responses, {0}, pending)) return pending;
        if (s[0].kind != Signature::Kind::Error) return State::Inconclusive;
        if (aspect == Draft21TokenAspect::RegisterRetainedAfterUnauthorized && s[0].code != kUnauthorized)
            return State::Inconclusive;
        // A session closed with UNKNOWN_AUTH_TOKEN_ALIAS says the Alias was not kept.
        if (responses.close_code == kUnknownAuthTokenAliasClose) return State::Fail;
        if (pending_or_unusable(responses, {1, 2}, pending)) return pending;
        if (unknown_by_mapping(s[1])) return State::Fail;
        return s[1] == s[2] ? State::Inconclusive : State::Pass;
    }
    case Draft21TokenAspect::SetupRegisterFallsBack: {
        // [0] USE_ALIAS A, [1] USE_ALIAS control; A was only in SETUP.
        if (responses.close_code == kAuthTokenCacheOverflow) return State::Fail;
        if (pending_or_unusable(responses, {0, 1}, pending)) return pending;
        return s[0] == s[1] ? State::Pass : State::Fail;
    }
    }
    return State::Pending;
}

// ---------------------------------------------------------------- profiles

// A request is sent once the previous request's response is complete: Alias
// state is per session and a USE_ALIAS must not race its REGISTER (Section 8.9).
RawProbeWrite request(Bytes bytes, bool gated) {
    RawProbeWrite result{RawProbeChannel::NewBidi, std::move(bytes), true};
    if (gated) {
        result.evidence_ready = [](const RawProbeGateInput& input) {
            if (input.prior_writes.empty()) return false;
            const auto& previous = input.prior_writes.back();
            if (!previous.stream_id || !previous.delivery_event_count ||
                *previous.delivery_event_count > input.events.size()) return false;
            const auto observed = observe_events(input.events);
            const auto found = observed.streams.find(*previous.stream_id);
            if (found == observed.streams.end() || found->second.first_event < *previous.delivery_event_count)
                return false;
            return first_message(found->second.bytes).has_value() || found->second.reset;
        };
    }
    return result;
}

std::vector<Draft21TokenProbe> profiles(std::chrono::milliseconds deadline, const Fixture& fixture) {
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid token probe fixture or deadline");
    std::size_t total = fixture.name.size() + 1;
    for (const auto& field : fixture.ns) total += field.size();
    if (total > 4096) throw std::invalid_argument("token probe fixture leaves no room for a missing track");
    std::vector<Draft21TokenProbe> result;
    struct Step { Use use; std::uint64_t alias; bool missing; };
    const auto add = [&](const char* requirement, const char* scenario, const char* evaluator,
                         Draft21TokenAspect aspect, std::vector<Step> steps) {
        RawProbeDefinition definition;
        definition.id = scenario;
        definition.deadline = deadline;
        const bool oversized = aspect == Draft21TokenAspect::SetupRegisterFallsBack;
        if (oversized) {
            // Section 9.1.4: this server SETUP registers a token whose cost
            // exceeds the peer's advertised cache; the peer must not fail.
            Bytes option_value = token(Use::Register, kAliasA, kOversizedValue);
            Bytes body;
            integer(body, 3);
            integer(body, option_value.size());
            body.insert(body.end(), option_value.begin(), option_value.end());
            Bytes setup;
            integer(setup, 0x2f00);
            setup.push_back(static_cast<std::byte>(body.size() >> 8u));
            setup.push_back(static_cast<std::byte>(body.size() & 255u));
            setup.insert(setup.end(), body.begin(), body.end());
            definition.setup_bytes = std::move(setup);
            definition.peer_setup_ready = [](std::span<const std::byte> input) {
                const auto cache = peer_cache_size(input);
                return cache && *cache < 16 + kOversizedValue;
            };
        } else {
            definition.setup_bytes = {std::byte{0xaf}, std::byte{0}, std::byte{0}, std::byte{0}};
            definition.peer_setup_ready = [](std::span<const std::byte> input) {
                const auto cache = peer_cache_size(input);
                return cache && *cache >= 16 + kSmallValue;
            };
        }
        std::uint64_t id = 1;
        for (std::size_t index = 0; index < steps.size(); ++index, id += 2) {
            const auto size = steps[index].use == Use::Register || steps[index].use == Use::UseValue
                ? kSmallValue : 0;
            definition.writes.push_back(request(
                track_status(id, fixture, token(steps[index].use, steps[index].alias, size),
                             steps[index].missing), index != 0));
        }
        definition.response_ready = [aspect](const RawProbeTranscript& t) {
            return evaluate(aspect, t, t.unknown_auth_token_alias_compatibility_code) != State::Pending;
        };
        result.push_back({requirement, evaluator, aspect, std::move(definition)});
    };
    add("D21-8-9-MUST-264", "d21-token-delete-and-reuse", "d21-delete-retires-token-alias",
        Draft21TokenAspect::DeleteRetiresAlias,
        {{Use::Register, kAliasA, false}, {Use::UseAlias, kAliasA, false},
         {Use::UseAlias, kAliasControl, false}, {Use::Delete, kAliasA, false},
         {Use::UseAlias, kAliasA, false}, {Use::Register, kAliasA, false}});
    add("D21-8-9-MUST-265", "d21-token-register-alias-lifetime",
        "d21-register-preserves-token-association", Draft21TokenAspect::RegisterAssociates,
        {{Use::Register, kAliasA, false}, {Use::UseAlias, kAliasA, false},
         {Use::UseValue, 0, false}, {Use::UseAlias, kAliasControl, false}});
    add("D21-8-9-MUST-269", "d21-request-deleted-token-alias", "d21-unknown-token-alias-message-error",
        Draft21TokenAspect::DeletedAliasRejected,
        {{Use::Register, kAliasA, false}, {Use::Delete, kAliasA, false}, {Use::UseAlias, kAliasA, false}});
    add("D21-8-9-MUST-271", "d21-register-token-on-unauthorized-request",
        "d21-register-on-nonsession-message-error", Draft21TokenAspect::RegisterRetainedAfterUnauthorized,
        {{Use::Register, kAliasA, false}, {Use::UseAlias, kAliasA, false},
         {Use::UseAlias, kAliasControl, false}});
    add("D21-8-9-MUST-271", "d21-register-token-on-other-request-error",
        "d21-register-on-nonsession-message-error", Draft21TokenAspect::RegisterRetainedAfterOtherError,
        {{Use::Register, kAliasA, true}, {Use::UseAlias, kAliasA, false},
         {Use::UseAlias, kAliasControl, false}});
    add("D21-9-1-4-MUST-308", "d21-setup-register-use-value-fallback",
        "d21-setup-overflow-token-used-without-registration", Draft21TokenAspect::SetupRegisterFallsBack,
        {{Use::UseAlias, kAliasA, false}, {Use::UseAlias, kAliasControl, false}});
    return result;
}

// Only the track identity is configurable. Write 1 is a TRACK_STATUS for the
// fixture track in every probe (write 0 may name a missing track).
std::optional<Fixture> recover_fixture(std::span<const std::byte> input) {
    if (input.size() > kMaximumBytes) return {};
    wire::Cursor cursor(input);
    const auto decoded = d21::decode_request_frame(cursor, true);
    const auto* request = std::get_if<d21::RequestFrame>(&decoded);
    if (!request || request->type.type != kTrackStatus || cursor.remaining() != 0) return {};
    wire::Cursor body(request->body);
    const auto id = number(body);
    const auto count = number(body);
    if (id != 3 || !count || *count > 32) return {};
    Fixture fixture;
    std::size_t total = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto field = wire::read_length_prefixed_bytes(body, 4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&field);
        if (!value || value->empty()) return {};
        total += value->size();
        fixture.ns.emplace_back(value->begin(), value->end());
    }
    const auto name = wire::read_length_prefixed_bytes(body, 4096 - total);
    const auto* value = std::get_if<std::span<const std::byte>>(&name);
    if (!value) return {};
    fixture.name.assign(value->begin(), value->end());
    if (!fetch_first_object_fixture_valid(fixture.ns, fixture.name)) return {};
    return fixture;
}

bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
           std::holds_alternative<transport::LocalCloseEvent>(event) ||
           std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
           std::holds_alternative<transport::TransportErrorEvent>(event) ||
           std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}

}  // namespace

std::vector<Draft21TokenProbe> draft21_gap_a_token_probes(std::chrono::milliseconds deadline,
                                                          Namespace ns, Bytes name) {
    return profiles(deadline, {std::move(ns), std::move(name)});
}

// The accepted writes of an interrupted context must be exactly a prefix of the
// expected stimulus, fully accepted and in the same order.
bool partial_stimulus_valid(const RawProbeTranscript& t, const RawProbeDefinition& expected) {
    if (!t.transport_established || !t.peer_setup_received ||
        t.writes.size() != expected.writes.size() || t.setup.write.bytes != expected.setup_bytes ||
        t.setup.accepted != expected.setup_bytes.size() || !t.setup.stream_id ||
        ((*t.setup.stream_id) & 3u) != 3u) return false;
    bool open_tail = false;
    for (std::size_t index = 0; index < t.writes.size(); ++index) {
        const auto& write = t.writes[index];
        const auto& wanted = expected.writes[index];
        const bool untouched = !write.stream_id && write.accepted == 0 && !write.fin_accepted &&
                               !write.delivery_event_count;
        if (untouched) { open_tail = true; continue; }
        // Accepted writes form a prefix, each exactly as the stimulus defines it.
        if (open_tail || !write.stream_id || !write.delivery_event_count ||
            *write.delivery_event_count > t.events.size() || write.write.bytes != wanted.bytes ||
            write.write.fin != wanted.fin || write.write.channel != wanted.channel ||
            write.accepted != wanted.bytes.size() || write.fin_accepted != wanted.fin) return false;
    }
    return true;
}

std::optional<bool> evaluate_draft21_gap_a_token_probe(const RawProbeTranscript& t,
                                                       const Draft21TokenProbe& p) {
    if (t.scenario_id != p.definition.id || p.definition.deadline.count() <= 0 ||
        t.harness_failed || t.writes.size() < 2) return {};
    const auto fixture = recover_fixture(t.writes[1].write.bytes);
    if (!fixture) return {};
    const auto candidates = profiles(p.definition.deadline, *fixture);
    const auto expected = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return candidate.requirement_id == p.requirement_id && candidate.evaluator_id == p.evaluator_id &&
               candidate.aspect == p.aspect && candidate.definition.id == p.definition.id;
    });
    if (expected == candidates.end()) return {};
    const auto end = std::find_if(t.events.begin(), t.events.end(), terminal);
    RawProbeTranscript prefix = t;
    prefix.events.assign(t.events.begin(), end);
    if (t.complete) {
        if (!raw_probe_stimulus_valid(prefix, expected->definition)) return {};
    } else {
        // A peer that closes the session mid-sequence ends the context early;
        // the close is only evidence when the stimulus up to it was exact.
        const bool peer_closed = end != t.events.end() &&
            std::holds_alternative<transport::PeerCloseEvent>(*end);
        if (!peer_closed || !partial_stimulus_valid(prefix, expected->definition)) return {};
    }
    const auto state = evaluate(p.aspect, t, t.unknown_auth_token_alias_compatibility_code);
    if (state == State::Pass) return true;
    if (state == State::Fail) return false;
    return {};
}

}  // namespace moq::interop::scenarios
