#include "moq/interop/wire/draft21/request_frame.h"

#include <span>
#include <variant>

namespace moq::interop::wire::draft21 {

DecodeResult<RequestFrame> decode_request_frame(Cursor& input,
                                                bool first_on_stream) {
    Cursor working = input;
    const auto type = decode_message_type(working, StreamRole::Request,
                                          first_on_stream);
    if (const auto* need = std::get_if<NeedMore>(&type)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type)) return *error;
    const auto length = read_bytes(working, 2);
    if (const auto* need = std::get_if<NeedMore>(&length)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length)) return *error;
    const auto length_bytes = std::get<std::span<const std::byte>>(length);
    const auto body_length =
        (static_cast<std::size_t>(std::to_integer<unsigned>(length_bytes[0])) << 8u) |
        std::to_integer<unsigned>(length_bytes[1]);
    const auto body = read_bytes(working, body_length);
    if (const auto* need = std::get_if<NeedMore>(&body)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&body)) return *error;
    const auto body_bytes = std::get<std::span<const std::byte>>(body);
    RequestFrame result{std::get<MessageTypeInfo>(type),
                        std::vector<std::byte>(body_bytes.begin(),
                                               body_bytes.end())};
    input = working;
    return result;
}

}  // namespace moq::interop::wire::draft21
