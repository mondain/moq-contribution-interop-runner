#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstdint>
#include <optional>
#include <span>

namespace moq::interop::wire::draft21 {

enum class LocationFilterKind {
    None,
    RelativeGroup,
    NextObject,
    Absolute,
};

struct LocationFilter {
    LocationFilterKind kind{LocationFilterKind::None};
    std::uint64_t start_group{0};
    std::uint64_t start_object{0};
    std::optional<std::uint64_t> end_group_delta;
    std::optional<std::uint64_t> end_object;
};

// Input is the complete length-delimited parameter value, not its framing.
DecodeResult<LocationFilter> decode_location_filter(
    std::span<const std::byte> payload);

}  // namespace moq::interop::wire::draft21
