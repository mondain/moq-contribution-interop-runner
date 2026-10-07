#include "moq/interop/wire/moqlite06/varint.h"

#include <array>
#include <limits>
#include <span>
#include <variant>

namespace moq::interop::wire::moqlite06 {

DecodeResult<std::uint64_t> read_varint(Cursor& input) {
    Cursor working = input;
    const auto start = input.offset();

    const auto first_result = read_bytes(working, 1);
    if (std::holds_alternative<NeedMore>(first_result)) return NeedMore{start, 1, input.remaining()};
    if (const auto* error = std::get_if<DecodeError>(&first_result)) return *error;
    const auto first = std::to_integer<std::uint8_t>(
        std::get<std::span<const std::byte>>(first_result)[0]);

    const std::size_t width = std::size_t{1} << (first >> 6u);
    const auto rest_result = read_bytes(working, width - 1);
    if (std::holds_alternative<NeedMore>(rest_result)) {
        return NeedMore{start, width, input.remaining()};
    }
    if (const auto* error = std::get_if<DecodeError>(&rest_result)) return *error;

    std::uint64_t value = first & 0x3fu;
    for (const auto octet : std::get<std::span<const std::byte>>(rest_result)) {
        value = (value << 8u) | std::to_integer<std::uint8_t>(octet);
    }
    input = working;
    return value;
}

std::size_t varint_size(std::uint64_t value) {
    if (value <= 0x3fu) return 1;
    if (value <= 0x3fffu) return 2;
    if (value <= 0x3fffffffu) return 4;
    if (value <= kMaxVarint) return 8;
    return 0;
}

bool write_varint(std::uint64_t value, ByteWriter& output) {
    const auto width = varint_size(value);
    if (width == 0 || width > output.remaining()) return false;

    std::array<std::byte, 8> encoded{};
    auto remaining_value = value;
    for (std::size_t index = width; index > 0; --index) {
        encoded[index - 1] = static_cast<std::byte>(remaining_value & 0xffu);
        remaining_value >>= 8u;
    }
    const unsigned prefix = width == 1 ? 0u : width == 2 ? 1u : width == 4 ? 2u : 3u;
    encoded[0] |= static_cast<std::byte>(prefix << 6u);
    return output.append_bytes(std::span<const std::byte>(encoded).first(width));
}

DecodeResult<std::string> read_string(Cursor& input, std::size_t max_length) {
    Cursor working = input;
    const auto length_result = read_varint(working);
    if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;

    const auto declared = std::get<std::uint64_t>(length_result);
    if (declared > max_length) {
        return DecodeError{DecodeErrorCode::LengthExceedsLimit, input.offset(),
                           "string length exceeds configured limit"};
    }

    const auto bytes_result = read_bytes(working, static_cast<std::size_t>(declared));
    if (const auto* need = std::get_if<NeedMore>(&bytes_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&bytes_result)) return *error;

    const auto encoded = std::get<std::span<const std::byte>>(bytes_result);
    std::string value;
    value.reserve(encoded.size());
    for (const auto octet : encoded) {
        value.push_back(static_cast<char>(std::to_integer<unsigned char>(octet)));
    }
    input = working;
    return value;
}

bool write_string(std::string_view value, ByteWriter& output) {
    const auto prefix = varint_size(value.size());
    if (prefix == 0 || prefix > output.remaining()) return false;
    if (value.size() > output.remaining() - prefix) return false;
    if (!write_varint(value.size(), output)) return false;
    return output.append_bytes(std::as_bytes(std::span(value)));
}

}  // namespace moq::interop::wire::moqlite06
