#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/location_filter.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

void integer(Bytes& output, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64 capacity");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}

Bytes frame(std::uint64_t type, const Bytes& body) {
    if (body.size() > 65535) throw std::logic_error("probe frame exceeds uint16");
    Bytes result;
    integer(result, type);
    result.push_back(static_cast<std::byte>(body.size() >> 8u));
    result.push_back(static_cast<std::byte>(body.size() & 255u));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<wire::draft21::SetupMessage>(
        wire::draft21::decode_setup(cursor));
}

Bytes subscribe(const Bytes& parameters, unsigned parameter_count = 1,
                unsigned request_id = 1) {
    // Section 9.6 Figure 10: Request ID, zero namespace fields, name x,
    // parameter count. The raw parameters deliberately bypass valid encoders.
    auto body = bytes({request_id, 0, 1, 'x', parameter_count});
    body.insert(body.end(), parameters.begin(), parameters.end());
    return frame(3, body);
}

bool successful_response_ready(std::span<const std::byte> input,
                               wire::draft21::ResponseContext context) {
    wire::Cursor cursor(input);
    return std::holds_alternative<wire::draft21::SuccessfulResponse>(
               wire::draft21::decode_successful_response(cursor, context)) &&
           cursor.remaining() == 0;
}

struct FetchTrack {
    std::vector<Bytes> track_namespace;
    Bytes track_name;
};

bool valid_track(const FetchTrack& track) {
    return fetch_first_object_fixture_valid(track.track_namespace, track.track_name);
}

Bytes track_request(const FetchTrack& track, std::uint64_t type) {
    auto body = bytes({1});
    integer(body, track.track_namespace.size());
    for (const auto& field : track.track_namespace) {
        integer(body, field.size());
        body.insert(body.end(), field.begin(), field.end());
    }
    integer(body, track.track_name.size());
    body.insert(body.end(), track.track_name.begin(), track.track_name.end());
    integer(body, 0);
    return frame(type, body);
}

Bytes fetch(const FetchTrack& track) { return track_request(track, 0x16); }

std::optional<FetchTrack> request_track(std::span<const std::byte> input,
                                      wire::draft21::MessageKind kind) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_request_frame(cursor, true);
    const auto* request = std::get_if<wire::draft21::RequestFrame>(&decoded);
    if (!request || request->type.kind != kind ||
        cursor.remaining() != 0) return std::nullopt;
    wire::Cursor body(request->body);
    const auto id = wire::read_vi64(body);
    const auto count = wire::read_vi64(body);
    const auto* request_id = std::get_if<std::uint64_t>(&id);
    const auto* fields = std::get_if<std::uint64_t>(&count);
    if (!request_id || *request_id != 1 || !fields || *fields > 32)
        return std::nullopt;
    FetchTrack track;
    for (std::uint64_t index = 0; index < *fields; ++index) {
        const auto decoded_field = wire::read_length_prefixed_bytes(body, 4096);
        const auto* field = std::get_if<std::span<const std::byte>>(&decoded_field);
        if (!field || field->empty()) return std::nullopt;
        track.track_namespace.emplace_back(field->begin(), field->end());
    }
    const auto decoded_name = wire::read_length_prefixed_bytes(body, 4096);
    const auto* name = std::get_if<std::span<const std::byte>>(&decoded_name);
    if (!name) return std::nullopt;
    track.track_name.assign(name->begin(), name->end());
    const auto decoded_parameters = wire::read_vi64(body);
    const auto* parameters = std::get_if<std::uint64_t>(&decoded_parameters);
    if (!parameters || *parameters != 0 || body.remaining() != 0 || !valid_track(track))
        return std::nullopt;
    return track;
}

