#include "moq/interop/wire/draft22/publisher_request.h"

#include <variant>

namespace moq::interop::wire::draft22 {

DecodeResult<PublisherRequestMessage> decode_publisher_request_message(
    Cursor& input, bool first_on_stream, bool received_from_client,
    bool namespace_scoped_request) {
    Cursor probe = input;
    const auto type = decode_message_type(probe, StreamRole::Request, first_on_stream);
    if (const auto* need = std::get_if<NeedMore>(&type)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type)) return *error;

    Cursor working = input;
    switch (std::get<MessageTypeInfo>(type).kind) {
    case MessageKind::Publish: {
        const auto decoded = decode_publish(working);
        if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
        input = working;
        return PublisherRequestMessage{std::get<PublishMessage>(decoded)};
    }
    case MessageKind::RequestError: {
        const auto decoded =
            decode_request_error(working, received_from_client, namespace_scoped_request);
        if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
        input = working;
        return PublisherRequestMessage{std::get<RequestErrorMessage>(decoded)};
    }
    default:
        return DecodeError{DecodeErrorCode::ProtocolViolation, input.offset(),
                           "unsupported draft-22 publisher request message"};
    }
}

}  // namespace moq::interop::wire::draft22
