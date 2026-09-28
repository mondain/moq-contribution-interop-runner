#include "moq/interop/wire/draft18/messages.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace moq::interop::wire::draft18 {
namespace {

constexpr std::uint64_t kSetupMessageType = 0x2f00;
constexpr std::size_t kMaximumMessagePayload = 65'535;
constexpr std::size_t kMaximumFrameSize = kMaximumMessagePayload + 11;

DecodeError invalid(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::InvalidValue, offset, std::move(detail)};
}

template <class T>
DraftDecodeResult<T> bounded_payload_error(const NeedMore& need,
                                           std::string detail) {
    return invalid(need.offset, std::move(detail));
}

std::vector<std::byte> copy_bytes(std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

bool is_known_setup_option(std::uint64_t type) {
    return type == 0x01 || type == 0x03 || type == 0x04 || type == 0x05 ||
           type == 0x07;
}

bool is_repeatable_setup_option(std::uint64_t type) { return type == 0x03; }

std::optional<DecodeError> validate_setup_options(
    std::span<const KeyValuePair> options, std::size_t offset) {
    std::optional<std::uint64_t> previous;
    for (const auto& option : options) {
        if (previous && option.type == *previous &&
            is_known_setup_option(option.type) &&
            !is_repeatable_setup_option(option.type)) {
            return invalid(offset, "duplicate non-repeatable SETUP option");
        }
        previous = option.type;
    }
    return std::nullopt;
}

EncodeResult encode_key_value_pairs_to(std::span<const KeyValuePair> entries,
                                       ByteWriter& output) {
    std::uint64_t previous_type = 0;
    bool first = true;
    for (const auto& entry : entries) {
        if (!first && entry.type < previous_type) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         "KVP types are not ordered");
        }
        const auto delta = first ? entry.type : entry.type - previous_type;
        if (!write_vi64(delta, output)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "KVP type exceeds output capacity");
        }

        if ((entry.type & 1u) == 0u) {
            const auto* integer = std::get_if<VarIntValue>(&entry.value);
            if (integer == nullptr) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "even KVP type requires vi64 value");
            }
            if (!write_vi64(integer->value, output)) {
                return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                             "KVP integer exceeds output capacity");
            }
        } else {
            const auto* bytes = std::get_if<ByteValue>(&entry.value);
            if (bytes == nullptr) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "odd KVP type requires byte value");
            }
            if (bytes->bytes.size() > kMaximumMessagePayload) {
                return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                             "KVP byte value exceeds draft limit");
            }
            if (!write_length_prefixed_bytes(bytes->bytes, output)) {
                return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                             "KVP bytes exceed output capacity");
            }
        }
        previous_type = entry.type;
        first = false;
    }
    return EncodeResult::success();
}

}  // namespace

EncodeResult::EncodeResult(bool success, EncodeError error)
    : success_(success), error_(std::move(error)) {}

EncodeResult EncodeResult::success() { return EncodeResult(true); }

EncodeResult EncodeResult::failure(EncodeErrorCode code, std::string detail) {
    return EncodeResult(false, EncodeError{code, std::move(detail)});
}

bool EncodeResult::has_value() const noexcept { return success_; }

const EncodeError* EncodeResult::error() const noexcept {
    return success_ ? nullptr : &error_;
}

KeyValueDecodeResult decode_key_value_pairs(Cursor& input,
                                            std::size_t payload_length,
                                            const Limits& limits) {
    Cursor working = input;
    const auto payload_result = read_bytes(working, payload_length);
    if (const auto* need = std::get_if<NeedMore>(&payload_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&payload_result)) return *error;

    const auto payload = std::get<std::span<const std::byte>>(payload_result);
    Cursor bounded(payload, input.offset());
    KeyValuePairs entries;
    std::uint64_t previous_type = 0;
    while (bounded.remaining() != 0) {
        const auto entry_offset = bounded.offset();
        const auto delta_result = read_vi64(bounded);
        if (const auto* need = std::get_if<NeedMore>(&delta_result)) {
            return bounded_payload_error<KeyValuePairs>(
                *need, "KVP type is truncated within bounded payload");
        }
        if (const auto* error = std::get_if<DecodeError>(&delta_result)) return *error;
        const auto delta = std::get<std::uint64_t>(delta_result);
        if (delta > std::numeric_limits<std::uint64_t>::max() - previous_type) {
            return invalid(entry_offset, "KVP resolved type overflows uint64");
        }
        const auto type = previous_type + delta;

        if ((type & 1u) == 0u) {
            const auto value_start = bounded.remaining();
            const auto value_result = read_vi64(bounded);
            if (const auto* need = std::get_if<NeedMore>(&value_result)) {
                return bounded_payload_error<KeyValuePairs>(
                    *need, "KVP integer is truncated within bounded payload");
            }
            if (const auto* error = std::get_if<DecodeError>(&value_result)) return *error;
            const auto consumed = value_start - bounded.remaining();
            const auto raw_offset = payload.size() - value_start;
            entries.push_back(KeyValuePair{
                type,
                VarIntValue{std::get<std::uint64_t>(value_result),
                            copy_bytes(payload.subspan(raw_offset, consumed))}});
        } else {
            const auto length_result = read_vi64(bounded);
            if (const auto* need = std::get_if<NeedMore>(&length_result)) {
                return bounded_payload_error<KeyValuePairs>(
                    *need, "KVP length is truncated within bounded payload");
            }
            if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
            const auto declared = std::get<std::uint64_t>(length_result);
            const auto configured_limit =
                std::min(limits.maximum_odd_value_length, kMaximumMessagePayload);
            if (declared > configured_limit) {
                return DecodeError{DecodeErrorCode::LengthExceedsLimit, entry_offset,
                                   "KVP byte value exceeds configured limit"};
            }
            const auto value_result =
                read_bytes(bounded, static_cast<std::size_t>(declared));
            if (const auto* need = std::get_if<NeedMore>(&value_result)) {
                return bounded_payload_error<KeyValuePairs>(
                    *need, "KVP bytes are truncated within bounded payload");
            }
            if (const auto* error = std::get_if<DecodeError>(&value_result)) return *error;
            entries.push_back(KeyValuePair{
                type, ByteValue{copy_bytes(
                          std::get<std::span<const std::byte>>(value_result))}});
        }
        previous_type = type;
    }

    input = working;
    return entries;
}

