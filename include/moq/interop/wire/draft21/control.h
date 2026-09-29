#pragma once

#include "moq/interop/wire/draft21/goaway.h"
#include "moq/interop/wire/draft21/setup.h"

#include <variant>

namespace moq::interop::wire::draft21 {

using ControlMessage = std::variant<SetupMessage, GoawayMessage>;

DecodeResult<ControlMessage> decode_control_message(
    Cursor& input, bool first_on_stream, bool received_from_client);

}  // namespace moq::interop::wire::draft21
