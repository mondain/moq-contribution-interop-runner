#include "moq/interop/wire/draft21/key_values.h"

#include <limits>
#include <span>
#include <variant>

namespace moq::interop::wire::draft21 {
namespace {

constexpr std::size_t kMaximumOddValueLength = 65535;

DecodeError violation(std::size_t offset, const char* detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, detail};
}

}  // namespace

DecodeResult<KeyValues> decode_key_values(Cursor& input,
                                          std::uint64_t count) {
    Cursor working = input;
    KeyValues result;
    std::uint64_t previous_type = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto pair_offset = working.offset();
        const auto delta = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&delta)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&delta)) return *error;
        const auto increment = std::get<std::uint64_t>(delta);
        if (increment > std::numeric_limits<std::uint64_t>::max() -
                            previous_type) {
            return violation(pair_offset, "draft-21 key-value type overflow");
        }
        const auto type = previous_type + increment;
        previous_type = type;
        if ((type & 1u) == 0u) {
            const auto value = read_vi64(working);
            if (const auto* need = std::get_if<NeedMore>(&value)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&value)) return *error;
            result.push_back({type, std::get<std::uint64_t>(value)});
            continue;
        }
        const auto length = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&length)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&length)) return *error;
        const auto declared = std::get<std::uint64_t>(length);
        if (declared > kMaximumOddValueLength) {
            return violation(pair_offset, "draft-21 odd value exceeds 65535 bytes");
        }
        const auto value = read_bytes(working, static_cast<std::size_t>(declared));
        if (const auto* need = std::get_if<NeedMore>(&value)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&value)) return *error;
        const auto bytes = std::get<std::span<const std::byte>>(value);
        result.push_back({type, std::vector<std::byte>(bytes.begin(),
                                                      bytes.end())});
    }
    input = working;
    return result;
}

std::optional<KeyValueEncodeError> encode_key_values(
    const KeyValues& values, ByteWriter& output) {
    ByteWriter encoded(output.remaining());
    std::uint64_t previous_type = 0;
    for (const auto& pair : values) {
        if (pair.type < previous_type) return KeyValueEncodeError::InvalidValue;
        if (!write_vi64(pair.type - previous_type, encoded)) {
            return KeyValueEncodeError::OutputCapacity;
        }
        previous_type = pair.type;
        if ((pair.type & 1u) == 0u) {
            const auto* integer = std::get_if<std::uint64_t>(&pair.value);
            if (!integer) return KeyValueEncodeError::InvalidValue;
            if (!write_vi64(*integer, encoded)) {
                return KeyValueEncodeError::OutputCapacity;
            }
            continue;
        }
        const auto* bytes = std::get_if<std::vector<std::byte>>(&pair.value);
        if (!bytes || bytes->size() > kMaximumOddValueLength) {
            return KeyValueEncodeError::InvalidValue;
        }
        if (!write_length_prefixed_bytes(*bytes, encoded)) {
            return KeyValueEncodeError::OutputCapacity;
        }
    }
    if (!output.append_bytes(encoded.bytes())) {
        return KeyValueEncodeError::OutputCapacity;
    }
    return std::nullopt;
}

}  // namespace moq::interop::wire::draft21
