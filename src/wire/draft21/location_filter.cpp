#include "moq/interop/wire/draft21/location_filter.h"

#include <array>
#include <limits>
#include <variant>

namespace moq::interop::wire::draft21 {

DecodeResult<LocationFilter> decode_location_filter(
    std::span<const std::byte> payload) {
    if (payload.empty()) return LocationFilter{};
    Cursor input(payload);
    std::array<std::uint64_t, 4> fields{};
    std::size_t count = 0;
    while (input.remaining() != 0) {
        if (count == fields.size()) {
            return DecodeError{DecodeErrorCode::ProtocolViolation,
                               input.offset(),
                               "LOCATION_FILTER has over four fields"};
        }
        const auto value = read_vi64(input);
        if (std::holds_alternative<NeedMore>(value)) {
            return DecodeError{DecodeErrorCode::ProtocolViolation,
                               input.offset(),
                               "truncated LOCATION_FILTER varint"};
        }
        if (const auto* error = std::get_if<DecodeError>(&value)) return *error;
        fields[count++] = std::get<std::uint64_t>(value);
    }
    LocationFilter result;
    result.start_group = fields[0];
    if (count == 1) {
        result.kind = LocationFilterKind::RelativeGroup;
        return result;
    }
    result.start_object = fields[1];
    if (count == 2 && fields[0] == 0 && fields[1] == 0) {
        result.kind = LocationFilterKind::NextObject;
        return result;
    }
    result.kind = LocationFilterKind::Absolute;
    if (count >= 3) {
        if (fields[2] > std::numeric_limits<std::uint64_t>::max() - fields[0]) {
            return DecodeError{DecodeErrorCode::ProtocolViolation, 0,
                               "LOCATION_FILTER end group overflows"};
        }
        result.end_group_delta = fields[2];
    }
    if (count == 4) result.end_object = fields[3];
    return result;
}

}  // namespace moq::interop::wire::draft21
