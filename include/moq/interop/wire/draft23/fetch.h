#pragma once
#include "moq/interop/wire/draft21/publish.h"
#include <optional>

namespace moq::interop::wire::draft23 {
struct FetchState {
    bool descending{false};
    std::optional<draft21::Location> prior_location;
    std::optional<std::uint64_t> prior_subgroup;
    std::optional<std::uint8_t> prior_priority;
    bool ended_track{false};
};
struct FetchObject {
    std::uint64_t flags;
    draft21::Location location;
    std::optional<std::uint64_t> subgroup;
    std::uint8_t priority;
    draft21::KeyValues properties;
    std::vector<std::byte> payload;
};
enum class FetchRangeKind { NonExistent, Unknown, TimedOut, Gone };
struct FetchRange { FetchRangeKind kind; draft21::Location location; };
struct FetchEnd { bool end_of_track; };
using FetchEntry = std::variant<FetchObject, FetchRange, FetchEnd>;
// Decode one entry after FETCH_HEADER; caller owns request association. State
// and cursor advance together only after a complete entry. Payload and property
// bounds are harness resource limits, not claims about legal wire maxima.
DecodeResult<FetchEntry> decode_fetch_entry(Cursor& input, FetchState& state,
    std::size_t maximum_payload = 1u << 20, std::size_t maximum_properties = 65535);
}  // namespace moq::interop::wire::draft23
