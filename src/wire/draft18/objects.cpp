#include "moq/interop/wire/draft18/objects.h"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

namespace moq::interop::wire::draft18 {
namespace {

constexpr std::uint64_t kPaddingDatagramType = 0x132b3e29;

DecodeError protocol_violation(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, std::move(detail)};
}

DecodeError truncated_field(const NeedMore& need, std::string detail) {
    return protocol_violation(need.offset, std::move(detail));
}

DecodeError malformed_properties(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::KeyValueFormattingError, offset,
            std::move(detail)};
}

bool is_valid_object_type(std::uint64_t type) {
    if (type <= 0x0f) return true;
    if (type < 0x20 || type > 0x2f) return false;
    return (type & 0x02u) == 0u;
}

DatagramDecodeResult decode_padding(Cursor& working) {
    while (working.remaining() != 0) {
        const auto byte_offset = working.offset();
        const auto byte_result = read_bytes(working, 1);
        if (const auto* need = std::get_if<NeedMore>(&byte_result)) {
            return truncated_field(*need, "padding byte is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&byte_result)) return *error;
        if (std::get<std::span<const std::byte>>(byte_result).front() !=
            std::byte{0}) {
            return protocol_violation(byte_offset,
                                      "padding datagram contains nonzero data");
        }
    }
    return DiscardedPaddingDatagram{};
}

}  // namespace

DatagramDecodeResult decode_datagram(std::span<const std::byte> bytes,
                                     const Limits& limits) {
    Cursor working(bytes);
    const auto type_offset = working.offset();
    const auto type_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&type_result)) {
        return truncated_field(*need, "datagram Type is truncated");
    }
    if (const auto* error = std::get_if<DecodeError>(&type_result)) return *error;
    const auto type = std::get<std::uint64_t>(type_result);

    if (type == kPaddingDatagramType) {
        auto padding_result = decode_padding(working);
        return padding_result;
    }
    if (!is_valid_object_type(type)) {
        return protocol_violation(type_offset, "unknown or invalid datagram type");
    }

    const auto track_alias_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&track_alias_result)) {
        return truncated_field(*need, "Track Alias is truncated");
    }
    if (const auto* error = std::get_if<DecodeError>(&track_alias_result)) return *error;
    const auto group_id_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&group_id_result)) {
        return truncated_field(*need, "Group ID is truncated");
    }
    if (const auto* error = std::get_if<DecodeError>(&group_id_result)) return *error;

    std::uint64_t object_id = 0;
    if ((type & 0x04u) == 0u) {
        const auto object_id_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&object_id_result)) {
            return truncated_field(*need, "Object ID is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&object_id_result)) return *error;
        object_id = std::get<std::uint64_t>(object_id_result);
    }

    std::optional<std::uint8_t> publisher_priority;
    if ((type & 0x08u) == 0u) {
        const auto priority_result = read_bytes(working, 1);
        if (const auto* need = std::get_if<NeedMore>(&priority_result)) {
            return truncated_field(*need, "Publisher Priority is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&priority_result)) return *error;
        publisher_priority = std::to_integer<std::uint8_t>(
            std::get<std::span<const std::byte>>(priority_result).front());
    }

    KeyValuePairs properties;
    if ((type & 0x01u) != 0u) {
        const auto properties_length_offset = working.offset();
        const auto properties_length_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&properties_length_result)) {
            return truncated_field(*need, "Object Properties length is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&properties_length_result)) {
            return *error;
        }
        const auto declared = std::get<std::uint64_t>(properties_length_result);
        if (declared == 0) {
            return protocol_violation(properties_length_offset,
                                      "present Object Properties are empty");
        }
        if (declared > std::numeric_limits<std::size_t>::max()) {
            return DecodeError{DecodeErrorCode::LengthNotRepresentable,
                               properties_length_offset,
                               "Object Properties length is not representable"};
        }
        const auto properties_length = static_cast<std::size_t>(declared);
        if (properties_length > limits.maximum_object_properties_length) {
            return DecodeError{DecodeErrorCode::LengthExceedsLimit,
                               properties_length_offset,
                               "Object Properties exceed configured limit"};
        }
        const auto properties_result =
            decode_key_value_pairs(working, properties_length, limits);
        if (auto* decoded = std::get_if<KeyValuePairs>(&properties_result)) {
            properties = std::move(*decoded);
        } else if (const auto* need = std::get_if<NeedMore>(&properties_result)) {
            return malformed_properties(
                need->offset,
                "Object Properties are shorter than their declared length");
        } else if (const auto* error = std::get_if<DecodeError>(&properties_result)) {
            if (error->code == DecodeErrorCode::InvalidValue) {
                return malformed_properties(error->offset,
                                            "Object Property KVP is malformed");
            }
            return *error;
        } else {
            return std::get<DraftAmbiguity>(properties_result);
        }
    }

    std::optional<std::uint64_t> status;
    std::size_t payload_length = 0;
    std::vector<std::byte> retained_payload;
    if ((type & 0x20u) != 0u) {
        const auto status_offset = working.offset();
        const auto status_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&status_result)) {
            return truncated_field(*need, "Object Status is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&status_result)) return *error;
        status = std::get<std::uint64_t>(status_result);
        if (working.remaining() != 0) {
            return protocol_violation(working.offset(),
                                      "bytes follow Object Status");
        }
        if (!properties.empty() && *status != 0) {
            return protocol_violation(status_offset,
                                      "non-Normal status has Object Properties");
        }
    } else {
        payload_length = working.remaining();
        const auto retained_length =
            std::min(payload_length, limits.maximum_retained_payload_length);
        const auto retained_result = read_bytes(working, retained_length);
        if (const auto* need = std::get_if<NeedMore>(&retained_result)) {
            return truncated_field(*need, "Object payload evidence is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&retained_result)) return *error;
        const auto retained =
            std::get<std::span<const std::byte>>(retained_result);
        retained_payload.assign(retained.begin(), retained.end());
        const auto discarded_result = read_bytes(working, working.remaining());
        if (const auto* need = std::get_if<NeedMore>(&discarded_result)) {
            return truncated_field(*need, "Object payload is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&discarded_result)) return *error;
    }

    return ObjectEvent{
        type,
        std::get<std::uint64_t>(track_alias_result),
        std::get<std::uint64_t>(group_id_result),
        object_id,
        publisher_priority,
        (type & 0x02u) != 0u,
        std::move(properties),
        status,
        payload_length,
        std::move(retained_payload),
    };
}

}  // namespace moq::interop::wire::draft18
