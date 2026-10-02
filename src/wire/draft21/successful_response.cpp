#include "moq/interop/wire/draft21/successful_response.h"
#include "moq/interop/wire/draft21/request_frame.h"

#include <limits>
#include <utility>

namespace moq::interop::wire::draft21 {
namespace {
DecodeError violation(std::size_t offset, const char* detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, detail};
}

template <class T>
std::optional<DecodeError> bounded_error(const DecodeResult<T>& decoded) {
    if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
    if (const auto* need = std::get_if<NeedMore>(&decoded))
        return violation(need->offset, "truncated successful response body");
    return std::nullopt;
}

DecodeResult<std::vector<ResponseParameter>> decode_parameters(
    Cursor& body, std::uint64_t count, ResponseContext context) {
    std::vector<ResponseParameter> result;
    std::uint64_t previous = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto offset = body.offset();
        const auto delta = read_vi64(body);
        if (const auto error = bounded_error(delta)) return *error;
        const auto increment = std::get<std::uint64_t>(delta);
        if (index != 0 && increment == 0)
            return violation(offset, "duplicate nonrepeatable successful response parameter");
        if (increment > std::numeric_limits<std::uint64_t>::max() - previous)
            return violation(offset, "successful response parameter type overflow");
        const auto type = previous + increment;
        previous = type;
        // Sections 9.20.17-18 define the only parameters in these OKs.
        // In particular, Parameter9 uses two vi64 fields, not odd KV framing.
        if (context == ResponseContext::Fetch ||
            (type != 8 && (type != 9 || (context != ResponseContext::Subscribe &&
                                      context != ResponseContext::RequestUpdate))))
            return violation(offset, "parameter forbidden in successful response context");
        const auto first = read_vi64(body);
        if (const auto error = bounded_error(first)) return *error;
        if (type == 8) {
            result.push_back({type, std::get<std::uint64_t>(first)});
        } else {
            const auto object = read_vi64(body);
            if (const auto error = bounded_error(object)) return *error;
            result.push_back({type, Location{std::get<std::uint64_t>(first),
                                            std::get<std::uint64_t>(object)}});
        }
    }
    return result;
}

std::optional<DecodeError> validate_track_properties(std::span<const std::byte> bytes,
                                                   std::size_t absolute_offset) {
    // Views avoid recursive stack growth and copying each enclosing value
    // when a legal Track Immutable Properties wrapper contains another.
    struct PropertyBlock {
        std::span<const std::byte> bytes;
        std::size_t absolute_offset;
    };
    std::vector<PropertyBlock> pending{{bytes, absolute_offset}};
    while (!pending.empty()) {
        Cursor properties(pending.back().bytes, pending.back().absolute_offset);
        pending.pop_back();
        std::uint64_t previous = 0;
        while (properties.remaining() != 0) {
            const auto offset = properties.offset();
            const auto delta = read_vi64(properties);
            if (const auto error = bounded_error(delta)) return error;
            const auto increment = std::get<std::uint64_t>(delta);
            if (increment > std::numeric_limits<std::uint64_t>::max() - previous)
                return violation(offset, "Track Property type overflow");
            const auto type = previous + increment;
            previous = type;
            if (type >= 0x4000 && type <= 0x7fff)
                return violation(offset, "unknown mandatory Track Property requires cancellation");
            if (type == 0x3c || type == 0x3e)
                return violation(offset, "Object Property used as Track Property");
            if ((type & 1u) != 0) {
                const auto value = read_length_prefixed_bytes(properties, 65535);
                if (const auto error = bounded_error(value)) return error;
                if (type == 0x0b) {
                    const auto contents = std::get<std::span<const std::byte>>(value);
                    pending.push_back({contents, properties.offset() - contents.size()});
                }
                continue;
            }
            const auto value = read_vi64(properties);
            if (const auto error = bounded_error(value)) return error;
            const auto integer = std::get<std::uint64_t>(value);
            if ((type == 0x0e && integer > 255) ||
                (type == 0x22 && (integer < 1 || integer > 2)) ||
                (type == 0x30 && integer > 1))
                return violation(offset, "Track Property value outside defined bounds");
        }
    }
    return std::nullopt;
}
}  // namespace

std::optional<DecodeError> validate_track_properties(const KeyValues& properties) {
    ByteWriter encoded(65535);
    if (encode_key_values(properties, encoded))
        return violation(0, "invalid decoded Track Properties representation");
    return validate_track_properties(encoded.bytes(), 0);
}

DecodeResult<SuccessfulResponse> decode_successful_response(
    Cursor& input, ResponseContext context) {
    Cursor working = input;
    const auto frame = decode_request_frame(working, false);
    if (const auto* need = std::get_if<NeedMore>(&frame)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&frame)) return *error;
    const auto& decoded = std::get<RequestFrame>(frame);
    const auto expected = context == ResponseContext::Subscribe
        ? MessageKind::SubscribeOk : context == ResponseContext::Fetch
        ? MessageKind::FetchOk : MessageKind::RequestOk;
    if (decoded.type.kind != expected)
        return violation(input.offset(), "unexpected successful response message type");
    Cursor body(decoded.body, working.offset() - decoded.body.size());
    SuccessfulResponse result;
    if (context == ResponseContext::Subscribe) {
        const auto alias = read_vi64(body);
        if (const auto error = bounded_error(alias)) return *error;
        result.track_alias = std::get<std::uint64_t>(alias);
    }
    if (context == ResponseContext::Fetch) {
        const auto flag = read_bytes(body, 1);
        if (const auto error = bounded_error(flag)) return *error;
        const auto value = std::to_integer<std::uint8_t>(
            std::get<std::span<const std::byte>>(flag).front());
        if (value > 1) return violation(body.offset() - 1, "invalid FETCH_OK End Of Track");
        const auto group = read_vi64(body);
        if (const auto error = bounded_error(group)) return *error;
        const auto object = read_vi64(body);
        if (const auto error = bounded_error(object)) return *error;
        result.end_of_track = value;
        result.end_location = Location{std::get<std::uint64_t>(group),
                                       std::get<std::uint64_t>(object)};
    }
    const auto count = read_vi64(body);
    if (const auto error = bounded_error(count)) return *error;
    auto parameters = decode_parameters(body, std::get<std::uint64_t>(count), context);
    if (const auto error = bounded_error(parameters)) return *error;
    result.parameters = std::get<std::vector<ResponseParameter>>(std::move(parameters));
    if ((context == ResponseContext::RequestUpdate ||
         context == ResponseContext::SubscribeNamespace) && body.remaining() != 0)
        return violation(body.offset(), "successful response context forbids Track Properties");
    const auto property_offset = body.offset();
    const auto property_bytes = read_bytes(body, body.remaining());
    if (const auto error = bounded_error(property_bytes)) return *error;
    const auto property_span = std::get<std::span<const std::byte>>(property_bytes);
    if (const auto error = validate_track_properties(property_span, property_offset)) return *error;
    Cursor properties(property_span, property_offset);
    auto values = decode_key_values_to_end(properties);
    if (const auto error = bounded_error(values)) return *error;
    result.track_properties = std::get<KeyValues>(std::move(values));
    input = working;
    return result;
}
}  // namespace moq::interop::wire::draft21
