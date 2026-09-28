#pragma once

#include "moq/interop/wire/draft18/messages.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft18 {

struct ObjectEvent {
    std::uint64_t datagram_type;
    std::uint64_t track_alias;
    std::uint64_t group_id;
    std::uint64_t object_id;
    std::optional<std::uint8_t> publisher_priority;
    bool end_of_group;
    KeyValuePairs properties;
    std::optional<std::uint64_t> status;
    std::size_t payload_length;
    std::vector<std::byte> retained_payload;
};

struct DiscardedPaddingDatagram {};

using DatagramDecodeResult =
    std::variant<ObjectEvent, DiscardedPaddingDatagram, DecodeError,
                 DraftAmbiguity>;

DatagramDecodeResult decode_datagram(std::span<const std::byte> bytes,
                                     const Limits& limits);

}  // namespace moq::interop::wire::draft18
