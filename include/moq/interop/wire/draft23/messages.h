#pragma once

#include "moq/interop/wire/draft22/location_filter.h"
#include "moq/interop/wire/draft21/key_values.h"
#include "moq/interop/wire/draft21/publish.h"

namespace moq::interop::wire::draft23 {

// Draft 23 section 9.19: parameter types are delta encoded in each scope.
struct Parameter {
    std::uint64_t type;
    std::variant<std::uint8_t, std::uint64_t, draft21::Location,
                 std::vector<std::byte>, draft22::LocationFilter,
                 std::vector<Parameter>> value;
};
using Parameters = std::vector<Parameter>;
using TrackNamespace = std::vector<std::vector<std::byte>>;
struct PublishNamespaceMessage { std::uint64_t request_id; TrackNamespace track_namespace; Parameters parameters; };
struct PublishMessage {
    std::uint64_t request_id;
    TrackNamespace track_namespace;
    std::vector<std::byte> track_name;
    std::uint64_t track_alias;
    Parameters parameters;
    draft21::KeyValues track_properties;
};
struct SubscribeMessage { std::uint64_t request_id; TrackNamespace track_namespace; std::vector<std::byte> track_name; Parameters parameters; };
struct FetchMessage { std::uint64_t request_id; TrackNamespace track_namespace; std::vector<std::byte> track_name; Parameters parameters; };
struct RequestOkMessage { Parameters parameters; draft21::KeyValues track_properties; };
struct SubscribeOkMessage { std::uint64_t track_alias; Parameters parameters; draft21::KeyValues track_properties; };
struct RequestErrorMessage {
    std::uint64_t error_code;
    std::uint64_t retry_interval;
    std::vector<std::byte> reason;
    draft21::KeyValues error_details;
};
struct PublishDoneMessage { std::uint64_t status_code; std::uint64_t stream_count; std::vector<std::byte> reason; };
struct GoawayMessage { std::vector<std::byte> uri; std::uint64_t timeout; };
using Message = std::variant<PublishNamespaceMessage, PublishMessage, SubscribeMessage,
                             RequestOkMessage, SubscribeOkMessage, RequestErrorMessage, FetchMessage,
                             PublishDoneMessage, GoawayMessage>;

struct MessageDecodeFailure {
    DecodeError error;
    std::uint64_t session_error;
    bool unsupported{false};  // No normative error code is claimed for a harness limit.
};
using MessageDecodeResult = std::variant<Message, NeedMore, MessageDecodeFailure>;
constexpr std::uint64_t session_error_code(const MessageDecodeFailure& failure) noexcept {
    return failure.session_error;
}
MessageDecodeResult decode_message_with_error(Cursor& input, std::uint64_t maximum_body_size = 65535);

// Structural parsing plus parameter scope/value checks. REQUEST_OK context, property
// semantics and request lifecycle belong to the session. Neither operation partially
// advances its caller's cursor/output on failure. Unknown types are errors.
bool encode_message(const Message& message, ByteWriter& output, std::uint64_t maximum_body_size = 65535);
DecodeResult<Message> decode_message(Cursor& input, std::uint64_t maximum_body_size = 65535);

}  // namespace moq::interop::wire::draft23
