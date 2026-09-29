#include "moq/interop/wire/draft21/request_error.h"

#include "moq/interop/wire/draft21/request_frame.h"

#include <array>
#include <span>
#include <utility>
#include <variant>

namespace moq::interop::wire::draft21 {
namespace {

constexpr std::uint64_t kRequestErrorType = 0x05;
constexpr std::uint64_t kRedirectCode = 0x34;
constexpr std::size_t kMaximumBodyLength = 65535;
constexpr std::size_t kMaximumReasonLength = 1024;
constexpr std::size_t kMaximumFullTrackName = 4096;
constexpr std::uint64_t kMaximumNamespaceFields = 32;

DecodeError violation(std::size_t offset, const char* detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, detail};
}

DecodeResult<std::uint64_t> required_vi64(Cursor& input) {
    const auto value = read_vi64(input);
    if (std::holds_alternative<NeedMore>(value)) {
        return violation(input.offset(), "truncated draft-21 REQUEST_ERROR body");
    }
    return value;
}

DecodeResult<std::vector<std::byte>> required_field(Cursor& input,
                                                     std::size_t maximum) {
    const auto value = read_length_prefixed_bytes(input, maximum);
    if (std::holds_alternative<NeedMore>(value)) {
        return violation(input.offset(), "truncated draft-21 REQUEST_ERROR field");
    }
    if (const auto* error = std::get_if<DecodeError>(&value)) {
        if (error->code == DecodeErrorCode::LengthExceedsLimit) {
            return violation(error->offset, "draft-21 REQUEST_ERROR field too long");
        }
        return *error;
    }
    const auto bytes = std::get<std::span<const std::byte>>(value);
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

DecodeResult<RedirectTarget> decode_redirect(Cursor& input,
                                              bool received_from_client,
                                              bool namespace_scoped_request) {
    RedirectTarget redirect;
    const auto uri = required_field(input, kMaximumBodyLength);
    if (const auto* error = std::get_if<DecodeError>(&uri)) return *error;
    redirect.connect_uri = std::get<std::vector<std::byte>>(uri);
    if (received_from_client && !redirect.connect_uri.empty()) {
        return violation(input.offset(), "client Redirect URI must be empty");
    }
    const auto count = required_vi64(input);
    if (const auto* error = std::get_if<DecodeError>(&count)) return *error;
    const auto field_count = std::get<std::uint64_t>(count);
    if (field_count > kMaximumNamespaceFields) {
        return violation(input.offset(), "Redirect namespace has over 32 fields");
    }
    std::size_t namespace_length = 0;
    for (std::uint64_t index = 0; index < field_count; ++index) {
        const auto field = required_field(input,
                                          kMaximumFullTrackName - namespace_length);
        if (const auto* error = std::get_if<DecodeError>(&field)) return *error;
        auto bytes = std::get<std::vector<std::byte>>(field);
        if (bytes.empty()) {
            return violation(input.offset(), "empty Redirect namespace field");
        }
        namespace_length += bytes.size();
        redirect.track_namespace.push_back(std::move(bytes));
    }
    const auto name = required_field(input,
                                     kMaximumFullTrackName - namespace_length);
    if (const auto* error = std::get_if<DecodeError>(&name)) return *error;
    redirect.track_name = std::get<std::vector<std::byte>>(name);
    if (namespace_scoped_request && !redirect.track_name.empty()) {
        return violation(input.offset(), "namespace Redirect track name must be empty");
    }
    return redirect;
}

bool valid_redirect(const RedirectTarget& redirect, bool sending_as_client,
                    bool namespace_scoped_request) {
    if ((sending_as_client && !redirect.connect_uri.empty()) ||
        (namespace_scoped_request && !redirect.track_name.empty()) ||
        redirect.track_namespace.size() > kMaximumNamespaceFields ||
        redirect.track_name.size() > kMaximumFullTrackName) {
        return false;
    }
    std::size_t name_length = redirect.track_name.size();
    for (const auto& field : redirect.track_namespace) {
        if (field.empty() || field.size() > kMaximumFullTrackName - name_length) {
            return false;
        }
        name_length += field.size();
    }
    return true;
}

}  // namespace

DecodeResult<RequestErrorMessage> decode_request_error(
    Cursor& input, bool received_from_client, bool namespace_scoped_request) {
    Cursor working = input;
    const auto framed = decode_request_frame(working, false);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& frame = std::get<RequestFrame>(framed);
    if (frame.type.type != kRequestErrorType) {
        return violation(input.offset(), "expected draft-21 REQUEST_ERROR");
    }
    Cursor payload(frame.body, working.offset() - frame.body.size());
    const auto code = required_vi64(payload);
    if (const auto* error = std::get_if<DecodeError>(&code)) return *error;
    const auto retry = required_vi64(payload);
    if (const auto* error = std::get_if<DecodeError>(&retry)) return *error;
    const auto reason = required_field(payload, kMaximumReasonLength);
    if (const auto* error = std::get_if<DecodeError>(&reason)) return *error;
    RequestErrorMessage result{std::get<std::uint64_t>(code),
                               std::get<std::uint64_t>(retry),
                               std::get<std::vector<std::byte>>(reason),
                               std::nullopt};
    if (result.error_code == kRedirectCode) {
        const auto redirect = decode_redirect(payload, received_from_client,
                                              namespace_scoped_request);
        if (const auto* error = std::get_if<DecodeError>(&redirect)) return *error;
        result.redirect = std::get<RedirectTarget>(redirect);
    }
    if (payload.remaining() != 0) {
        return violation(payload.offset(), "draft-21 REQUEST_ERROR body length mismatch");
    }
    input = working;
    return result;
}

std::optional<RequestErrorEncodeError> encode_request_error(
    const RequestErrorMessage& message, bool sending_as_client,
    bool namespace_scoped_request, ByteWriter& output) {
    if (message.reason.size() > kMaximumReasonLength ||
        (message.error_code == kRedirectCode) != message.redirect.has_value() ||
        (message.redirect &&
         !valid_redirect(*message.redirect, sending_as_client,
                         namespace_scoped_request))) {
        return RequestErrorEncodeError::InvalidValue;
    }
    ByteWriter body(kMaximumBodyLength);
    if (!write_vi64(message.error_code, body) ||
        !write_vi64(message.retry_interval, body) ||
        !write_length_prefixed_bytes(message.reason, body)) {
        return RequestErrorEncodeError::OutputCapacity;
    }
    if (message.redirect) {
        const auto& redirect = *message.redirect;
        if (!write_length_prefixed_bytes(redirect.connect_uri, body) ||
            !write_vi64(redirect.track_namespace.size(), body)) {
            return RequestErrorEncodeError::OutputCapacity;
        }
        for (const auto& field : redirect.track_namespace) {
            if (!write_length_prefixed_bytes(field, body)) {
                return RequestErrorEncodeError::OutputCapacity;
            }
        }
        if (!write_length_prefixed_bytes(redirect.track_name, body)) {
            return RequestErrorEncodeError::OutputCapacity;
        }
    }
    ByteWriter frame(kMaximumBodyLength + 11);
    const std::array<std::byte, 2> length{
        static_cast<std::byte>(body.size() >> 8u),
        static_cast<std::byte>(body.size())};
    if (!write_vi64(kRequestErrorType, frame) ||
        !frame.append_bytes(length) || !frame.append_bytes(body.bytes()) ||
        !output.append_bytes(frame.bytes())) {
        return RequestErrorEncodeError::OutputCapacity;
    }
    return std::nullopt;
}

}  // namespace moq::interop::wire::draft21
