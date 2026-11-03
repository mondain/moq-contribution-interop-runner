#include "moq/interop/wire/draft23/setup.h"
#include "moq/interop/wire/draft23/frame.h"

namespace moq::interop::wire::draft23 {

DecodeResult<SetupMessage> decode_setup(Cursor& input) {
    Cursor working = input;
    const auto frame = decode_frame(working, 65535);
    if (const auto* need = std::get_if<NeedMore>(&frame)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&frame)) return *error;
    const auto& message = std::get<Frame>(frame);
    if (message.type != 0x2f00)
        return DecodeError{DecodeErrorCode::ProtocolViolation, input.offset(), "expected SETUP"};
    Cursor payload(message.body);
    auto parsed = draft21::decode_key_values_to_end(payload);
    if (const auto* error = std::get_if<DecodeError>(&parsed)) return *error;
    if (const auto* need = std::get_if<NeedMore>(&parsed)) return *need;
    SetupMessage setup{std::get<draft21::KeyValues>(std::move(parsed))};
    bool seen = false;
    for (const auto& option : setup.options) {
        if (option.type != 9) continue;
        if (seen) return DecodeError{DecodeErrorCode::InvalidValue, input.offset(), "duplicate MAX_CONTROL_MESSAGE_SIZE"};
        seen = true;
        const auto& bytes = std::get<std::vector<std::byte>>(option.value);
        Cursor value(bytes);
        const auto decoded = read_vi64(value);
        const auto* limit = std::get_if<std::uint64_t>(&decoded);
        if (!limit || value.remaining() != 0 || *limit < 65535)
            return DecodeError{DecodeErrorCode::InvalidValue, input.offset(), "invalid MAX_CONTROL_MESSAGE_SIZE"};
        setup.maximum_control_message_size = *limit;
    }
    input = working;
    return setup;
}

}  // namespace moq::interop::wire::draft23
