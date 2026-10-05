#include "moq/interop/wire/draft22/location_filter.h"

#include <array>
#include <cstddef>
#include <limits>
#include <variant>

namespace moq::interop::wire::draft22 {
namespace {

// Number of vi64 fields that follow each Location Filter Type 0x00-0x05.
constexpr std::array<std::size_t, 6> kFieldCount{0, 1, 2, 3, 4, 0};

DecodeError violation(std::size_t offset, const char* detail) {
    return DecodeError{DecodeErrorCode::ProtocolViolation, offset, detail};
}

DecodeResult<std::uint64_t> required_vi64(Cursor& input) {
    const auto start = input.offset();
    auto result = read_vi64(input);
    if (std::holds_alternative<NeedMore>(result)) {
        return violation(start, "truncated draft-22 LOCATION_FILTER");
    }
    return result;
}

}  // namespace

DecodeResult<LocationFilter> decode_location_filter(Cursor& input) {
    Cursor working = input;
    const auto type_offset = working.offset();
    const auto type = required_vi64(working);
    if (const auto* error = std::get_if<DecodeError>(&type)) return *error;
    const auto raw_type = std::get<std::uint64_t>(type);
    if (raw_type >= kFieldCount.size()) {
        return violation(type_offset, "unknown draft-22 Location Filter Type");
    }
    const auto count = kFieldCount[static_cast<std::size_t>(raw_type)];
    std::array<std::uint64_t, 4> fields{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = required_vi64(working);
        if (const auto* error = std::get_if<DecodeError>(&value)) return *error;
        fields[index] = std::get<std::uint64_t>(value);
    }
    LocationFilter result;
    result.type = static_cast<LocationFilterType>(raw_type);
    if (count >= 1) result.start_group = fields[0];
    if (count >= 2) result.start_object = fields[1];
    if (count >= 3) {
        if (fields[2] > std::numeric_limits<std::uint64_t>::max() - fields[0]) {
            return violation(type_offset, "draft-22 LOCATION_FILTER end group overflows");
        }
        result.end_group_delta = fields[2];
    }
    if (count == 4) result.end_object = fields[3];
    input = working;
    return result;
}

std::optional<LocationFilterEncodeError> encode_location_filter(
    const LocationFilter& filter, ByteWriter& output) {
    const auto raw_type = static_cast<std::uint64_t>(filter.type);
    if (raw_type >= kFieldCount.size()) return LocationFilterEncodeError::InvalidValue;
    const auto count = kFieldCount[static_cast<std::size_t>(raw_type)];
    if (filter.end_group_delta.has_value() != (count >= 3) ||
        filter.end_object.has_value() != (count == 4) ||
        (count < 1 && filter.start_group != 0) ||
        (count < 2 && filter.start_object != 0)) {
        return LocationFilterEncodeError::InvalidValue;
    }
    if (filter.end_group_delta &&
        *filter.end_group_delta >
            std::numeric_limits<std::uint64_t>::max() - filter.start_group) {
        return LocationFilterEncodeError::InvalidValue;
    }
    ByteWriter encoded(output.remaining());
    bool ok = write_vi64(raw_type, encoded);
    if (ok && count >= 1) ok = write_vi64(filter.start_group, encoded);
    if (ok && count >= 2) ok = write_vi64(filter.start_object, encoded);
    if (ok && count >= 3) ok = write_vi64(*filter.end_group_delta, encoded);
    if (ok && count == 4) ok = write_vi64(*filter.end_object, encoded);
    if (!ok || !output.append_bytes(encoded.bytes())) {
        return LocationFilterEncodeError::OutputCapacity;
    }
    return std::nullopt;
}

}  // namespace moq::interop::wire::draft22
