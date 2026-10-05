#pragma once

#include "moq/interop/wire/draft22/publish.h"
#include "moq/interop/wire/draft22/shared.h"

#include <variant>

namespace moq::interop::wire::draft22 {

// The currently implemented publisher-facing subset, not the complete draft-22
// request-message set. Other active types are rejected explicitly.
using PublisherRequestMessage = std::variant<PublishMessage, RequestErrorMessage>;

DecodeResult<PublisherRequestMessage> decode_publisher_request_message(
    Cursor& input, bool first_on_stream, bool received_from_client = true,
    bool namespace_scoped_request = false);

}  // namespace moq::interop::wire::draft22
