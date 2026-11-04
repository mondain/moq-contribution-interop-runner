#include "moq/interop/session/draft23_probes.h"

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft23/messages.h"

#include <array>

namespace moq::interop::session {
namespace {

using Bytes = std::vector<std::byte>;
using Channel = Draft23ProbeChannel;
using Expect = Draft23ProbeExpect;

// Session close codes (Section 12.2) and REQUEST_ERROR codes (Section 12.3).
constexpr std::uint64_t kProtocolViolation = 0x3;
constexpr std::uint64_t kInvalidRequestId = 0x4;
constexpr std::uint64_t kMalformedKeyValue = 0x6;
constexpr std::uint64_t kInvalidPath = 0x8;
constexpr std::uint64_t kInvalidAuthority = 0x19;
constexpr std::uint64_t kInvalidStreamHeader = 0x200;
constexpr std::uint64_t kMalformedObject = 0x201;
constexpr std::uint64_t kInvalidObjectStatus = 0x202;
constexpr std::uint64_t kMalformedMessage = 0x300;
constexpr std::uint64_t kInvalidField = 0x301;
constexpr std::uint64_t kInvalidTrackNamespace = 0x302;
constexpr std::uint64_t kUnexpectedMessage = 0x303;
constexpr std::uint64_t kRoleViolation = 0x304;
constexpr std::uint64_t kInvalidParameter = 0x305;
constexpr std::uint64_t kClosedControlStream = 0x306;
constexpr std::uint64_t kDeltaEncodingOverflow = 0x400;
constexpr std::uint64_t kFieldLengthExceeded = 0x401;
constexpr std::uint64_t kDoesNotExist = 0x10;
constexpr std::uint64_t kRedirect = 0x34;

constexpr std::uint64_t kInvalidFilter = 0x36;

constexpr std::array<Draft23Probe, 52> kProbes{{
    {"d23-probe-unknown-control-message", "D23-9-MUST-257", {}, "d23-close-on-unknown-message-type", Channel::Bidi, Expect::Close, kUnexpectedMessage},
    {"d23-probe-bad-first-bidi-message", "D23-6-3-MUST-137", {}, "d23-close-on-bad-first-bidi-message", Channel::Bidi, Expect::Close, kUnexpectedMessage},
    {"d23-probe-length-mismatch", "D23-9-MUST-258", {}, "d23-close-on-length-mismatch", Channel::Bidi, Expect::Close, kMalformedMessage},
    {"d23-probe-oversize-control-message", "D23-9-MUST-260", {}, "d23-close-on-oversize-control-message", Channel::Bidi, Expect::Close, kFieldLengthExceeded},
    {"d23-probe-server-authority", "D23-9-1-1-MUST-268", {}, "d23-close-on-server-authority", Channel::SetupOption, Expect::Close, kInvalidAuthority},
    {"d23-probe-server-path", "D23-9-1-2-MUST-272", {}, "d23-close-on-server-path", Channel::SetupOption, Expect::Close, kInvalidPath},
    {"d23-probe-unknown-uni-stream", "D23-6-4-1-MUST-146", {}, "d23-close-on-unknown-uni-stream", Channel::Uni, Expect::Close, kInvalidStreamHeader},
    {"d23-probe-subscribe-reserved-namespace", "D23-2-4-3-MUST-034", {}, "d23-reject-reserved-namespace", Channel::Bidi, Expect::RequestError, kDoesNotExist},
    {"d23-probe-subscribe-session-namespace", "D23-6-5-MUST-160", {}, "d23-reject-session-namespace", Channel::Bidi, Expect::RequestError, kDoesNotExist},
    {"d23-probe-empty-track-name", "D23-2-4-1-MUST-029", {}, "d23-close-on-empty-track-name", Channel::Bidi, Expect::Close, kProtocolViolation},
    {"d23-probe-control-stream-closed", "D23-6-3-MUST-141", {}, "d23-close-on-control-stream-closed", Channel::ControlFin, Expect::Close, kClosedControlStream},
    {"d23-probe-wrong-request-id-parity", "D23-6-4-2-1-MUST-148", {}, "d23-close-on-wrong-request-id-parity", Channel::Bidi, Expect::Close, kInvalidRequestId},
    {"d23-probe-duplicate-request-id", "D23-6-4-2-1-MUST-149", {}, "d23-close-on-duplicate-request-id", Channel::Bidi, Expect::Close, kInvalidRequestId},
    {"d23-probe-setup-delta-overflow", "D23-8-3-MUST-211", {}, "d23-close-on-setup-delta-overflow", Channel::SetupBytes, Expect::Close, kDeltaEncodingOverflow},
    {"d23-probe-setup-malformed-value", "D23-8-3-MUST-213", {}, "d23-close-on-malformed-key-value", Channel::SetupBytes, Expect::Close, kMalformedKeyValue},
    {"d23-probe-reason-phrase-too-long", "D23-8-5-MUST-230", {}, "d23-close-on-long-reason-phrase", Channel::AnnounceReply, Expect::Close, kFieldLengthExceeded},
    {"d23-probe-namespace-empty-field", "D23-8-7-MUST-233", {}, "d23-close-on-empty-namespace-field", Channel::Bidi, Expect::Close, kInvalidTrackNamespace},
    {"d23-probe-namespace-too-many-fields", "D23-8-7-MUST-234", {}, "d23-close-on-too-many-namespace-fields", Channel::Bidi, Expect::Close, kInvalidTrackNamespace},
    {"d23-probe-namespace-too-long", "D23-8-7-MUST-235", {}, "d23-close-on-long-namespace", Channel::Bidi, Expect::Close, kFieldLengthExceeded},
    {"d23-probe-unknown-setup-option", "D23-9-1-MUST-263", {"D23-9-1-MUST-264", "D23-13-MUST-500", "D23-16-4-MUST-523"}, "d23-tolerate-unknown-setup-option", Channel::SetupOption, Expect::Tolerate, 0},
    {"d23-probe-duplicate-unknown-setup-option", "D23-9-1-MUST-266", {}, "d23-tolerate-duplicate-unknown-setup-option", Channel::SetupOption, Expect::Tolerate, 0},
    {"d23-probe-request-ok-with-properties", "D23-9-3-MUST-306", {}, "d23-close-on-request-ok-properties", Channel::AnnounceReply, Expect::Close, kInvalidField},
    {"d23-probe-unknown-error-detail", "D23-9-4-2-MUST-309", {"D23-9-4-2-MUST-310", "D23-9-4-2-MUST-312", "D23-13-MUST-502"}, "d23-tolerate-unknown-error-detail", Channel::PublishReply, Expect::Tolerate, 0},
    {"d23-probe-redirect-without-detail", "D23-9-4-2-1-MUST-314", {}, "d23-close-on-redirect-without-detail", Channel::AnnounceReply, Expect::Close, kMalformedMessage},
    {"d23-probe-redirect-track-name-in-namespace-request", "D23-9-4-2-1-MUST-317", {"D23-9-4-2-1-MUST-318", ""}, "d23-close-on-redirect-track-name", Channel::AnnounceReply, Expect::Close, kInvalidField},
    {"d23-probe-unknown-request-error-code", "D23-13-MUST-503", {"D23-13-MUST-NOT-499", "D23-13-MUST-NOT-504"}, "d23-tolerate-unknown-error-code", Channel::PublishReply, Expect::Tolerate, 0},
    {"d23-probe-publish-state-notify-on-announce", "D23-9-10-MUST-345", {}, "d23-close-on-state-notify-off-subscription", Channel::AnnounceExtra, Expect::Close, kUnexpectedMessage},
    {"d23-probe-publish-state-notify-to-publisher", "D23-9-10-MUST-346", {}, "d23-close-on-state-notify-to-publisher", Channel::SubscribeExtra, Expect::Close, kRoleViolation},
    {"d23-probe-unknown-message-parameter", "D23-9-19-MUST-353", {"D23-9-19-MUST-354", ""}, "d23-close-on-unknown-parameter", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-parameter-out-of-scope", "D23-9-19-1-MUST-358", {}, "d23-close-on-out-of-scope-parameter", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-group-order-out-of-range", "D23-9-19-8-MUST-377", {}, "d23-close-on-group-order-out-of-range", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-paused-out-of-range", "D23-9-19-18-MUST-396", {}, "d23-close-on-paused-out-of-range", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-include-properties-out-of-range", "D23-9-19-22-MUST-407", {}, "d23-close-on-include-properties-out-of-range", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-subgroup-header-reserved-mode", "D23-11-3-1-MUST-444", {}, "d23-close-on-reserved-subgroup-mode", Channel::Uni, Expect::Close, kInvalidStreamHeader},
    {"d23-probe-datagram-unknown-type", "D23-11-MUST-430", {}, "d23-close-on-unknown-datagram-type", Channel::Datagram, Expect::Close, kInvalidStreamHeader},
    {"d23-probe-datagram-reserved-bit", "D23-11-2-1-MUST-437", {}, "d23-close-on-datagram-reserved-bit", Channel::Datagram, Expect::Close, kInvalidStreamHeader},
    {"d23-probe-datagram-status-end-of-group", "D23-11-2-1-MUST-439", {}, "d23-close-on-datagram-status-end-of-group", Channel::Datagram, Expect::Close, kInvalidStreamHeader},
    {"d23-probe-datagram-properties-length-zero", "D23-11-2-1-MUST-438", {}, "d23-close-on-datagram-empty-properties", Channel::Datagram, Expect::Close, kMalformedObject},
    {"d23-probe-filter-ranges-over-limit", "D23-3-3-2-MUST-076", {"D23-9-1-6-MUST-285", ""}, "d23-reject-filter-ranges-over-limit", Channel::Bidi, Expect::RequestError, kInvalidFilter},
    {"d23-probe-location-filter-unknown-type", "D23-9-19-9-MUST-379", {}, "d23-close-on-unknown-location-filter-type", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-location-filter-overflow", "D23-9-19-9-MUST-380", {}, "d23-close-on-location-filter-overflow", Channel::Bidi, Expect::Close, kDeltaEncodingOverflow},
    {"d23-probe-fill-parameters-invalid", "D23-9-19-15-MUST-388", {}, "d23-close-on-invalid-fill-parameter", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-padding-stream", "D23-11-5-1-MUST-476", {}, "d23-tolerate-padding-stream", Channel::Uni, Expect::Tolerate, 0},
    {"d23-probe-padding-datagram", "D23-11-5-2-MUST-480", {}, "d23-tolerate-padding-datagram", Channel::Datagram, Expect::Tolerate, 0},
    {"d23-probe-goaway-twice", "D23-9-2-MUST-298", {}, "d23-close-on-second-goaway", Channel::ControlBytes, Expect::Close, kUnexpectedMessage},
    {"d23-probe-goaway-uri-too-long", "D23-9-2-MUST-301", {}, "d23-close-on-long-goaway-uri", Channel::ControlBytes, Expect::Close, kFieldLengthExceeded},
    {"d23-probe-goaway-on-subscribe-stream", "D23-9-2-MUST-291", {}, "d23-close-on-goaway-from-requester", Channel::SubscribeExtra, Expect::Close, kProtocolViolation},
    {"d23-probe-request-update-wrong-direction", "D23-9-5-MUST-321", {}, "d23-close-on-misdirected-request-update", Channel::AnnounceExtra, Expect::Close, kUnexpectedMessage},
    {"d23-probe-second-announce-response", "D23-4-1-SHOULD-098", {}, "d23-close-on-second-announce-response", Channel::AnnounceExtra, Expect::Close, kUnexpectedMessage},
    {"d23-probe-second-publish-response", "D23-3-1-SHOULD-038", {}, "d23-close-on-second-publish-response", Channel::PublishExtra, Expect::Close, kUnexpectedMessage},
    {"d23-probe-duplicate-parameter", "D23-9-19-SHOULD-356", {}, "d23-close-on-duplicate-parameter", Channel::Bidi, Expect::Close, kInvalidParameter},
    {"d23-probe-datagram-status-properties-not-normal", "D23-11-2-1-MUST-441", {}, "d23-close-on-datagram-properties-on-status", Channel::Datagram, Expect::Close, kInvalidObjectStatus},
}};

// ---- byte builders ------------------------------------------------------------------------------------------------

Bytes vi(std::uint64_t value) {
    wire::ByteWriter writer(16);
    (void)wire::write_vi64(value, writer);
    return {writer.bytes().begin(), writer.bytes().end()};
}
Bytes raw(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
Bytes text(std::string_view value) {
    const auto view = std::as_bytes(std::span(value));
    return {view.begin(), view.end()};
}
Bytes repeated(char c, std::size_t count) { return Bytes(count, static_cast<std::byte>(c)); }
Bytes join(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}
Bytes frame(std::uint64_t type, const Bytes& body) { return join({vi(type), vi(body.size()), body}); }
Bytes field(const Bytes& value) { return join({vi(value.size()), value}); }

Bytes names(const std::vector<Bytes>& fields) {
    Bytes result = vi(fields.size());
    for (const auto& value : fields) { const auto encoded = field(value); result.insert(result.end(), encoded.begin(), encoded.end()); }
    return result;
}
// SUBSCRIBE: Request ID, namespace, track name, then `parameters` exactly as given (count first).
Bytes subscribe(std::uint64_t request_id, const std::vector<Bytes>& fields, const Bytes& name, const Bytes& parameters) {
    return frame(3, join({vi(request_id), names(fields), field(name), parameters}));
}
Bytes no_parameters() { return vi(0); }
Bytes one_parameter(std::uint64_t type, const Bytes& encoded_value) { return join({vi(1), vi(type), encoded_value}); }

// REQUEST_ERROR: code, retry interval, reason phrase, then the Error Details exactly as given.
Bytes request_error(std::uint64_t code, const Bytes& reason, const Bytes& details) {
    return frame(5, join({vi(code), vi(0), field(reason), details}));
}

const Draft23Probe* by_id(std::string_view id) { return find_draft23_probe(id); }

// The REDIRECT Error Detail (type 1): Connect URI, Track Namespace, Track Name (Section 9.4.2.1).
Bytes redirect_detail(const Bytes& track_name) {
    return join({vi(1), field(join({field({}), names({text("media")}), field(track_name)}))});
}

}  // namespace

std::span<const Draft23Probe> draft23_probes() { return kProbes; }

const Draft23Probe* find_draft23_probe(std::string_view scenario_id) {
    for (const auto& probe : kProbes) if (probe.scenario_id == scenario_id) return &probe;
    return nullptr;
}

std::vector<std::string_view> draft23_probe_rows(const Draft23Probe& probe) {
    std::vector<std::string_view> rows{probe.requirement_id};
    for (const auto extra : probe.more_requirement_ids) if (!extra.empty()) rows.push_back(extra);
    return rows;
}

std::vector<std::byte> draft23_hand_built_subscribe(std::uint64_t request_id, const std::vector<std::string>& namespace_fields,
                                                    const std::string& track_name) {
    std::vector<Bytes> fields;
    for (const auto& value : namespace_fields) fields.push_back(text(value));
    return subscribe(request_id, fields, text(track_name), no_parameters());
}

std::vector<std::vector<std::byte>> draft23_probe_payloads(const Draft23Probe& probe, std::uint64_t peer_message_limit) {
    Draft23PeerLimits limits;
    limits.control_message_size = peer_message_limit;
    return draft23_probe_payloads(probe, limits);
}

std::vector<std::vector<std::byte>> draft23_probe_payloads(const Draft23Probe& probe, const Draft23PeerLimits& limits) {
    const auto id = probe.scenario_id;
    const auto peer_message_limit = limits.control_message_size;
    const std::vector<Bytes> media = limits.track_namespace.empty() ? std::vector<Bytes>{text("media")} : limits.track_namespace;
    const Bytes track = limits.track_name.empty() ? text("track") : limits.track_name;
    // Odd Request IDs belong to the server, which the runner is.
    if (id == "d23-probe-unknown-control-message") return {raw({0x3f, 0})};
    if (id == "d23-probe-bad-first-bidi-message") return {raw({4, 2, 7, 0})};
    if (id == "d23-probe-length-mismatch") return {raw({6, 1, 0})};
    if (id == "d23-probe-unknown-uni-stream") return {raw({0x0a})};
    if (id == "d23-probe-oversize-control-message") return {join({vi(6), vi(peer_message_limit + 1)})};
    if (id == "d23-probe-subscribe-reserved-namespace") return {subscribe(1, {text("."), text("probe")}, track, no_parameters())};
    if (id == "d23-probe-subscribe-session-namespace") return {subscribe(1, {text(".session"), text("probe")}, track, no_parameters())};
    if (id == "d23-probe-empty-track-name") return {subscribe(1, {text("a")}, {}, no_parameters())};
    if (id == "d23-probe-control-stream-closed") return {Bytes{}};
    if (id == "d23-probe-wrong-request-id-parity") return {subscribe(2, media, track, no_parameters())};
    if (id == "d23-probe-duplicate-request-id") {
        const auto message = subscribe(1, media, track, no_parameters());
        return {message, message};
    }
    if (id == "d23-probe-setup-delta-overflow") return {frame(0x2f00, join({vi(~std::uint64_t{0}), vi(0), vi(1), vi(0)}))};
    if (id == "d23-probe-setup-malformed-value") return {frame(0x2f00, join({vi(9), vi(1), raw({0x40})}))};
    if (id == "d23-probe-reason-phrase-too-long") return {request_error(kDoesNotExist, repeated('x', 1025), {})};
    if (id == "d23-probe-namespace-empty-field") return {subscribe(1, {Bytes{}}, track, no_parameters())};
    if (id == "d23-probe-namespace-too-many-fields") return {subscribe(1, std::vector<Bytes>(33, text("a")), track, no_parameters())};
    if (id == "d23-probe-namespace-too-long") return {subscribe(1, {repeated('a', 4100)}, track, no_parameters())};
    if (id == "d23-probe-request-ok-with-properties") {
        wire::ByteWriter writer(64);
        wire::draft23::RequestOkMessage message{{}, {{0x21, text("x")}}};
        if (!wire::draft23::encode_message(message, writer)) return {};
        return {{writer.bytes().begin(), writer.bytes().end()}};
    }
    if (id == "d23-probe-unknown-error-detail") {
        // REDIRECT (known, wrong code for it) and an unknown type that repeats: all ignored, none a reason to close.
        const Bytes details = join({vi(1), field(raw({0, 0, 0})), vi(0x40), field(text("a")), vi(0), field(text("b"))});
        return {request_error(kDoesNotExist, {}, details)};
    }
    if (id == "d23-probe-redirect-without-detail") return {request_error(kRedirect, {}, {})};
    if (id == "d23-probe-redirect-track-name-in-namespace-request") return {request_error(kRedirect, {}, redirect_detail(text("x")))};
    if (id == "d23-probe-unknown-request-error-code") return {request_error(0x7777, {}, {})};
    if (id == "d23-probe-publish-state-notify-on-announce" || id == "d23-probe-publish-state-notify-to-publisher")
        return {frame(0x22, vi(0))};
    if (id == "d23-probe-unknown-message-parameter") return {subscribe(1, media, track, one_parameter(0x7e, vi(1)))};
    if (id == "d23-probe-parameter-out-of-scope")  // LARGEST_OBJECT may not appear in SUBSCRIBE
        return {subscribe(1, media, track, one_parameter(9, join({vi(2), raw({0, 0})})))};
    if (id == "d23-probe-group-order-out-of-range") return {subscribe(1, media, track, one_parameter(0x22, raw({7})))};
    if (id == "d23-probe-paused-out-of-range") return {subscribe(1, media, track, one_parameter(0x11, raw({2})))};
    if (id == "d23-probe-include-properties-out-of-range") return {subscribe(1, media, track, one_parameter(0x35, raw({2})))};
    if (id == "d23-probe-subgroup-header-reserved-mode") return {raw({0x16, 7, 0, 0})};  // flags, alias, group, priority
    // Object Datagram: Type Flags, Track Alias, Group ID, Object ID, Priority, then what the flags select (Section 11.2.1).
    if (id == "d23-probe-datagram-unknown-type") return {raw({0x40, 7, 0, 0, 0, 'x'})};
    if (id == "d23-probe-datagram-reserved-bit") return {raw({0x10, 7, 0, 0, 0, 'x'})};
    if (id == "d23-probe-datagram-status-end-of-group") return {raw({0x22, 7, 0, 0, 0, 0})};
    if (id == "d23-probe-datagram-properties-length-zero") return {raw({0x01, 7, 0, 0, 0, 0, 'x'})};
    if (id == "d23-probe-datagram-status-properties-not-normal") return {raw({0x21, 7, 0, 0, 0, 2, 2, 5, 3})};
    // Parameters the publisher must reject (Sections 3.3.2, 9.19.9 and 9.19.15). Values stay below 64 so each is one byte.
    if (id == "d23-probe-filter-ranges-over-limit") {
        // One range more than the publisher allows; every range is Start=1, End=0 (delta from its own Start).
        Bytes ranges;
        for (std::uint64_t i = 0; i <= limits.filter_ranges; ++i) { ranges.push_back(std::byte{1}); ranges.push_back(std::byte{0}); }
        return {subscribe(1, media, track, one_parameter(0x25, field(ranges)))};
    }
    if (id == "d23-probe-location-filter-unknown-type") return {subscribe(1, media, track, one_parameter(0x21, vi(9)))};
    if (id == "d23-probe-location-filter-overflow")  // StartGroup 2^64-1 plus an EndGroupDelta of 1
        return {subscribe(1, media, track, one_parameter(0x21, join({vi(3), vi(~std::uint64_t{0}), vi(0), vi(1)})))};
    if (id == "d23-probe-fill-parameters-invalid")  // OBJECT_DELIVERY_TIMEOUT is not in the FILL_PARAMETERS list
        return {subscribe(1, media, track, one_parameter(0x23, join({vi(1), vi(2), vi(5)})))};
    if (id == "d23-probe-padding-stream") return {join({vi(0x132B3E28), repeated('\0', 16)})};
    if (id == "d23-probe-padding-datagram") return {join({vi(0x132B3E29), repeated('\0', 16)})};
    if (id == "d23-probe-goaway-twice") {
        const auto goaway = frame(0x10, join({vi(0), vi(0)}));  // empty New Session URI, timeout 0
        return {join({goaway, goaway})};
    }
    if (id == "d23-probe-goaway-uri-too-long") return {frame(0x10, join({vi(8193), repeated('u', 8193), vi(0)}))};
    if (id == "d23-probe-goaway-on-subscribe-stream") return {frame(0x10, join({vi(0), vi(0)}))};
    if (id == "d23-probe-request-update-wrong-direction") return {frame(0x02, join({vi(0), vi(0)}))};
    // A second REQUEST_OK (type 7, empty parameters and properties) on a request that has already been answered.
    if (id == "d23-probe-second-announce-response" || id == "d23-probe-second-publish-response") return {frame(7, join({vi(0), vi(0)}))};
    if (id == "d23-probe-duplicate-parameter")  // GROUP_ORDER twice
        return {subscribe(1, media, track, join({vi(2), vi(0x22), raw({1}), vi(0), raw({1})}))};
    (void)by_id;
    return {};
}

std::vector<std::byte> draft23_probe_bytes(const Draft23Probe& probe, std::uint64_t peer_message_limit) {
    auto payloads = draft23_probe_payloads(probe, peer_message_limit);
    return payloads.empty() ? Bytes{} : std::move(payloads.front());
}

void draft23_probe_setup_options(const Draft23Probe& probe, wire::draft21::KeyValues& options) {
    if (probe.scenario_id == "d23-probe-server-authority") options.push_back({5, text("example.test")});
    if (probe.scenario_id == "d23-probe-server-path") options.push_back({1, text("/moq")});
    // An option type no registry defines; both copies of the duplicate carry different values.
    if (probe.scenario_id == "d23-probe-unknown-setup-option") options.push_back({0x61, text("x")});
    if (probe.scenario_id == "d23-probe-duplicate-unknown-setup-option") {
        options.push_back({0x61, text("x")});
        options.push_back({0x61, text("y")});
    }
}

}  // namespace moq::interop::session
