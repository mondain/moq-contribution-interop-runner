#pragma once

#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/request_error.h"

#include <variant>

namespace moq::interop::wire::draft21 {

// The currently implemented publisher-facing subset, not the complete
// draft-21 request-message set. Other active types are rejected explicitly.
using PublisherRequestMessage =
    std::variant<PublishMessage, RequestErrorMessage>;

DecodeResult<PublisherRequestMessage> decode_publisher_request_message(
    Cursor& input, bool first_on_stream, bool received_from_client = true,
    bool namespace_scoped_request = false);

}  // namespace moq::interop::wire::draft21
