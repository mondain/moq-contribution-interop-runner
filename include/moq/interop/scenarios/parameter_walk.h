#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace moq::interop::scenarios {

// One reader of draft 21-family Message Parameters (Section 9.20) for scenario evaluators, aware of the
// run's wire draft. Each parameter is a vi64 Type delta (ascending) followed by a value whose encoding the
// Type dictates:
//   Byte            0x10 0x20 0x22 0x35                      one octet
//   Varint          0x02 0x04 0x06 0x08 0x0a 0x32            one vi64
//   Location        0x09                                     two vi64s (Group, Object)
//   LengthPrefixed  0x03 0x23 0x25-0x29 0x34, and 0x21 below wire draft 22   vi64 Length (<= 65535), bytes
//   LocationFilter  0x21 under wire draft 22                 Type + the fields it dictates, no Length
//                                                            (wire::draft22::decode_location_filter)
// Any other type cannot be skipped and stops the walk.
enum class ParameterValueKind { Byte, Varint, Location, LengthPrefixed, LocationFilter };

enum class ParameterWalkStatus {
    Complete,      // every parameter was read
    Malformed,     // a delta or value was truncated, a Length exceeded 65535, or a filter did not decode
    TypeOverflow,  // a delta took the Type past 2^64-1
    UnknownType,   // a Type whose value encoding is not known
};

struct WalkedParameter {
    std::uint64_t type{0};
    ParameterValueKind kind{ParameterValueKind::Byte};
    std::span<const std::byte> value;    // every byte after the Type delta
    std::span<const std::byte> payload;  // LengthPrefixed: the bytes after the Length; otherwise == value
    std::optional<std::uint64_t> number;                                    // Byte and Varint
    std::optional<std::pair<std::uint64_t, std::uint64_t>> location;        // Location
    // FILL_PARAMETERS (0x23): the status of walking its payload as nested parameters by the same rule
    // (walk_nested_parameters). The payload is length-bounded, so this never changes the outer framing.
    std::optional<ParameterWalkStatus> nested;
};

struct ParameterWalkResult {
    ParameterWalkStatus status{ParameterWalkStatus::Complete};
    std::size_t visited{0};                  // parameters read and passed to the visitor
    std::optional<std::uint64_t> failed_type;  // Malformed value or UnknownType: the Type the walk stopped at
};

using ParameterVisitor = std::function<void(const WalkedParameter&)>;

// Walks `count` parameters from `body`, calling `visit` for each one fully read, in order. Stops at the
// first failure; where the cursor is left after a failure is unspecified.
ParameterWalkResult walk_message_parameters(wire::Cursor& body, std::uint64_t count, const ParameterVisitor& visit);

// Walks a FILL_PARAMETERS payload: parameters with no count, up to the end of `payload`.
ParameterWalkResult walk_nested_parameters(std::span<const std::byte> payload, const ParameterVisitor& visit);

}  // namespace moq::interop::scenarios