EncodeResult encode_key_value_pairs(std::span<const KeyValuePair> entries,
                                    ByteWriter& output) {
    ByteWriter staged(kMaximumMessagePayload);
    const auto result = encode_key_value_pairs_to(entries, staged);
    if (!result.has_value()) return result;
    if (!output.append_bytes(staged.bytes())) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "encoded KVPs exceed output capacity");
    }
    return EncodeResult::success();
}

TrackPropertiesDecodeResult decode_track_properties(Cursor& input,
                                                     std::size_t payload_length,
                                                     const Limits& limits) {
    Cursor working = input;
    const auto result = decode_key_value_pairs(working, payload_length, limits);
    if (const auto* value = std::get_if<KeyValuePairs>(&result)) {
        input = working;
        return TrackProperties{std::move(*value)};
    }
    if (const auto* need = std::get_if<NeedMore>(&result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&result)) return *error;
    return std::get<DraftAmbiguity>(result);
}

bool is_mandatory_track_property(const KeyValuePair& property) noexcept {
    return property.type >= 0x4000 && property.type <= 0x7fff;
}

MessageDecodeResult decode_message(StreamRole role, Cursor& input,
                                   const Limits& limits) {
    Cursor working = input;
    const auto type_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&type_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type_result)) return *error;

    const auto type = std::get<std::uint64_t>(type_result);
    if (type != kSetupMessageType) {
        return invalid(input.offset(), "unknown, reserved, or removed message type");
    }
    if (role != StreamRole::Control) {
        return invalid(input.offset(), "SETUP is not valid on a request stream");
    }

    const auto length_result = read_bytes(working, 2);
    if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
    const auto length_bytes = std::get<std::span<const std::byte>>(length_result);
    const auto payload_length =
        (static_cast<std::size_t>(std::to_integer<std::uint8_t>(length_bytes[0])) << 8u) |
        static_cast<std::size_t>(std::to_integer<std::uint8_t>(length_bytes[1]));

    const auto payload_offset = working.offset();
    const auto payload_result = read_bytes(working, payload_length);
    if (const auto* need = std::get_if<NeedMore>(&payload_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&payload_result)) return *error;

    const auto payload = std::get<std::span<const std::byte>>(payload_result);
    Cursor payload_cursor(payload, payload_offset);
    const auto options_result =
        decode_key_value_pairs(payload_cursor, payload.size(), limits);
    if (const auto* need = std::get_if<NeedMore>(&options_result)) {
        return invalid(need->offset, "SETUP option exceeds framed payload");
    }
    if (const auto* error = std::get_if<DecodeError>(&options_result)) return *error;
    if (const auto* ambiguity = std::get_if<DraftAmbiguity>(&options_result)) {
        return *ambiguity;
    }

    auto options = std::get<KeyValuePairs>(options_result);
    if (const auto duplicate = validate_setup_options(options, payload_offset)) {
        return *duplicate;
    }
    input = working;
    return Message{SetupMessage{std::move(options)}};
}

EncodeResult encode_message(const Message& message, ByteWriter& output) {
    const auto& setup = std::get<SetupMessage>(message);
    if (const auto duplicate = validate_setup_options(setup.options, 0)) {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     duplicate->detail);
    }

    ByteWriter payload(kMaximumMessagePayload);
    const auto payload_result = encode_key_value_pairs_to(setup.options, payload);
    if (!payload_result.has_value()) return payload_result;
    if (payload.size() > kMaximumMessagePayload) {
        return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                     "message payload exceeds uint16");
    }

    ByteWriter frame(kMaximumFrameSize);
    if (!write_vi64(kSetupMessageType, frame)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "message type exceeds frame capacity");
    }
    const std::array<std::byte, 2> length{
        static_cast<std::byte>((payload.size() >> 8u) & 0xffu),
        static_cast<std::byte>(payload.size() & 0xffu),
    };
    if (!frame.append_bytes(length) || !frame.append_bytes(payload.bytes())) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "message exceeds frame capacity");
    }
    if (!output.append_bytes(frame.bytes())) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "message exceeds output capacity");
    }
    return EncodeResult::success();
}

}  // namespace moq::interop::wire::draft18
