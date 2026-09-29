#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace moq::interop::wire::draft21 {

struct RedirectTarget {
    std::vector<std::byte> connect_uri;
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name;
};

struct RequestErrorMessage {
    std::uint64_t error_code;
    std::uint64_t retry_interval;
    std::vector<std::byte> reason;
    std::optional<RedirectTarget> redirect;
};

enum class RequestErrorEncodeError {
    InvalidValue,
    OutputCapacity,
};

DecodeResult<RequestErrorMessage> decode_request_error(
    Cursor& input, bool received_from_client, bool namespace_scoped_request);
std::optional<RequestErrorEncodeError> encode_request_error(
    const RequestErrorMessage& message, bool sending_as_client,
    bool namespace_scoped_request, ByteWriter& output);

}  // namespace moq::interop::wire::draft21
