#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstdint>
#include <optional>

namespace moq::interop::wire::draft22 {

// draft-ietf-moq-transport-22 section 9.20.9. The wire value is the Location Filter Type;
// there is no Length field and no length-delimited payload any more.
enum class LocationFilterType : std::uint64_t {
    None = 0,
    RelativeGroup = 1,
    Absolute = 2,
    AbsoluteBounded = 3,
    AbsoluteRange = 4,
    NextObject = 5,
};

// Wire-faithful: a field is populated exactly when the Type carries it, otherwise it is
// zero (or empty for the optional ones).
struct LocationFilter {
    LocationFilterType type{LocationFilterType::None};
    std::uint64_t start_group{0};
    std::uint64_t start_object{0};
    std::optional<std::uint64_t> end_group_delta;
    std::optional<std::uint64_t> end_object;
};

enum class LocationFilterEncodeError {
    InvalidValue,
    OutputCapacity,
};

// Reads the Type and exactly the fields it dictates from `input`, which is the remainder
// of a length-bounded message body. An unknown Type, a StartGroup + EndGroupDelta overflow
// and truncation are all ProtocolViolation. NeedMore is never returned. The cursor is only
// advanced on success.
DecodeResult<LocationFilter> decode_location_filter(Cursor& input);

// Writes nothing when the filter is inconsistent (populated fields must be exactly those
// its Type carries, and StartGroup + EndGroupDelta must not overflow) or does not fit.
std::optional<LocationFilterEncodeError> encode_location_filter(
    const LocationFilter& filter, ByteWriter& output);

}  // namespace moq::interop::wire::draft22
