#include "moq/interop/wire/draft21/setup.h"

#include "moq/interop/wire/draft21/token.h"

#include <array>
#include <limits>
#include <span>
#include <utility>

namespace moq::interop::wire::draft21 {
namespace {

constexpr std::uint64_t kSetupType = 0x2f00;
constexpr std::size_t kMaximumBodyLength = 65'535;

bool known_option(std::uint64_t type) {
    return type == 1 || type == 3 || type == 4 || type == 5 ||
           type == 6 || type == 7 || type == 8;
}

DecodeError protocol_error(std::size_t offset, const char* detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, detail};
}

}  // namespace

DecodeResult<SetupMessage> decode_setup(Cursor& input) {
    Cursor working = input;
    const auto type = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&type)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type)) return *error;
    if (std::get<std::uint64_t>(type) != kSetupType) {
        return protocol_error(input.offset(), "expected draft-21 SETUP");
    }
    const auto length_bytes = read_bytes(working, 2);
    if (const auto* need = std::get_if<NeedMore>(&length_bytes)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length_bytes)) return *error;
    const auto length_span = std::get<std::span<const std::byte>>(length_bytes);
    const auto body_length =
        (static_cast<std::size_t>(std::to_integer<unsigned>(length_span[0])) << 8u) |
        std::to_integer<unsigned>(length_span[1]);
    const auto body = read_bytes(working, body_length);
    if (const auto* need = std::get_if<NeedMore>(&body)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&body)) return *error;

    Cursor payload(std::get<std::span<const std::byte>>(body),
                   working.offset() - body_length);
    SetupMessage result;
    std::uint64_t previous_type = 0;
    while (payload.remaining() != 0) {
        const auto option_offset = payload.offset();
        const auto delta = read_vi64(payload);
        if (std::holds_alternative<NeedMore>(delta)) {
            return protocol_error(option_offset, "truncated SETUP option type");
        }
        if (const auto* error = std::get_if<DecodeError>(&delta)) return *error;
        const auto increment = std::get<std::uint64_t>(delta);
        if (increment > std::numeric_limits<std::uint64_t>::max() -
                            previous_type) {
            return protocol_error(option_offset, "SETUP option type overflow");
        }
        const auto option_type = previous_type + increment;
        if (increment == 0 && known_option(option_type) && option_type != 3) {
            return protocol_error(option_offset, "duplicate SETUP option");
        }
        previous_type = option_type;

        if ((option_type & 1u) == 0u) {
            const auto value = read_vi64(payload);
            if (std::holds_alternative<NeedMore>(value)) {
                return protocol_error(option_offset, "truncated SETUP integer");
            }
            if (const auto* error = std::get_if<DecodeError>(&value)) return *error;
            result.options.push_back({option_type,
                                      std::get<std::uint64_t>(value)});
            continue;
        }
        const auto value_length = read_vi64(payload);
        if (std::holds_alternative<NeedMore>(value_length)) {
            return protocol_error(option_offset, "truncated SETUP value length");
        }
        if (const auto* error = std::get_if<DecodeError>(&value_length)) {
            return *error;
        }
        const auto declared = std::get<std::uint64_t>(value_length);
        if (declared > kMaximumBodyLength) {
            return protocol_error(option_offset, "SETUP value exceeds 65535 bytes");
        }
        const auto value = read_bytes(payload, static_cast<std::size_t>(declared));
        if (std::holds_alternative<NeedMore>(value)) {
            return protocol_error(option_offset, "truncated SETUP value");
        }
        if (const auto* error = std::get_if<DecodeError>(&value)) return *error;
        const auto value_bytes = std::get<std::span<const std::byte>>(value);
        if (option_type == 3) {
            const auto token = decode_token(value_bytes,
                                            payload.offset() - value_bytes.size());
            if (const auto* error = std::get_if<DecodeError>(&token)) return *error;
            if (std::holds_alternative<NeedMore>(token)) {
                return protocol_error(option_offset, "truncated SETUP Token");
            }
        }
        result.options.push_back({option_type,
                                  std::vector<std::byte>(value_bytes.begin(),
                                                         value_bytes.end())});
    }
    input = working;
    return result;
}

std::optional<SetupEncodeError> encode_setup(const SetupMessage& message,
                                             ByteWriter& output) {
    ByteWriter body(kMaximumBodyLength);
    std::uint64_t previous_type = 0;
    for (const auto& option : message.options) {
        if (option.type < previous_type ||
            (option.type == previous_type && known_option(option.type) &&
             option.type != 3)) {
            return SetupEncodeError::InvalidValue;
        }
        if (!write_vi64(option.type - previous_type, body)) {
            return SetupEncodeError::OutputCapacity;
        }
        previous_type = option.type;
        if ((option.type & 1u) == 0u) {
            const auto* integer = std::get_if<std::uint64_t>(&option.value);
            if (!integer) return SetupEncodeError::InvalidValue;
            if (!write_vi64(*integer, body)) {
                return SetupEncodeError::OutputCapacity;
            }
            continue;
        }
        const auto* bytes = std::get_if<std::vector<std::byte>>(&option.value);
        if (!bytes || bytes->size() > kMaximumBodyLength) {
            return SetupEncodeError::InvalidValue;
        }
        if (option.type == 3 &&
            !std::holds_alternative<Token>(decode_token(*bytes))) {
            return SetupEncodeError::InvalidValue;
        }
        if (!write_vi64(bytes->size(), body) || !body.append_bytes(*bytes)) {
            return SetupEncodeError::OutputCapacity;
        }
    }

    ByteWriter frame(kMaximumBodyLength + 11);
    const auto length = body.size();
    const std::array<std::byte, 2> length_bytes{
        static_cast<std::byte>(length >> 8u), static_cast<std::byte>(length)};
    if (!write_vi64(kSetupType, frame) ||
        !frame.append_bytes(length_bytes) ||
        !frame.append_bytes(body.bytes())) {
        return SetupEncodeError::OutputCapacity;
    }
    if (!output.append_bytes(frame.bytes())) {
        return SetupEncodeError::OutputCapacity;
    }
    return std::nullopt;
}

}  // namespace moq::interop::wire::draft21