std::optional<FetchTrack> publish_track(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_publish(cursor);
    const auto* publish = std::get_if<wire::draft21::PublishMessage>(&decoded);
    if (!publish || publish->request_id != 0 || cursor.remaining() != 0 ||
        wire::draft21::validate_track_properties(publish->track_properties)) return std::nullopt;
    for (const auto& parameter : publish->parameters) {
        if (parameter.type == 0x21 &&
            !std::holds_alternative<wire::draft21::LocationFilter>(
                wire::draft21::decode_location_filter(std::get<Bytes>(parameter.value))))
            return std::nullopt;
        // This SETUP offers no token cache for an alias or REGISTER.
        if (parameter.type == 3 &&
            std::get<wire::draft21::Token>(parameter.value).alias_type !=
                wire::draft21::TokenAliasType::UseValue) return std::nullopt;
    }
    FetchTrack track{publish->track_namespace, publish->track_name};
    return valid_track(track) ? std::optional{std::move(track)} : std::nullopt;
}

bool same_track(const FetchTrack& left, const FetchTrack& right) {
    return left.track_namespace == right.track_namespace && left.track_name == right.track_name;
}

std::optional<FetchTrack> accepted_publish_track(const RawProbeGateInput& input) {
    if (input.prior_writes.size() != 1) return std::nullopt;
    const auto& accepted = input.prior_writes.front();
    if (!accepted.stream_id || (*accepted.stream_id & 3u) != 0 ||
        accepted.write.channel != RawProbeChannel::PeerBidi || accepted.write.fin ||
        accepted.write.operation != RawProbeOperation::Write ||
        accepted.write.bytes != bytes({7, 0, 1, 0}) ||
        accepted.accepted != accepted.write.bytes.size() ||
        !accepted.delivery_event_count || *accepted.delivery_event_count > input.events.size())
        return std::nullopt;
    Bytes opener;
    for (std::size_t index = 0; index < input.events.size(); ++index) {
        const auto& event = input.events[index];
        if (std::holds_alternative<transport::PeerCloseEvent>(event) ||
            std::holds_alternative<transport::LocalCloseEvent>(event)) return std::nullopt;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && data->stream_id == *accepted.stream_id) {
            if (data->fin || index >= *accepted.delivery_event_count ||
                data->data.size() > 65546 - opener.size()) return std::nullopt;
            opener.insert(opener.end(), data->data.begin(), data->data.end());
        }
        if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
            reset && reset->stream_id == *accepted.stream_id) return std::nullopt;
        if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event);
            stop && stop->stream_id == *accepted.stream_id) return std::nullopt;
    }
    return publish_track(opener);
}

std::optional<std::uint64_t> peer_token_cache_capacity(
    std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_setup(cursor);
    const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded);
    if (!setup) return std::nullopt;
    for (const auto& option : setup->options) {
        if (option.type == 4) {
            const auto* capacity = std::get_if<std::uint64_t>(&option.value);
            return capacity ? std::optional{*capacity} : std::nullopt;
        }
    }
    return 0;
}
}  // namespace

