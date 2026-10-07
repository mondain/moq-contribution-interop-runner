#include "moq/interop/wire/moqlite06/setup.h"

#include <algorithm>
#include <span>
#include <unordered_set>
#include <variant>

#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

DecodeError violation(std::size_t offset, const char* detail) {
    return DecodeError{DecodeErrorCode::ProtocolViolation, offset, detail};
}

// A field read from inside a body: running out of body is a malformed message, not a short input.
template <class T>
std::optional<DecodeError> body_error(const DecodeResult<T>& result, std::size_t offset, const char* detail) {
    if (std::holds_alternative<NeedMore>(result)) return violation(offset, detail);
    if (const auto* error = std::get_if<DecodeError>(&result)) return *error;
    return std::nullopt;
}

// The value must be exactly one varint.
DecodeResult<std::uint64_t> read_single_varint(const SetupParameter& parameter) {
    Cursor cursor(parameter.value);
    const auto result = read_varint(cursor);
    if (const auto error = body_error(result, 0, "setup parameter value is not a complete varint")) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;
    return std::get<std::uint64_t>(result);
}

std::vector<std::byte> varint_bytes(std::uint64_t value) {
    ByteWriter out(8);
    if (!write_varint(value, out)) return {};
    const auto written = out.bytes();
    return std::vector<std::byte>(written.begin(), written.end());
}

}  // namespace

DecodeResult<SetupMessage> decode_setup(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = read_framed_body(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto body = std::get<std::span<const std::byte>>(framed);

    Cursor cursor(body, working.offset() - body.size());
    const auto count_result = read_varint(cursor);
    if (const auto error = body_error(count_result, cursor.offset(), "setup message has no parameter count")) {
        return *error;
    }
    const auto count = std::get<std::uint64_t>(count_result);
    if (count > limits.max_parameters) {
        return DecodeError{DecodeErrorCode::LengthExceedsLimit, cursor.offset(),
                           "setup parameter count exceeds configured limit"};
    }

    SetupMessage message;
    std::unordered_set<std::uint64_t> seen;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto id_result = read_varint(cursor);
        if (const auto error = body_error(id_result, cursor.offset(), "setup parameter count exceeds parameters present")) {
            return *error;
        }
        const auto length_result = read_varint(cursor);
        if (const auto error = body_error(length_result, cursor.offset(), "setup parameter has no length")) {
            return *error;
        }
        const auto id = std::get<std::uint64_t>(id_result);
        const auto length = std::get<std::uint64_t>(length_result);
        if (length > cursor.remaining()) return violation(cursor.offset(), "setup parameter length exceeds message length");
        if (!seen.insert(id).second) return violation(cursor.offset(), "duplicate setup parameter id");

        const auto value = read_bytes(cursor, static_cast<std::size_t>(length));
        if (const auto error = body_error(value, cursor.offset(), "setup parameter value is truncated")) return *error;
        const auto& span = std::get<std::span<const std::byte>>(value);
        message.parameters.push_back(SetupParameter{id, std::vector<std::byte>(span.begin(), span.end())});
    }
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

std::optional<EncodeError> encode_setup(const SetupMessage& message, ByteWriter& output) {
    const auto& limits = kDefaultLimits;
    if (message.parameters.size() > limits.max_parameters) return EncodeError::LimitExceeded;

    std::unordered_set<std::uint64_t> seen;
    for (const auto& parameter : message.parameters) {
        if (parameter.id > kMaxVarint) return EncodeError::InvalidValue;
        if (!seen.insert(parameter.id).second) return EncodeError::InvalidValue;
    }

    ByteWriter body(limits.max_message_length);
    bool fits = write_varint(message.parameters.size(), body);
    for (const auto& parameter : message.parameters) {
        if (!fits) break;
        fits = write_varint(parameter.id, body) && write_varint(parameter.value.size(), body) &&
               body.append_bytes(parameter.value);
    }
    if (!fits) return EncodeError::LimitExceeded;

    if (!write_framed_message(body.bytes(), output)) return EncodeError::OutputCapacity;
    return std::nullopt;
}

DecodeResult<SetupCapabilities> read_capabilities(const SetupMessage& message, const DecodeLimits& limits) {
    SetupCapabilities capabilities;
    std::unordered_set<std::uint64_t> seen;
    for (const auto& parameter : message.parameters) {
        if (!seen.insert(parameter.id).second) return violation(0, "duplicate setup parameter id");

        std::optional<std::uint64_t>* slot = nullptr;
        switch (parameter.id) {
            case kParamProbe: slot = &capabilities.probe; break;
            case kParamRole: slot = &capabilities.role; break;
            case kParamCost: slot = &capabilities.cost; break;
            case kParamHop: slot = &capabilities.hop_id; break;
            case kParamPath:
                if (parameter.value.size() > limits.max_string_length) {
                    return DecodeError{DecodeErrorCode::LengthExceedsLimit, 0, "setup path exceeds configured limit"};
                }
                capabilities.path = std::string(reinterpret_cast<const char*>(parameter.value.data()),
                                                parameter.value.size());
                break;
            default: break;  // unknown ids are ignored (draft 7.3)
        }
        if (slot == nullptr) continue;

        const auto value = read_single_varint(parameter);
        if (const auto* error = std::get_if<DecodeError>(&value)) return *error;
        *slot = std::get<std::uint64_t>(value);
    }
    return capabilities;
}

SetupMessage build_setup(const SetupCapabilities& capabilities) {
    SetupMessage message;
    const auto add_varint = [&message](std::uint64_t id, const std::optional<std::uint64_t>& value) {
        if (value) message.parameters.push_back(SetupParameter{id, varint_bytes(*value)});
    };
    add_varint(kParamProbe, capabilities.probe);
    if (capabilities.path) {
        const auto* data = reinterpret_cast<const std::byte*>(capabilities.path->data());
        message.parameters.push_back(
            SetupParameter{kParamPath, std::vector<std::byte>(data, data + capabilities.path->size())});
    }
    add_varint(kParamRole, capabilities.role);
    add_varint(kParamCost, capabilities.cost);
    add_varint(kParamHop, capabilities.hop_id);
    return message;
}

ProbeLevel probe_level(const SetupCapabilities& capabilities) {
    if (!capabilities.probe) return ProbeLevel::None;
    return static_cast<ProbeLevel>(std::min<std::uint64_t>(*capabilities.probe, 2));
}

Role effective_role(const SetupCapabilities& capabilities) {
    if (!capabilities.role) return Role::Both;
    switch (*capabilities.role) {
        case 1: return Role::Publisher;
        case 2: return Role::Subscriber;
        default: return Role::Both;
    }
}

std::uint64_t effective_cost(const SetupCapabilities& capabilities) { return capabilities.cost.value_or(1); }

}  // namespace moq::interop::wire::moqlite06
