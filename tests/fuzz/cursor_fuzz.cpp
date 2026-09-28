#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <variant>

namespace {

using moq::interop::wire::ByteWriter;
using moq::interop::wire::Cursor;
using moq::interop::wire::DecodeError;
using moq::interop::wire::NeedMore;

[[noreturn]] void invariant_failed() { std::abort(); }

void require(bool condition) {
    if (!condition) invariant_failed();
}

template <class T>
void require_decode_position(const std::variant<T, NeedMore, DecodeError>& result,
                             const Cursor& cursor, std::size_t initial_offset,
                             std::size_t maximum_offset) {
    if (std::holds_alternative<T>(result)) {
        require(cursor.offset() >= initial_offset);
        require(cursor.offset() <= maximum_offset);
    } else {
        require(cursor.offset() == initial_offset);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::as_bytes(std::span(data, size));

    Cursor integer_cursor(input);
    const auto integer_result = moq::interop::wire::read_vi64(integer_cursor);
    require_decode_position(integer_result, integer_cursor, 0, size);
    if (const auto* value = std::get_if<std::uint64_t>(&integer_result)) {
        ByteWriter encoded(9);
        require(moq::interop::wire::write_vi64(*value, encoded));
        Cursor round_trip(encoded.bytes());
        const auto decoded = moq::interop::wire::read_vi64(round_trip);
        require(std::holds_alternative<std::uint64_t>(decoded));
        require(std::get<std::uint64_t>(decoded) == *value);
        require(round_trip.remaining() == 0);
    }

    if (size <= (std::numeric_limits<std::size_t>::max() - 9u) / 2u) {
        ByteWriter self_append(size * 2u);
        require(self_append.append_bytes(input));
        const auto self_alias = self_append.bytes();
        require(self_append.append_bytes(self_alias));
        require(self_append.size() == size * 2u);

        ByteWriter framed(size * 2u + 9u);
        require(framed.append_bytes(input));
        const auto framed_alias = framed.bytes();
        require(moq::interop::wire::write_length_prefixed_bytes(framed_alias, framed));
        require(framed.size() > size);
    }

    const auto limit = size == 0 ? std::size_t{0}
                                 : static_cast<std::size_t>(data[0]);
    Cursor bytes_cursor(input);
    const auto bytes_result =
        moq::interop::wire::read_length_prefixed_bytes(bytes_cursor, limit);
    require_decode_position(bytes_result, bytes_cursor, 0, size);
    if (const auto* value =
            std::get_if<std::span<const std::byte>>(&bytes_result)) {
        require(value->size() <= limit);
    }

    Cursor string_cursor(input);
    const auto string_result =
        moq::interop::wire::read_length_prefixed_string(string_cursor, limit);
    require_decode_position(string_result, string_cursor, 0, size);
    if (const auto* value = std::get_if<std::string>(&string_result)) {
        require(value->size() <= limit);
    }

    Cursor high_offset_cursor(input, std::numeric_limits<std::size_t>::max() - size);
    const auto high_offset_result =
        moq::interop::wire::read_length_prefixed_bytes(high_offset_cursor, limit);
    require_decode_position(high_offset_result, high_offset_cursor,
                            std::numeric_limits<std::size_t>::max() - size,
                            std::numeric_limits<std::size_t>::max());

    return 0;
}
