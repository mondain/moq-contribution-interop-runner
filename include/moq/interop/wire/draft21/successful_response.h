#pragma once

#include "moq/interop/wire/draft21/publish.h"

#include <optional>

namespace moq::interop::wire::draft21 {

enum class ResponseContext { Subscribe, SubscribeNamespace, SubscribeTracks, RequestUpdate, Fetch };

struct ResponseParameter {
    std::uint64_t type;
    std::variant<std::uint64_t, Location> value;
};

struct SuccessfulResponse {
    std::optional<std::uint64_t> track_alias;
    std::vector<ResponseParameter> parameters;
    KeyValues track_properties;
    std::optional<std::uint8_t> end_of_track;
    std::optional<Location> end_location;
};

// Validate already decoded Track Properties with the same bounds and
// nested Immutable Properties rules as a successful response.
std::optional<DecodeError> validate_track_properties(const KeyValues& properties);

// Decode one complete OK, validating parameter scope and Track Property
// encoding/defined bounds. No negotiated extension parameters are assumed.
DecodeResult<SuccessfulResponse> decode_successful_response(
    Cursor& input, ResponseContext context);

}  // namespace moq::interop::wire::draft21
