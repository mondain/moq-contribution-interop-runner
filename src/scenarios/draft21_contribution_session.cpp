// Draft-21 contribution profiles that exercise session and request-stream
// behavior of the publisher under test: SETUP handling, REQUEST_UPDATE
// accounting, response message types, FETCH range errors, namespace
// discovery ordering, Message Parameter serialization and padding.

#include "draft21_contribution_filter_testing.h"
#include "draft21_contribution_support.h"
#include "moq/interop/scenarios/wire_draft.h"

#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <limits>
#include <set>

namespace moq::interop::scenarios::d21c {
namespace {
namespace d21 = wire::draft21;

constexpr std::uint64_t kAuthTokenCacheOverflow = 0x13;
constexpr std::uint64_t kTooManyRequestUpdates = 0x1b;
constexpr std::uint64_t kInvalidRange = 0x11;
constexpr std::uint64_t kGreaseValue = 0x9d;
constexpr std::uint64_t kPaddingStream = 0x132b3e28;
constexpr std::uint64_t kPaddingDatagram = 0x132b3e29;
constexpr std::size_t kLargeToken = 40000;
constexpr std::size_t kPaddingBytes = 131072;
constexpr std::size_t kMaximumLimit = 64;

bool application_close(const View& view, std::uint64_t code) {
    return view.close() && view.close()->application && view.close()->code == code;
}

// A complete SUBSCRIBE_OK or REQUEST_ERROR on the request stream proves the
// publisher kept serving requests; a close without it is only inconclusive.
Judgement liveness(const View& view, std::size_t write, std::optional<std::uint64_t> failing_close) {
    if (failing_close && application_close(view, *failing_close)) return {true, false};
    if (opens_with_response(view.write_frames(write))) return {true, true};
    if (view.close()) return {true, std::nullopt};
    return {false, std::nullopt};
}

Bytes setup_bytes(const d21::SetupMessage& message) {
    wire::ByteWriter output(65546);
    if (d21::encode_setup(message, output)) throw std::logic_error("unencodable contribution SETUP");
    return {output.bytes().begin(), output.bytes().end()};
}

Bytes text(const char* value) {
    Bytes result;
    for (const char* cursor = value; *cursor; ++cursor) result.push_back(static_cast<std::byte>(*cursor));
    return result;
}

RawProbeWrite request_write(Bytes bytes, bool fin = false) {
    return {RawProbeChannel::NewBidi, std::move(bytes), fin};
}

// Writes a follow-up on the stream opened by write `base`, once its
// SUBSCRIBE_OK is complete.
RawProbeWrite update_write(Bytes bytes, std::size_t base) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), false, base};
    write.peer_response_ready = subscribe_ok_ready;
    return write;
}

// ---- SETUP token registration (Section 9.1.4) -----------------------------
Spec register_spec(const char* scenario, bool exceeds_announced_cache) {
    Spec spec;
    spec.scenario = scenario;
    spec.rows = {{"D21-9-1-4-MUST-NOT-307", "d21-setup-overflow-is-not-cache-overflow-close"}};
    spec.build = [exceeds_announced_cache](const Fixture& fixture) {
        auto definition = base_definition("");
        const Bytes value = exceeds_announced_cache ? Bytes(kLargeToken, std::byte{'a'}) : Bytes{};
        definition.setup_bytes = setup_bytes({{{3, token_value(1, 1, kGreaseValue, value)}}});
        // The oversized token must exceed the publisher's announced limit,
        // or its default of zero; a token cost is 16 bytes plus its value.
        definition.peer_setup_ready = [exceeds_announced_cache](auto input) {
            if (!setup_decodes(input)) return false;
            const auto limit = setup_numeric_option(input, 4);
            return exceeds_announced_cache ? limit && *limit >= 1 && *limit < 16 + kLargeToken
                                           : !limit || *limit == 0;
        };
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        return definition;
    };
    spec.judge = [](const View& view) { return liveness(view, 0, kAuthTokenCacheOverflow); };
    return spec;
}

// ---- GREASE (Section 13) --------------------------------------------------
Spec grease_setup_spec() {
    Spec spec;
    spec.scenario = "d21-grease-setup-options";
    spec.rows = {{"D21-13-MUST-595", "d21-grease-setup-options-ignored"},
                 {"D21-16-4-MUST-629", "d21-grease-setup-options-ignored"}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        // Known MOQT_IMPLEMENTATION first, then reserved GREASE options: an
        // odd length-prefixed one (repeated) and an even integer-valued one.
        definition.setup_bytes = setup_bytes({{
            {7, text("interop")},
            {kGreaseValue, text("abc")},
            {kGreaseValue, text("x")},
            {0x11c, std::uint64_t{0x1234}}}});
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        return definition;
    };
    spec.judge = [](const View& view) { return liveness(view, 0, std::nullopt); };
    return spec;
}

