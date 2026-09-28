#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace moq::interop::wire {

struct NeedMore {
    std::size_t offset;
    std::size_t required;
    std::size_t available;
};

enum class DecodeErrorCode {
    InvalidValue,
    ProtocolViolation,
    KeyValueFormattingError,
    LengthExceedsLimit,
    LengthNotRepresentable,
    OffsetOverflow,
};

struct DecodeError {
    DecodeErrorCode code;
    std::size_t offset;
    std::string detail;
};

template <class T>
using DecodeResult = std::variant<T, NeedMore, DecodeError>;

class Cursor {
public:
    explicit Cursor(std::span<const std::byte> bytes,
                    std::size_t absolute_offset = 0) noexcept;

    [[nodiscard]] std::size_t offset() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;

private:
    friend DecodeResult<std::span<const std::byte>> read_bytes(Cursor&, std::size_t);
    friend DecodeResult<std::uint64_t> read_vi64(Cursor&);

    std::span<const std::byte> bytes_;
    std::size_t absolute_offset_;
    std::size_t position_{0};
};

class ByteWriter {
public:
    explicit ByteWriter(std::size_t maximum_size) noexcept;

    [[nodiscard]] bool append_byte(std::byte value);
    [[nodiscard]] bool append_bytes(std::span<const std::byte> values);
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;

private:
    std::size_t maximum_size_;
    std::vector<std::byte> bytes_;
};

DecodeResult<std::span<const std::byte>> read_bytes(Cursor& input, std::size_t length);
DecodeResult<std::uint64_t> read_vi64(Cursor& input);
bool write_vi64(std::uint64_t value, ByteWriter& output);

DecodeResult<std::span<const std::byte>> read_length_prefixed_bytes(
    Cursor& input, std::size_t maximum_length);
DecodeResult<std::string> read_length_prefixed_string(Cursor& input,
                                                      std::size_t maximum_length);
bool write_length_prefixed_bytes(std::span<const std::byte> value,
                                 ByteWriter& output);
bool write_length_prefixed_string(std::string_view value, ByteWriter& output);

}  // namespace moq::interop::wire
