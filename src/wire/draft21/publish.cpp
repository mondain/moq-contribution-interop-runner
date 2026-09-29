#include "moq/interop/wire/draft21/publish.h"

#include "moq/interop/wire/draft21/request_frame.h"

#include <span>
#include <utility>
#include <variant>

namespace moq::interop::wire::draft21 {
namespace {

constexpr std::size_t kMaximumFullTrackName = 4096;
constexpr std::uint64_t kMaximumNamespaceFields = 32;

DecodeError violation(std::size_t offset, const char* detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, detail};
}

DecodeResult<std::uint64_t> required_vi64(Cursor& input) {
    const auto result = read_vi64(input);
    if (std::holds_alternative<NeedMore>(result)) {
        return violation(input.offset(), "truncated draft-21 PUBLISH body");
    }
    return result;
}

DecodeResult<std::vector<std::byte>> required_field(Cursor& input,
                                                     std::size_t maximum) {
    const auto result = read_length_prefixed_bytes(input, maximum);
    if (std::holds_alternative<NeedMore>(result)) {
        return violation(input.offset(), "truncated draft-21 PUBLISH name");
    }
    if (const auto* error = std::get_if<DecodeError>(&result)) {
        if (error->code == DecodeErrorCode::LengthExceedsLimit) {
            return violation(error->offset, "draft-21 full track name exceeds 4096 bytes");
        }
        return *error;
    }
    const auto bytes = std::get<std::span<const std::byte>>(result);
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

}  // namespace

DecodeResult<PublishMessage> decode_publish(Cursor& input) {
    Cursor working = input;
    const auto decoded_frame = decode_request_frame(working, true);
    if (const auto* need = std::get_if<NeedMore>(&decoded_frame)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&decoded_frame)) return *error;
    const auto& frame = std::get<RequestFrame>(decoded_frame);
    if (frame.type.kind != MessageKind::Publish) {
        return violation(input.offset(), "expected draft-21 PUBLISH");
    }
    Cursor payload(frame.body, working.offset() - frame.body.size());
    const auto request_id = required_vi64(payload);
    if (const auto* error = std::get_if<DecodeError>(&request_id)) return *error;
    const auto namespace_count = required_vi64(payload);
    if (const auto* error = std::get_if<DecodeError>(&namespace_count)) return *error;
    const auto field_count = std::get<std::uint64_t>(namespace_count);
    if (field_count > kMaximumNamespaceFields) {
        return violation(payload.offset(), "draft-21 namespace has over 32 fields");
    }
    PublishMessage result{std::get<std::uint64_t>(request_id), {}, {}, 0, {}, {}};
    std::size_t namespace_length = 0;
    for (std::uint64_t index = 0; index < field_count; ++index) {
        const auto field = required_field(payload,
                                          kMaximumFullTrackName - namespace_length);
        if (const auto* error = std::get_if<DecodeError>(&field)) return *error;
        auto value = std::get<std::vector<std::byte>>(field);
        if (value.empty()) {
            return violation(payload.offset(), "empty draft-21 namespace field");
        }
        namespace_length += value.size();
        result.track_namespace.push_back(std::move(value));
    }
    const auto track_name = required_field(payload,
                                           kMaximumFullTrackName - namespace_length);
    if (const auto* error = std::get_if<DecodeError>(&track_name)) return *error;
    result.track_name = std::get<std::vector<std::byte>>(track_name);
    const auto alias = required_vi64(payload);
    if (const auto* error = std::get_if<DecodeError>(&alias)) return *error;
    result.track_alias = std::get<std::uint64_t>(alias);
    const auto parameter_count = required_vi64(payload);
    if (const auto* error = std::get_if<DecodeError>(&parameter_count)) return *error;
    const auto parameters = decode_key_values(
        payload, std::get<std::uint64_t>(parameter_count));
    if (std::holds_alternative<NeedMore>(parameters)) {
        return violation(payload.offset(), "truncated draft-21 PUBLISH parameters");
    }
    if (const auto* error = std::get_if<DecodeError>(&parameters)) return *error;
    result.parameters = std::get<KeyValues>(parameters);
    const auto properties = decode_key_values_to_end(payload);
    if (const auto* error = std::get_if<DecodeError>(&properties)) return *error;
    result.track_properties = std::get<KeyValues>(properties);
    input = working;
    return result;
}

}  // namespace moq::interop::wire::draft21