Bytes unknown_code_error_frame() {
    Bytes body;
    put_vi(body, kGreaseValue);  // unknown REQUEST_ERROR code
    put_vi(body, 0);             // do not retry
    put_vi(body, 0);             // empty reason
    return frame(kRequestError, body);
}

Spec grease_request_error_spec() {
    Spec spec;
    spec.scenario = "d21-grease-request-error";
    spec.rows = {{"D21-13-MUST-NOT-601", "d21-unknown-request-error-preserves-session"}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.peer_request_ready = [](auto input) {
            wire::Cursor cursor(input);
            return std::holds_alternative<d21::PublishMessage>(decode_publish_for_wire(cursor));
        };
        // Reject the publisher's own PUBLISH with an unknown code, then use
        // a fresh request on the same session.
        definition.writes.push_back({RawProbeChannel::PeerBidi, unknown_code_error_frame(), true});
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        return definition;
    };
    spec.judge = [](const View& view) { return liveness(view, 1, std::nullopt); };
    return spec;
}

// ---- REQUEST_UPDATE accounting (Sections 9.1.7, 9.5, 9.5.1) ----------------
Bytes updates(std::uint64_t first_id, std::size_t count, bool failing) {
    Bytes result;
    for (std::size_t index = 0; index < count; ++index) {
        std::vector<Param> params;
        if (failing) params.push_back(param_lp(0x03, bytes_of({2, 0})));  // USE_ALIAS for an unregistered alias
        else params.push_back(param_u8(0x20, 100 + static_cast<unsigned>(index)));
        const auto encoded = request_update_frame(first_id + 2 * index, params);
        result.insert(result.end(), encoded.begin(), encoded.end());
    }
    return result;
}

struct Responses {
    std::size_t count{0};
    std::size_t errors{0};
    std::size_t oks{0};
    bool subscribed{false};
};

// REQUEST_OK and REQUEST_ERROR frames after SUBSCRIBE_OK are update responses.
Responses update_responses(const std::vector<Frame>& frames) {
    Responses result;
    if (frames.empty() || frames.front().type != kSubscribeOk) return result;
    result.subscribed = true;
    for (std::size_t index = 1; index < frames.size(); ++index) {
        if (frames[index].type == kRequestOk) { ++result.count; ++result.oks; }
        else if (frames[index].type == kRequestError) { ++result.count; ++result.errors; }
    }
    return result;
}

bool fence_answered(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    return !frames.empty() && (frames.front().type == kRequestOk || frames.front().type == kRequestError);
}

Spec update_spec(const char* scenario, std::size_t count, bool failing,
                 std::vector<RowBinding> rows) {
    Spec spec;
    spec.scenario = scenario;
    spec.rows = std::move(rows);
    spec.build = [count, failing](const Fixture& fixture) {
        auto definition = base_definition("");
        // Pipelining needs the publisher to accept this many outstanding
        // updates on one stream; an omitted or zero limit is unlimited.
        definition.peer_setup_ready = [count](auto input) {
            if (!setup_decodes(input)) return false;
            const auto limit = setup_numeric_option(input, 8);
            return !limit || *limit == 0 || *limit >= count;
        };
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        definition.writes.push_back(update_write(updates(3, count, failing), 0));
        // A later request on a fresh stream bounds how long duplicate
        // responses to the updates are awaited.
        definition.writes.push_back(request_write(track_status_frame(3 + 2 * count, fixture), true));
        return definition;
    };
    spec.judge = [count, failing](const View& view) -> Judgement {
        if (view.close()) return {true, std::nullopt};
        const auto responses = update_responses(view.write_frames(0));
        if (!responses.subscribed) return {false, std::nullopt};
        if (responses.count > count) return {true, false};
        const bool fenced = fence_answered(view, 2);
        if (failing) {
            if (responses.count == 0 || !fenced) return {false, std::nullopt};
            return {true, responses.errors == responses.count ? std::optional<bool>{true} : std::nullopt};
        }
        if (responses.count != count || !fenced) return {false, std::nullopt};
        return {true, responses.oks == count ? std::optional<bool>{true} : std::nullopt};
    };
    return spec;
}