std::vector<Draft21CloseProbe> draft21_close_probes(
    std::chrono::milliseconds deadline, std::vector<Bytes> track_namespace,
    Bytes track_name) {
    const FetchTrack track{std::move(track_namespace), std::move(track_name)};
    if (deadline.count() <= 0 || !valid_track(track))
        throw std::invalid_argument("invalid draft21 close probe configuration");
    std::vector<Draft21CloseProbe> result;
    const auto add = [&](const char* requirement, const char* scenario,
                         const char* evaluator, RawProbeChannel channel,
                         Bytes payload,
                         std::optional<std::uint64_t> error = 3) {
        RawProbeDefinition definition{scenario, bytes({0xaf, 0, 0, 0}),
            {{channel, std::move(payload), false}}, true, setup_ready,
            std::chrono::milliseconds{1000}, {}};
        result.push_back({requirement, evaluator, error, std::move(definition)});
    };
    const auto setup = [&](const char* requirement, const char* scenario,
                           const char* evaluator, Bytes body,
                           std::uint64_t error = 3) {
        RawProbeDefinition definition{scenario, frame(0x2f00, body), {},
            false, setup_ready, std::chrono::milliseconds{1000}, {}};
        result.push_back({requirement, evaluator, error, std::move(definition)});
    };
    constexpr auto bidi = RawProbeChannel::NewBidi;
    constexpr auto control = RawProbeChannel::Control;

    // Sections 6.3, 6.4.1, 9 and 11: unknown types and illegal placement.
    add("D21-6-3-MUST-142", "d21-invalid-bidirectional-request-stream-opener",
        "d21-invalid-request-stream-opener-protocol-violation", bidi,
        bytes({7, 0, 1, 0}));
    add("D21-6-4-1-MUST-153", "d21-unknown-unidirectional-stream-type",
        "d21-unknown-stream-type-session-close", RawProbeChannel::NewUni,
        bytes({0x00}), std::nullopt);
    Bytes unknown_datagram;
    integer(unknown_datagram, 0x132b3e2a);
    add("D21-11-MUST-503", "d21-unknown-datagram-type",
        "d21-unknown-datagram-session-close", RawProbeChannel::Datagram,
        std::move(unknown_datagram), std::nullopt);
    add("D21-9-MUST-284", "d21-unknown-control-message",
        "d21-unknown-message-session-close", control, bytes({0x7e, 0, 0}),
        std::nullopt);
    add("D21-9-MUST-285", "d21-message-body-length-mismatch",
        "d21-message-body-length-protocol-violation", control,
        bytes({0x10, 0, 1, 0}));

    // Section 6.4.2.1, using otherwise complete SUBSCRIBE_NAMESPACE frames.
    add("D21-6-4-2-1-MUST-154", "d21-request-id-wrong-sender-parity",
        "d21-wrong-parity-invalid-request-id", bidi,
        bytes({0x50, 0, 3, 0, 0, 0}), 4);
    // The first request has zero Track Namespace fields (Section 4.1: all
    // namespaces), which any publisher can accept; one for an unknown prefix
    // could be refused and the session ended before the duplicate is read.
    add("D21-6-4-2-1-MUST-155", "d21-duplicate-request-id-across-streams",
        "d21-duplicate-invalid-request-id", bidi,
        bytes({0x50, 0, 3, 1, 0, 0}), 4);
    result.back().definition.writes.push_back(
        {bidi, bytes({0x50, 0, 5, 1, 1, 1, 'm', 0}), false});

    // Section 8.3: type overflow, excessive length and malformed known TOKEN.
    Bytes overflow;
    integer(overflow, std::numeric_limits<std::uint64_t>::max());
    overflow.insert(overflow.end(), {std::byte{0}, std::byte{1}, std::byte{0}});
    setup("D21-8-3-MUST-231", "d21-setup-key-value-type-overflow",
          "d21-key-value-type-overflow-protocol-violation", std::move(overflow));
    Bytes length = bytes({9});
    integer(length, 65536);
    setup("D21-8-3-MUST-232", "d21-setup-key-value-declared-length-overflow",
          "d21-key-value-length-overflow-protocol-violation", std::move(length));
    // Section 8.9: Alias Type is vi64 0..3; 4 is an invalid Token encoding.
    setup("D21-8-3-MUST-233", "d21-setup-known-key-value-malformed-value",
          "d21-known-key-value-formatting-error", bytes({3, 1, 4}), 6);

    // Section 8.7 plus Figures 10, 18 and 21.
    add("D21-8-7-MUST-251", "d21-subscribe-empty-namespace-field",
        "d21-empty-namespace-field-protocol-violation", bidi,
        frame(3, bytes({1, 1, 0, 1, 'x', 0})));
    Bytes many = bytes({1, 33});
    for (unsigned index = 0; index < 33; ++index)
        many.insert(many.end(), {std::byte{1}, std::byte{'n'}});
    auto many_subscribe = many;
    many_subscribe.insert(many_subscribe.end(), {std::byte{1}, std::byte{'x'},
                                               std::byte{0}});
    add("D21-8-7-MUST-252", "d21-subscribe-33-namespace-fields",
        "d21-too-many-namespace-fields-protocol-violation", bidi,
        frame(3, many_subscribe));
    many.push_back(std::byte{0});
    add("D21-9-15-MUST-383", "d21-subscribe-namespace-prefix-too-many-fields",
        "d21-namespace-discovery-prefix-bound", bidi, frame(0x50, many));
    add("D21-9-18-MUST-391", "d21-subscribe-tracks-prefix-too-many-fields",
        "d21-track-discovery-prefix-bound", bidi, frame(0x51, many));
    Bytes large_namespace = bytes({1, 1});
    integer(large_namespace, 4097);
    large_namespace.insert(large_namespace.end(), 4097, std::byte{'n'});
    large_namespace.push_back(std::byte{0});
    add("D21-8-7-MUST-253", "d21-subscribe-tracks-oversized-namespace",
        "d21-oversized-namespace-protocol-violation", bidi,
        frame(0x51, large_namespace));
    Bytes large_name = bytes({1, 1});
    integer(large_name, 4096);
    large_name.insert(large_name.end(), 4096, std::byte{'n'});
    large_name.insert(large_name.end(), {std::byte{1}, std::byte{'x'}, std::byte{0}});
    add("D21-8-7-MUST-254", "d21-subscribe-oversized-full-track-name",
        "d21-oversized-full-track-name-protocol-violation", bidi,
        frame(3, large_name));

    // Section 9.2: two complete GOAWAYs with empty URI and long timeout,
    // coalesced so a normal immediate drain cannot separate the stimuli.
    Bytes goaway_body{std::byte{0}};
    integer(goaway_body, 10000);
    auto duplicate_goaway = frame(0x10, goaway_body);
    const auto second_goaway = duplicate_goaway;
    duplicate_goaway.insert(duplicate_goaway.end(), second_goaway.begin(),
                            second_goaway.end());
    add("D21-9-2-MUST-327", "d21-duplicate-control-goaway",
        "d21-duplicate-control-goaway-protocol-violation", control,
        std::move(duplicate_goaway));
    Bytes uri;
    integer(uri, 8193);
    uri.insert(uri.end(), 8193, std::byte{'x'});
    integer(uri, 10000);
    add("D21-9-2-MUST-331", "d21-goaway-uri-length-boundary",
        "d21-goaway-uri-overflow-protocol-violation", control, frame(0x10, uri));

    // Section 9.20 and its parameter-specific subsections. Unknown 0x7e
    // is not offered in SETUP. EXPIRES is forbidden in a SUBSCRIBE.
    Bytes parameter_overflow = bytes({0x02, 0x00});
    integer(parameter_overflow, std::numeric_limits<std::uint64_t>::max());
    add("D21-9-20-MUST-396", "d21-parameter-type-delta-overflow",
        "d21-parameter-type-overflow-protocol-violation", bidi,
        subscribe(parameter_overflow, 2));
    add("D21-8-9-MUST-267", "d21-request-undecodable-authorization-token",
        "d21-token-decode-key-value-formatting-error", bidi,
        subscribe(bytes({0x03, 0x01, 0x03})), 6);
    add("D21-9-20-MUST-398", "d21-unknown-message-parameter",
        "d21-unknown-parameter-protocol-violation", bidi,
        subscribe(bytes({0x7e, 0})));
    add("D21-9-20-SHOULD-401", "d21-unexpected-duplicate-message-parameter",
        "d21-unexpected-duplicate-parameter-close-advisory", bidi,
        subscribe(bytes({0x10, 0, 0, 0}), 2));
    add("D21-9-20-1-MUST-404", "d21-parameter-invalid-message-scope",
        "d21-out-of-scope-parameter-protocol-violation", bidi,
        subscribe(bytes({8, 1})));
    add("D21-9-20-9-MUST-429", "d21-group-order-zero",
        "d21-group-order-bounds-protocol-violation", bidi,
        subscribe(bytes({0x22, 0})));
    Bytes location;
    integer(location, std::numeric_limits<std::uint64_t>::max());
    location.insert(location.end(), {std::byte{0}, std::byte{1}});
    Bytes filter{std::byte{0x21}};
    integer(filter, location.size());
    filter.insert(filter.end(), location.begin(), location.end());
    add("D21-9-20-10-MUST-432", "d21-location-filter-end-group-overflow",
        "d21-location-filter-overflow-protocol-violation", bidi,
        subscribe(filter));
    add("D21-9-20-19-MUST-460", "d21-forward-value-two",
        "d21-forward-bounds-protocol-violation", bidi,
        subscribe(bytes({0x10, 2})));
    add("D21-9-20-22-MUST-475", "d21-include-properties-value-two",
        "d21-include-properties-bounds-protocol-violation", bidi,
        subscribe(bytes({0x35, 2})));
    // Section 9.20.16 Table 6: each nested parameter is well formed,
    // but AUTHORIZATION_TOKEN, TRACK_PROPERTY_FILTER and FILL_PARAMETERS
    // are all forbidden in the length-bounded fill scope.
    add("D21-9-20-16-MUST-447", "d21-fill-forbidden-nested-authorization",
        "d21-fill-parameter-whitelist-protocol-violation", bidi,
        subscribe(bytes({0x23, 4, 3, 2, 3, 0})));
    add("D21-9-20-16-MUST-447", "d21-fill-forbidden-track-property-filter",
        "d21-fill-parameter-whitelist-protocol-violation", bidi,
        subscribe(bytes({0x23, 5, 0x29, 3, 0, 0, 0})));
    add("D21-9-20-16-MUST-447", "d21-fill-recursive-parameter",
        "d21-fill-parameter-whitelist-protocol-violation", bidi,
        subscribe(bytes({0x23, 2, 0x23, 0})));
    // Sections 9.5 and 9.13: TRACK_STATUS cannot receive REQUEST_UPDATE,
    // even from its original sender with a fresh, correctly odd Request ID.
    auto status_update = frame(0x0d, bytes({1, 0, 1, 'x', 0}));
    const auto update = frame(2, bytes({3, 0}));
    status_update.insert(status_update.end(), update.begin(), update.end());
    add("D21-9-5-MUST-344", "d21-update-on-track-status",
        "d21-request-update-context-and-direction", bidi,
        std::move(status_update));
    // Sections 8.9 and 9.1.3: reserve enough peer cache space for two
    // hypothetical 16-byte entries so a duplicate detector's absence
    // cannot produce an independent cache-overflow close.
    const auto registered = bytes({3, 3, 1, 0, 0});
    add("D21-8-9-MUST-268", "d21-token-duplicate-registration",
        "d21-duplicate-token-alias-session-error", bidi,
        subscribe(registered), 0x14);
    result.back().definition.writes.push_back(
        {bidi, subscribe(registered, 1, 3), false});
    result.back().definition.peer_setup_ready = [](auto input) {
        const auto capacity = peer_token_cache_capacity(input);
        return capacity && *capacity >= 32;
    };
    // A one-byte Token Value costs 17 bytes; larger advertised caches
    // do not establish this bounded overflow probe's precondition.
    add("D21-8-9-MUST-277", "d21-request-token-cache-overflow",
        "d21-token-cache-overflow-session-error", bidi,
        subscribe(bytes({3, 4, 1, 0, 0, 'x'})), 0x13);
    result.back().definition.peer_setup_ready = [](auto input) {
        const auto capacity = peer_token_cache_capacity(input);
        return capacity && *capacity < 17;
    };

    // Section 9: a complete valid opener precedes the unknown frame on
    // the same request stream, isolating unknown-message handling.
    auto unknown_request = subscribe({}, 0);
    const auto unknown_frame = frame(0x7e, {});
    unknown_request.insert(unknown_request.end(), unknown_frame.begin(),
                           unknown_frame.end());
    add("D21-9-MUST-284", "d21-unknown-request-stream-message",
        "d21-unknown-message-session-close", bidi,
        std::move(unknown_request), std::nullopt);
    // A missing FORWARD value is conclusive only when the request FIN
    // ends the stream. Keep its declared body length unchanged.
    auto truncated = subscribe(bytes({0x10, 0}));
    truncated.pop_back();
    add("D21-9-MUST-285", "d21-request-message-truncated-at-fin",
        "d21-message-body-length-protocol-violation", bidi,
        std::move(truncated));
    result.back().definition.writes.front().fin = true;

    add("D21-9-20-9-MUST-429", "d21-group-order-above-two",
        "d21-group-order-bounds-protocol-violation", bidi,
        subscribe(bytes({0x22, 3})));
    add("D21-9-20-9-MUST-429", "d21-fill-invalid-group-order",
        "d21-group-order-bounds-protocol-violation", bidi,
        subscribe(bytes({0x23, 2, 0x22, 3})));
    Bytes fill_location{std::byte{0x23}};
    integer(fill_location, filter.size());
    fill_location.insert(fill_location.end(), filter.begin(), filter.end());
    add("D21-9-20-10-MUST-432", "d21-fill-location-filter-end-group-overflow",
        "d21-location-filter-overflow-protocol-violation", bidi,
        subscribe(fill_location));
    add("D21-9-20-19-MUST-460", "d21-forward-value-255",
        "d21-forward-bounds-protocol-violation", bidi,
        subscribe(bytes({0x10, 255})));
    add("D21-9-20-22-MUST-475", "d21-include-properties-value-255",
        "d21-include-properties-bounds-protocol-violation", bidi,
        subscribe(bytes({0x35, 255})));
    // Section 9.1.3: omitted MAX_AUTH_TOKEN_CACHE_SIZE means zero.
    // The GREASE Token Type avoids relying on an application token
    // format; even an empty Token Value incurs a 16-byte cache charge.
    add("D21-8-9-MUST-277", "d21-request-alias-registration-with-default-zero-cache",
        "d21-token-cache-overflow-session-error", bidi,
        subscribe(bytes({3, 4, 1, 0, 0x80, 0x9d})), 0x13);
    result.back().definition.peer_setup_ready = [](auto input) {
        wire::Cursor cursor(input);
        const auto decoded = wire::draft21::decode_setup(cursor);
        const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded);
        if (!setup) return false;
        for (const auto& option : setup->options)
            if (option.type == 4) return false;
        return true;
    };
    // Section 9.20.6 permits FILL_TIMEOUT only in FETCH or nested fill,
    // so an otherwise complete SUBSCRIBE isolates its illegal scope.
    add("D21-9-20-1-MUST-404", "d21-fill-timeout-outside-fill-or-fetch",
        "d21-out-of-scope-parameter-protocol-violation", bidi,
        subscribe(bytes({0x0a, 0})));
    // Establish each request before updating it. GROUP_ORDER1 has a legal
    // value but is outside REQUEST_UPDATE's scope; FORWARD255 is in scope
    // for track discovery but outside its permitted range.
    add("D21-9-20-1-MUST-404", "d21-group-order-in-subscription-update",
        "d21-out-of-scope-parameter-protocol-violation", bidi,
        track_request(track, 3));
    result.back().definition.writes.push_back(
        {bidi, frame(2, bytes({3, 1, 0x22, 1})), false, 0,
         [](auto input) { return successful_response_ready(
             input, wire::draft21::ResponseContext::Subscribe); }});
    // Section 9.18.1 carries SUBSCRIBE parameters into SUBSCRIBE_TRACKS and
    // Section 9.20.19 allows FORWARD 0 there. The initial Forward State 0
    // keeps a publisher from pushing objects for the PUBLISH messages this
    // probe never answers, so it still reads the follow-up REQUEST_UPDATE.
    add("D21-9-20-19-MUST-460", "d21-discovery-update-invalid-forward",
        "d21-forward-bounds-protocol-violation", bidi,
        frame(0x51, bytes({1, 0, 1, 0x10, 0})));
    result.back().definition.writes.push_back(
        {bidi, frame(2, bytes({3, 1, 0x10, 255})), false, 0,
         [](auto input) { return successful_response_ready(
             input, wire::draft21::ResponseContext::SubscribeTracks); }});
    add("D21-6-4-2-1-MUST-155", "d21-duplicate-request-update-id",
        "d21-duplicate-invalid-request-id", bidi,
        track_request(track, 3), 4);
    result.back().definition.writes.push_back(
        {bidi, frame(2, bytes({3, 0})), false, 0,
         [](auto input) { return successful_response_ready(
             input, wire::draft21::ResponseContext::Subscribe); }});
    // A successful first UPDATE releases its outstanding update credit
    // before the duplicate, including when MAX_REQUEST_UPDATES is one.
    result.back().definition.writes.push_back(
        {bidi, frame(2, bytes({3, 0})), false, 1,
         [](auto input) { return successful_response_ready(
             input, wire::draft21::ResponseContext::RequestUpdate); }});

    // Section 9.10 Figure 14 has no Request ID. An empty parameter list
    // isolates the request type from parameter scope or direction changes.
    add("D21-9-10-MUST-369", "d21-publish-state-notify-on-namespace-request",
        "d21-publish-state-notify-request-type-error", bidi,
        frame(0x50, bytes({1, 0, 0})));
    result.back().definition.writes.push_back(
        {bidi, frame(0x22, bytes({0})), false, 0,
         [](auto input) { return successful_response_ready(
             input, wire::draft21::ResponseContext::SubscribeNamespace); }});
    add("D21-9-10-MUST-369", "d21-publish-state-notify-on-fetch",
        "d21-publish-state-notify-request-type-error", bidi, fetch(track));
    result.back().definition.writes.push_back(
        {bidi, frame(0x22, bytes({0})), false, 0,
         [](auto input) { return successful_response_ready(
             input, wire::draft21::ResponseContext::Fetch); }});

    // Section 9.10: subscriber direction is forbidden for both ways of
    // establishing a subscription, regardless of who opened the stream.
    add("D21-9-10-MUST-370", "d21-subscriber-sends-publish-state-notify",
        "d21-publish-state-notify-direction-error", bidi,
        track_request(track, 3));
    result.back().definition.writes.push_back(
        {bidi, frame(0x22, bytes({0})), false, 0,
         [](auto input) { return successful_response_ready(
             input, wire::draft21::ResponseContext::Subscribe); }});
    add("D21-9-10-MUST-370", "d21-publish-established-subscriber-sends-publish-state-notify",
        "d21-publish-state-notify-direction-error", RawProbeChannel::PeerBidi,
        frame(7, bytes({0})));
    result.back().definition.peer_request_ready = [track](auto input) {
        const auto actual = publish_track(input);
        return actual && same_track(*actual, track);
    };
    RawProbeWrite notify{RawProbeChannel::PeerBidi, frame(0x22, bytes({0})), false, 0};
    notify.evidence_ready = [track](const auto& input) {
        const auto actual = accepted_publish_track(input);
        return actual && same_track(*actual, track);
    };
    result.back().definition.writes.push_back(std::move(notify));

    for (auto& probe : result) probe.definition.deadline = deadline;

    return result;
}

