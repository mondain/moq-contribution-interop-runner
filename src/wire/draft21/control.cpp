#include "moq/interop/wire/draft21/control.h"

#include "moq/interop/wire/draft21/message_types.h"

#include <variant>

namespace moq::interop::wire::draft21 {

DecodeResult<ControlMessage> decode_control_message(
    Cursor& input, bool first_on_stream, bool received_from_client) {
    Cursor probe = input;
    const auto type = decode_message_type(probe, StreamRole::Control,
                                          first_on_stream);
    if (const auto* need = std::get_if<NeedMore>(&type)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type)) return *error;

    Cursor working = input;
    if (std::get<MessageTypeInfo>(type).kind == MessageKind::Setup) {
        const auto decoded = decode_setup(working);
        if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
        input = working;
        return ControlMessage{std::get<SetupMessage>(decoded)};
    }
    if (std::get<MessageTypeInfo>(type).kind == MessageKind::Goaway) {
        const auto decoded = decode_goaway(working, received_from_client);
        if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
        input = working;
        return ControlMessage{std::get<GoawayMessage>(decoded)};
    }
    return DecodeError{DecodeErrorCode::ProtocolViolation, input.offset(),
                       "unsupported draft-21 control message"};
}

}  // namespace moq::interop::wire::draft21