std::vector<Spec> credit_specs() {
    constexpr const char* requirement = "D21-9-1-7-MUST-317";
    constexpr const char* evaluator = "d21-update-overrun-session-error";
    std::vector<Spec> result;

    // One more outstanding update than the announced limit on one stream.
    Spec overrun;
    overrun.scenario = "d21-request-update-overrun";
    overrun.rows = {{requirement, evaluator}};
    overrun.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.peer_setup_ready = [](auto input) {
            if (!setup_decodes(input)) return false;
            const auto limit = setup_numeric_option(input, 8);
            return limit && *limit >= 1 && *limit <= kMaximumLimit;
        };
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        auto burst = update_write({}, 0);
        burst.prepare_bytes = [](const RawProbeGateInput& input) -> std::optional<Bytes> {
            const auto setup = peer_setup_from_events(input.events);
            const auto limit = setup ? setup_option_value(*setup, 8) : std::nullopt;
            if (!limit || *limit < 1 || *limit > kMaximumLimit) return std::nullopt;
            return updates(3, static_cast<std::size_t>(*limit) + 1, false);
        };
        definition.writes.push_back(std::move(burst));
        return definition;
    };
    // An immediate responder might never observe the overrun, so only the
    // mandated close is conclusive.
    overrun.judge = [](const View& view) -> Judgement {
        if (application_close(view, kTooManyRequestUpdates)) return {true, true};
        const auto limit = view.peer_option(8);
        const auto responses = update_responses(view.write_frames(0));
        if (limit && responses.count >= *limit + 1) return {true, std::nullopt};
        if (view.close()) return {true, std::nullopt};
        return {false, std::nullopt};
    };
    result.push_back(std::move(overrun));

    // The limit is per request stream: spend it fully on two streams.
    Spec independent;
    independent.scenario = "d21-request-update-independent-streams";
    independent.rows = {{requirement, evaluator}};
    independent.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.peer_setup_ready = [](auto input) {
            if (!setup_decodes(input)) return false;
            const auto limit = setup_numeric_option(input, 8);
            return limit && *limit >= 1 && *limit <= 16;
        };
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        definition.writes.push_back(request_write(subscribe_frame(3, fixture)));
        for (std::size_t stream = 0; stream < 2; ++stream) {
            auto burst = update_write({}, stream);
            burst.prepare_bytes = [stream](const RawProbeGateInput& input) -> std::optional<Bytes> {
                const auto setup = peer_setup_from_events(input.events);
                const auto limit = setup ? setup_option_value(*setup, 8) : std::nullopt;
                if (!limit || *limit < 1 || *limit > 16) return std::nullopt;
                return updates(5 + stream * 2 * *limit, static_cast<std::size_t>(*limit), false);
            };
            definition.writes.push_back(std::move(burst));
        }
        return definition;
    };
    independent.judge = [](const View& view) -> Judgement {
        if (application_close(view, kTooManyRequestUpdates)) return {true, false};
        const auto limit = view.peer_option(8);
        const auto first = update_responses(view.write_frames(0));
        const auto second = update_responses(view.write_frames(1));
        if (limit && first.count >= *limit && second.count >= *limit) return {true, true};
        if (view.close()) return {true, std::nullopt};
        return {false, std::nullopt};
    };
    result.push_back(std::move(independent));

    // An omitted or zero limit is unlimited, not zero credits.
    Spec unlimited;
    unlimited.scenario = "d21-request-update-unlimited";
    unlimited.rows = {{requirement, evaluator}};
    constexpr std::size_t kUnlimitedBurst = 8;
    unlimited.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.peer_setup_ready = [](auto input) {
            if (!setup_decodes(input)) return false;
            const auto limit = setup_numeric_option(input, 8);
            return !limit || *limit == 0;
        };
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        definition.writes.push_back(update_write(updates(3, kUnlimitedBurst, false), 0));
        return definition;
    };
    unlimited.judge = [](const View& view) -> Judgement {
        if (application_close(view, kTooManyRequestUpdates)) return {true, false};
        if (update_responses(view.write_frames(0)).count >= kUnlimitedBurst) return {true, true};
        if (view.close()) return {true, std::nullopt};
        return {false, std::nullopt};
    };
    result.push_back(std::move(unlimited));
    return result;
}