std::optional<bool> evaluate_draft21_close_probe(
    const RawProbeTranscript& transcript, const Draft21CloseProbe& probe) {
    const bool fetch_context = probe.definition.id == "d21-publish-state-notify-on-fetch";
    // These contexts open with a SUBSCRIBE for the run's configured track.
    const bool subscribe_context = probe.definition.id == "d21-subscriber-sends-publish-state-notify" ||
        probe.definition.id == "d21-group-order-in-subscription-update" ||
        probe.definition.id == "d21-duplicate-request-update-id";
    const bool publish_context = probe.definition.id ==
        "d21-publish-established-subscriber-sends-publish-state-notify";
    if (!fetch_context && !subscribe_context && !publish_context)
        return evaluate_raw_probe_close(transcript, probe.definition, probe.expected_close);
    if (transcript.writes.empty()) return std::nullopt;
    std::optional<FetchTrack> track;
    if (publish_context) {
        const auto marker = transcript.writes.front().delivery_event_count;
        if (!marker || *marker > transcript.events.size()) return std::nullopt;
        track = accepted_publish_track({std::span(transcript.writes).first(1),
                                       std::span(transcript.events).first(*marker)});
    } else {
        track = request_track(transcript.writes.front().write.bytes,
            fetch_context ? wire::draft21::MessageKind::Fetch : wire::draft21::MessageKind::Subscribe);
    }
    if (!track) return std::nullopt;
    // Recover only the target from actual request bytes; every other byte,
    // gate, stream, acceptance boundary and close is proved canonically.
    const auto probes = draft21_close_probes(probe.definition.deadline,
                                            track->track_namespace, track->track_name);
    const auto configured = std::find_if(probes.begin(), probes.end(), [&](const auto& candidate) {
        return candidate.definition.id == probe.definition.id;
    });
    return evaluate_raw_probe_close(transcript, configured->definition, configured->expected_close);
}
}  // namespace moq::interop::scenarios
