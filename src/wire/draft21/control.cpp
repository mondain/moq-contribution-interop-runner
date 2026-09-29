#include "moq/interop/wire/draft21/control.h"

#include "moq/interop/wire/draft21/message_types.h"
#include "moq/interop/wire/draft21/token.h"

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
        const auto& setup = std::get<SetupMessage>(decoded);
        if (received_from_client) {
            for (const auto& option : setup.options) {
                if (option.type != 3) continue;
                const auto& encoded = std::get<std::vector<std::byte>>(option.value);
                const auto token = decode_token(encoded);
                if (const auto* error = std::get_if<DecodeError>(&token)) {
                    return *error;
                }
                if (const auto* value = std::get_if<Token>(&token);
                    value && (value->alias_type == TokenAliasType::Delete ||
                              value->alias_type == TokenAliasType::UseAlias)) {
                    return DecodeError{DecodeErrorCode::ProtocolViolation,
                                       input.offset(),
                                       "client SETUP Token alias type is forbidden"};
                }
            }
        }
        input = working;
        return ControlMessage{setup};
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