// ---- SUBSCRIBE and FETCH responses (Sections 9.6, 9.7, 9.11) ----------------
Spec successful_subscribe_spec() {
    Spec spec;
    spec.scenario = "d21-successful-subscribe-response";
    spec.rows = {{"D21-9-6-MUST-354", "d21-successful-subscribe-uses-subscribe-ok"}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        return definition;
    };
    spec.judge = [](const View& view) -> Judgement {
        const auto* record = view.write_stream(0);
        const auto frames = view.write_frames(0);
        if (frames.empty() || !record) return {view.close().has_value(), std::nullopt};
        // REQUEST_OK is the generic update/PUBLISH response; it cannot
        // accept a SUBSCRIBE. An error rejects it and owes no SUBSCRIBE_OK.
        if (frames.front().type == kRequestOk) return {true, false};
        if (frames.front().type == kSubscribeOk)
            return {true, subscribe_ok_ready(record->bytes) ? std::optional<bool>{true} : std::nullopt};
        return {true, std::nullopt};
    };
    return spec;
}

std::optional<std::uint64_t> request_error_code(const Frame& frame_value) {
    if (frame_value.type != kRequestError) return std::nullopt;
    wire::Cursor body(frame_value.body);
    return read_vi(body);
}

// A start group far beyond any Largest Object.
Param far_start_filter() {
    return filter_param({std::uint64_t{1} << 62, 0});
}

Spec fetch_range_spec(const char* scenario, const char* requirement, const char* evaluator,
                      bool start_beyond, bool only_error_proves) {
    Spec spec;
    spec.scenario = scenario;
    spec.rows = {{requirement, evaluator}};
    spec.build = [start_beyond](const Fixture& fixture) {
        auto definition = base_definition("");
        std::vector<Param> params;
        if (start_beyond) params.push_back(far_start_filter());
        definition.writes.push_back(request_write(fetch_frame(1, fixture, params), true));
        return definition;
    };
    spec.judge = [only_error_proves](const View& view) -> Judgement {
        const auto frames = view.write_frames(0);
        if (frames.empty()) return {view.close().has_value(), std::nullopt};
        const auto code = request_error_code(frames.front());
        if (code) return {true, *code == kInvalidRange ? std::optional<bool>{true} : std::nullopt};
        // FETCH_OK beyond the Largest Object is only provably wrong when the
        // requested start cannot be published; an unknown fixture is not.
        if (frames.front().type == kFetchOk && !only_error_proves) return {true, false};
        return {true, std::nullopt};
    };
    return spec;
}

// ---- Namespace discovery order (Section 9.15) -------------------------------
std::optional<Namespace> namespace_suffix(const Frame& frame_value) {
    wire::Cursor body(frame_value.body);
    const auto count = read_vi(body);
    if (!count || *count > 32) return std::nullopt;
    Namespace suffix;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto field = wire::read_length_prefixed_bytes(body, 4096);
        const auto* value = std::get_if<std::span<const std::byte>>(&field);
        if (!value) return std::nullopt;
        suffix.emplace_back(value->begin(), value->end());
    }
    if (body.remaining() != 0) return std::nullopt;
    return suffix;
}

Spec namespace_order_spec() {
    Spec spec;
    spec.scenario = "d21-namespace-discovery-withdrawal-order";
    spec.rows = {{"D21-9-15-MUST-NOT-387", "d21-namespace-done-follows-namespace"}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(subscribe_namespace_frame(1, fixture.track_namespace)));
        return definition;
    };
    spec.judge = [](const View& view) -> Judgement {
        const auto frames = view.write_frames(0);
        if (frames.empty()) return {view.close().has_value(), std::nullopt};
        if (frames.front().type != kRequestOk) return {true, std::nullopt};
        std::set<Namespace> announced;
        bool withdrawn = false;
        for (std::size_t index = 1; index < frames.size(); ++index) {
            if (frames[index].type != kNamespace && frames[index].type != kNamespaceDone) continue;
            const auto suffix = namespace_suffix(frames[index]);
            if (!suffix) return {true, std::nullopt};
            if (frames[index].type == kNamespace) {
                announced.insert(*suffix);
            } else if (announced.erase(*suffix) == 0) {
                return {true, false};
            } else {
                withdrawn = true;
            }
        }
        // Only an observed withdrawal exercises the ordering rule.
        if (withdrawn) return {true, true};
        return {view.close().has_value(), std::nullopt};
    };
    return spec;
}

// ---- Message Parameter serialization (Section 9.20) --------------------------
bool permits_repeat(std::uint64_t type) {
    // Section 8.9 permits repeated tokens with distinct values; Section
    // 3.3.2 permits repeated range filters with distinct keys.
    return type == 0x03 || (type >= 0x25 && type <= 0x29);
}

