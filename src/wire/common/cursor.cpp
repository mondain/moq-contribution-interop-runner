#include "moq/interop/wire/cursor.h"

#include <array>
#include <limits>
#include <utility>

namespace moq::interop::wire {
namespace {

DecodeError offset_overflow(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::OffsetOverflow, offset, std::move(detail)};
}

std::size_t vi64_width(std::byte first_byte) {
    const auto first = std::to_integer<std::uint8_t>(first_byte);
    if (first == 0xffu) return 9;
    std::size_t leading_ones = 0;
    auto mask = std::uint8_t{0x80};
    while ((first & mask) != 0u) {
        ++leading_ones;
        mask = static_cast<std::uint8_t>(mask >> 1u);
    }
    return leading_ones + 1;
}

std::size_t shortest_vi64_width(std::uint64_t value) {
    for (std::size_t width = 1; width < 9; ++width) {
        const auto usable_bits = static_cast<unsigned>(width * 7);
        if (value <= ((std::uint64_t{1} << usable_bits) - 1u)) return width;
    }
    return 9;
}

}  // namespace

Cursor::Cursor(std::span<const std::byte> bytes, std::size_t absolute_offset) noexcept
    : bytes_(bytes), absolute_offset_(absolute_offset) {}

std::size_t Cursor::offset() const noexcept { return absolute_offset_ + position_; }

std::size_t Cursor::remaining() const noexcept { return bytes_.size() - position_; }

ByteWriter::ByteWriter(std::size_t maximum_size) noexcept
    : maximum_size_(maximum_size) {}

bool ByteWriter::append_byte(std::byte value) {
    return append_bytes(std::span<const std::byte>(&value, 1));
}

bool ByteWriter::append_bytes(std::span<const std::byte> values) {
    if (values.size() > remaining()) return false;
    bytes_.insert(bytes_.end(), values.begin(), values.end());
    return true;
}

std::span<const std::byte> ByteWriter::bytes() const noexcept { return bytes_; }

std::size_t ByteWriter::size() const noexcept { return bytes_.size(); }

std::size_t ByteWriter::remaining() const noexcept { return maximum_size_ - bytes_.size(); }

DecodeResult<std::span<const std::byte>> read_bytes(Cursor& input, std::size_t length) {
    const auto start = input.offset();
    if (length > input.remaining()) {
        return NeedMore{start, length, input.remaining()};
    }
    if (length > std::numeric_limits<std::size_t>::max() - start) {
        return offset_overflow(start, "read overflows absolute cursor offset");
    }
    const auto value = input.bytes_.subspan(input.position_, length);
    input.position_ += length;
    return value;
}

DecodeResult<std::uint64_t> read_vi64(Cursor& input) {
    const auto start = input.offset();
    if (input.remaining() == 0) return NeedMore{start, 1, 0};

    const auto width = vi64_width(input.bytes_[input.position_]);
    if (input.remaining() < width) return NeedMore{start, width, input.remaining()};
    if (width > std::numeric_limits<std::size_t>::max() - start) {
        return offset_overflow(start, "integer read overflows absolute cursor offset");
    }

    const auto first = std::to_integer<std::uint8_t>(input.bytes_[input.position_]);
    std::uint64_t value = 0;
    if (width < 9) {
        const auto payload_bits = static_cast<unsigned>(8 - width);
        const auto payload_mask = static_cast<std::uint8_t>(
            (std::uint32_t{1} << payload_bits) - 1u);
        value = first & payload_mask;
    }
    for (std::size_t index = 1; index < width; ++index) {
        value = (value << 8u) |
                std::to_integer<std::uint8_t>(input.bytes_[input.position_ + index]);
    }
    input.position_ += width;
    return value;
}

bool write_vi64(std::uint64_t value, ByteWriter& output) {
    const auto width = shortest_vi64_width(value);
    std::array<std::byte, 9> encoded{};
    auto remaining_value = value;
    for (std::size_t index = width; index > 1; --index) {
        encoded[index - 1] = static_cast<std::byte>(remaining_value & 0xffu);
        remaining_value >>= 8u;
    }
    if (width == 9) {
        encoded[0] = std::byte{0xff};
    } else {
        const auto prefix = width == 1
                                ? std::uint8_t{0}
                                : static_cast<std::uint8_t>(0xffu << (9u - width));
        encoded[0] = static_cast<std::byte>(
            prefix | static_cast<std::uint8_t>(remaining_value));
    }
    return output.append_bytes(std::span<const std::byte>(encoded).first(width));
}

DecodeResult<std::span<const std::byte>> read_length_prefixed_bytes(
    Cursor& input, std::size_t maximum_length) {
    Cursor working = input;
    const auto length_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;

    const auto declared = std::get<std::uint64_t>(length_result);
    if (declared > std::numeric_limits<std::size_t>::max()) {
        return DecodeError{DecodeErrorCode::LengthNotRepresentable, input.offset(),
                           "declared length cannot be represented by size_t"};
    }
    const auto length = static_cast<std::size_t>(declared);
    if (length > maximum_length) {
        return DecodeError{DecodeErrorCode::LengthExceedsLimit, input.offset(),
                           "declared length exceeds configured limit"};
    }
    if (length > std::numeric_limits<std::size_t>::max() - working.offset()) {
        return offset_overflow(input.offset(),
                               "declared length overflows absolute cursor offset");
    }

    const auto value_result = read_bytes(working, length);
    if (const auto* need = std::get_if<NeedMore>(&value_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&value_result)) return *error;
    const auto value = std::get<std::span<const std::byte>>(value_result);
    input = working;
    return value;
}

DecodeResult<std::string> read_length_prefixed_string(Cursor& input,
                                                      std::size_t maximum_length) {
    Cursor working = input;
    const auto bytes_result = read_length_prefixed_bytes(working, maximum_length);
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

bool write_length_prefixed_bytes(std::span<const std::byte> value,
                                 ByteWriter& output) {
    if constexpr (std::numeric_limits<std::size_t>::max() >
                  std::numeric_limits<std::uint64_t>::max()) {
        if (value.size() > std::numeric_limits<std::uint64_t>::max()) return false;
    }
    ByteWriter prefix(9);
    if (!write_vi64(static_cast<std::uint64_t>(value.size()), prefix)) return false;
    if (prefix.size() > output.remaining()) return false;
    if (value.size() > output.remaining() - prefix.size()) return false;
    if (!output.append_bytes(prefix.bytes())) return false;
    return output.append_bytes(value);
}

bool write_length_prefixed_string(std::string_view value, ByteWriter& output) {
    return write_length_prefixed_bytes(std::as_bytes(std::span(value)), output);
}

}  // namespace moq::interop::wire
