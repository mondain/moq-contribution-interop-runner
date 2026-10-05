#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"

#include <stdexcept>
#include <utility>

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

Bytes frame(unsigned type, const Bytes& body) {
    if (body.size() > 65535) throw std::logic_error("probe frame exceeds uint16");
    auto result = bytes({type, static_cast<unsigned>(body.size() >> 8u),
                         static_cast<unsigned>(body.size() & 255u)});
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<wire::draft21::SetupMessage>(
        wire::draft21::decode_setup(cursor));
}

bool publish_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = decode_publish_for_wire(cursor);
    const auto* publish = std::get_if<wire::draft21::PublishMessage>(&decoded);
    return publish && publish->request_id % 2 == 0 && cursor.remaining() == 0;
}

std::optional<std::uint64_t> number(wire::Cursor& input) {
    const auto decoded = wire::read_vi64(input);
    const auto* value = std::get_if<std::uint64_t>(&decoded);
    return value ? std::optional{*value} : std::nullopt;
}

bool namespace_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_request_frame(cursor, true);
    const auto* request = std::get_if<wire::draft21::RequestFrame>(&decoded);
    if (!request || request->type.kind != wire::draft21::MessageKind::PublishNamespace ||
        cursor.remaining() != 0) return false;
    wire::Cursor body(request->body);
    const auto request_id = number(body);
    const auto fields = number(body);
    if (!request_id || *request_id % 2 != 0 || !fields || *fields > 32) return false;
    std::size_t namespace_size = 0;
    for (std::uint64_t index = 0; index < *fields; ++index) {
        const auto decoded_field = wire::read_length_prefixed_bytes(body, 4096 - namespace_size);
        const auto* field = std::get_if<std::span<const std::byte>>(&decoded_field);
        if (!field || field->empty()) return false;
        namespace_size += field->size();
    }
    const auto count = number(body);
    if (!count) return false;
    // Section 9.20.3 is the only defined parameter scope admitting
    // PUBLISH_NAMESPACE. The server SETUP negotiates no extensions.
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto delta = number(body);
        if (!delta || *delta != (index == 0 ? 3u : 0u)) return false;
        const auto decoded_token = wire::read_length_prefixed_bytes(body, 65535);
        const auto* token = std::get_if<std::span<const std::byte>>(&decoded_token);
        if (!token || !std::holds_alternative<wire::draft21::Token>(
                          wire::draft21::decode_token(*token))) return false;
    }
    return body.remaining() == 0;
}
}  // namespace

std::vector<Draft21PeerCloseProbe> draft21_peer_close_probes(
    std::chrono::milliseconds deadline) {
    std::vector<Draft21PeerCloseProbe> result;
    const auto add = [&](const char* requirement, const char* scenario,
                         const char* evaluator, Bytes response, bool publish) {
        RawProbeDefinition definition{scenario, bytes({0xaf, 0, 0, 0}),
            {{RawProbeChannel::PeerBidi, std::move(response), false}}, true,
            setup_ready, deadline, {}, publish ? publish_ready : namespace_ready};
        result.push_back({requirement, evaluator, 3, std::move(definition)});
    };
    // Sections 8.5 and 9.4.2: the complete enclosing REQUEST_ERROR
    // carries only an oversized reason; it has no Redirect fields.
    auto reason = bytes({0, 0});
    integer(reason, 1025);
    reason.insert(reason.end(), 1025, std::byte{'x'});
    add("D21-8-5-MUST-248", "d21-publish-request-error-oversized-reason",
        "d21-oversized-reason-phrase-protocol-violation", frame(5, reason), true);
    // Sections 9.3 and 10.3: MAX_CACHE_DURATION is a legal Track
    // Property, but these two REQUEST_OK contexts forbid properties.
    const auto properties = frame(7, bytes({0, 4, 1}));
    add("D21-9-3-MUST-337", "d21-publish-ok-with-track-properties",
        "d21-request-ok-forbidden-track-properties", properties, true);
    add("D21-9-3-MUST-337", "d21-publish-namespace-ok-with-track-properties",
        "d21-request-ok-forbidden-track-properties", properties, false);
    auto update = result[result.size() - 2];
    update.definition.id = "d21-publish-update-ok-with-track-properties";
    update.definition.writes.front().bytes = frame(7, bytes({0}));
    update.definition.peer_request_ready = [](std::span<const std::byte> input) {
        wire::Cursor cursor(input);
        const auto decoded = decode_publish_for_wire(cursor);
        const auto* publish = std::get_if<wire::draft21::PublishMessage>(&decoded);
        return publish && publish->request_id == 0 && cursor.remaining() == 0;
    };
    RawProbeWrite update_reply{RawProbeChannel::PeerBidi, properties, false};
    update_reply.reuse_write_stream = 0;
    // A parameterless update avoids admitting an independently invalid scope.
    // The initial PUBLISH gate fixes ID0, so a nonzero even ID is fresh here.
    update_reply.peer_response_ready = [](std::span<const std::byte> input) {
        wire::Cursor cursor(input);
        const auto decoded = wire::draft21::decode_request_frame(cursor, false);
        const auto* request = std::get_if<wire::draft21::RequestFrame>(&decoded);
        if (!request || request->type.kind != wire::draft21::MessageKind::RequestUpdate ||
            cursor.remaining() != 0) return false;
        wire::Cursor body(request->body);
        const auto id = number(body);
        const auto count = number(body);
        return id && *id > 0 && *id % 2 == 0 && count && *count == 0 && body.remaining() == 0;
    };
    update.definition.writes.push_back(std::move(update_reply));
    result.push_back(std::move(update));
    // Empty Connect URI prevents a separate receiver-role violation;
    // the nonempty Track Name alone violates namespace Redirect scope.
    add("D21-9-4-1-MUST-341", "d21-publish-namespace-redirect-nonempty-track-name",
        "d21-namespace-redirect-track-name-protocol-violation",
        frame(5, bytes({0x34, 0, 0, 0, 1, 1, 'n', 1, 'x'})), false);
    // Section 9.5: unlike the subscriber of PUBLISH, a responder to
    // PUBLISH_NAMESPACE cannot send REQUEST_UPDATE on that request.
    add("D21-9-5-MUST-344", "d21-responder-update-on-publish-namespace",
        "d21-request-update-context-and-direction", frame(7, bytes({0})), false);
    result.back().definition.writes.push_back(
        {RawProbeChannel::PeerBidi, frame(2, bytes({1, 0})), false, 0, {}});
    return result;
}

}  // namespace moq::interop::scenarios