struct Walk {
    std::size_t declared{0};
    std::size_t parsed{0};
    bool structure{true};
    bool overflow{false};
    bool unknown{false};
    bool forbidden_repeat{false};
};

Walk walk_parameters(wire::Cursor& body, std::uint64_t count) {
    Walk walk;
    walk.declared = static_cast<std::size_t>(count);
    std::uint64_t previous = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto delta = read_vi(body);
        if (!delta) { walk.structure = false; return walk; }
        if (*delta > std::numeric_limits<std::uint64_t>::max() - previous) {
            walk.overflow = true;
            return walk;
        }
        const auto type = previous + *delta;
        if (index != 0 && *delta == 0 && !permits_repeat(type)) walk.forbidden_repeat = true;
        previous = type;
        bool ok = true;
        switch (type) {
            case 0x10: case 0x20: case 0x22: case 0x35:
                ok = read_n(body, 1).has_value();
                break;
            case 0x02: case 0x04: case 0x06: case 0x08: case 0x0a: case 0x32:
                ok = read_vi(body).has_value();
                break;
            case 0x09:
                ok = read_vi(body).has_value() && read_vi(body).has_value();
                break;
            case 0x03: case 0x21: case 0x23: case 0x25: case 0x26: case 0x27:
            case 0x28: case 0x29: case 0x34: {
                const auto length = read_vi(body);
                ok = length && *length <= 65535 && read_n(body, static_cast<std::size_t>(*length));
                break;
            }
            default:
                // The value encoding of an unknown type cannot be skipped.
                walk.unknown = true;
                return walk;
        }
        if (!ok) { walk.structure = false; return walk; }
        ++walk.parsed;
    }
    return walk;
}

std::optional<Walk> walk_frame(const Frame& frame_value) {
    wire::Cursor body(frame_value.body);
    if (frame_value.type == kSubscribeOk) {
        if (!read_vi(body)) return std::nullopt;
    } else if (frame_value.type == 0x1d) {
        if (!read_vi(body)) return std::nullopt;
        const auto fields = read_vi(body);
        if (!fields || *fields > 32) return std::nullopt;
        for (std::uint64_t index = 0; index < *fields; ++index) {
            const auto field = wire::read_length_prefixed_bytes(body, 4096);
            if (!std::holds_alternative<std::span<const std::byte>>(field)) return std::nullopt;
        }
        const auto name = wire::read_length_prefixed_bytes(body, 4096);
        if (!std::holds_alternative<std::span<const std::byte>>(name)) return std::nullopt;
        if (!read_vi(body)) return std::nullopt;
    } else if (frame_value.type != kPublishStateNotify) {
        return std::nullopt;
    }
    const auto count = read_vi(body);
    if (!count) return std::nullopt;
    return walk_parameters(body, *count);
}

// Publisher-originated messages that carry Message Parameters: the
// SUBSCRIBE_OK and notifications on our SUBSCRIBE stream and any PUBLISH.
std::vector<Walk> publisher_walks(const View& view) {
    std::vector<Walk> walks;
    for (const auto& frame_value : view.write_frames(0)) {
        if (const auto walk = walk_frame(frame_value)) walks.push_back(*walk);
    }
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 0u) continue;
        for (const auto& frame_value : view.frames(record)) {
            if (frame_value.type != 0x1d) continue;
            if (const auto walk = walk_frame(frame_value)) walks.push_back(*walk);
        }
    }
    return walks;
}

Judgement parameter_ready(const View& view, std::optional<bool> result) {
    return {!view.write_frames(0).empty() || view.close().has_value(), result};
}

Spec parameter_spec(const char* scenario, const char* requirement, const char* evaluator, Judge judge) {
    Spec spec;
    spec.scenario = scenario;
    spec.rows = {{requirement, evaluator}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        return definition;
    };
    spec.judge = std::move(judge);
    return spec;
}

// ---- Padding received by the publisher (Section 11.5) -----------------------
Bytes padding_stream() {
    Bytes result;
    put_vi(result, kPaddingStream);
    result.insert(result.end(), kPaddingBytes, std::byte{0});
    return result;
}

Bytes padding_datagram() {
    Bytes result;
    put_vi(result, kPaddingDatagram);
    result.resize(64, std::byte{0});
    return result;
}

Spec padding_spec(const char* scenario, const char* requirement, const char* evaluator, bool datagram) {
    Spec spec;
    spec.scenario = scenario;
    spec.rows = {{requirement, evaluator}};
    spec.build = [datagram](const Fixture& fixture) {
        auto definition = base_definition("");
        if (datagram) definition.writes.push_back({RawProbeChannel::Datagram, padding_datagram(), false});
        else definition.writes.push_back({RawProbeChannel::NewUni, padding_stream(), true});
        // Application behavior is shown by an ordinary request afterward.
        definition.writes.push_back(request_write(subscribe_frame(1, fixture)));
        return definition;
    };
    spec.judge = [](const View& view) { return liveness(view, 1, std::nullopt); };
    return spec;
}

}  // namespace

Bytes session_far_start_filter_for_test() { return encode_params({far_start_filter()}); }

std::vector<Spec> session_specs() {
    std::vector<Spec> result;
    result.push_back(register_spec("d21-setup-register-exceeds-token-cache", true));
    result.push_back(register_spec("d21-setup-register-default-zero-cache", false));
    result.push_back(grease_setup_spec());
    result.push_back(grease_request_error_spec());

    result.push_back(update_spec("d21-single-request-update-response", 1, false,
        {{"D21-9-5-MUST-345", "d21-request-update-response-exclusivity"}}));
    result.push_back(update_spec("d21-coalesced-successful-update-responses", 3, false,
        {{"D21-9-5-MUST-345", "d21-request-update-response-exclusivity"},
         {"D21-9-5-1-MUST-352", "d21-successful-updates-each-acknowledged"}}));
    result.push_back(update_spec("d21-coalesced-failed-update-response", 3, true,
        {{"D21-9-5-MUST-345", "d21-request-update-response-exclusivity"}}));
    for (auto& spec : credit_specs()) result.push_back(std::move(spec));

    result.push_back(successful_subscribe_spec());
    result.push_back(fetch_range_spec("d21-fetch-start-beyond-largest-object", "D21-9-11-MUST-376",
                                      "d21-fetch-start-invalid-range", true, false));
    result.push_back(fetch_range_spec("d21-fetch-track-with-no-published-objects", "D21-9-11-MUST-375",
                                      "d21-empty-track-fetch-invalid-range", false, true));
    result.push_back(namespace_order_spec());

    result.push_back(parameter_spec("d21-publisher-parameter-serialization", "D21-9-20-MUST-395",
        "d21-parameter-type-order", [](const View& view) -> Judgement {
            const auto walks = publisher_walks(view);
            for (const auto& walk : walks)
                if (walk.overflow) return {true, false};
            // One parameter cannot distinguish delta from absolute encoding.
            for (const auto& walk : walks)
                if (walk.parsed >= 2 && walk.structure && !walk.unknown && walk.parsed == walk.declared)
                    return parameter_ready(view, true);
            return parameter_ready(view, std::nullopt);
        }));
    result.push_back(parameter_spec("d21-publisher-parameter-negotiation", "D21-9-20-MUST-397",
        "d21-sent-parameters-defined-or-negotiated", [](const View& view) -> Judgement {
            const auto walks = publisher_walks(view);
            for (const auto& walk : walks)
                if (walk.unknown) return {true, false};
            for (const auto& walk : walks)
                if (!walk.structure || walk.overflow) return parameter_ready(view, std::nullopt);
            if (walks.empty()) return parameter_ready(view, std::nullopt);
            return parameter_ready(view, true);
        }));
    result.push_back(parameter_spec("d21-publisher-parameter-multiplicity", "D21-9-20-MUST-NOT-399",
        "d21-sent-parameter-duplicates-permitted-only", [](const View& view) -> Judgement {
            const auto walks = publisher_walks(view);
            for (const auto& walk : walks)
                if (walk.forbidden_repeat) return {true, false};
            for (const auto& walk : walks)
                if (!walk.structure || walk.overflow || walk.unknown)
                    return parameter_ready(view, std::nullopt);
            if (walks.empty()) return parameter_ready(view, std::nullopt);
            return parameter_ready(view, true);
        }));

    result.push_back(padding_spec("d21-inbound-padding-stream", "D21-11-5-1-MUST-566",
        "d21-padding-stream-drained-without-application-effects", false));
    result.push_back(padding_spec("d21-inbound-padding-datagram", "D21-11-5-2-MUST-571",
        "d21-padding-datagram-no-application-effects", true));
    return result;
}

}  // namespace moq::interop::scenarios::d21c
