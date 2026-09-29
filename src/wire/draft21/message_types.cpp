#include "moq/interop/wire/draft21/message_types.h"

#include <array>
#include <variant>

namespace moq::interop::wire::draft21 {
namespace {

// draft-ietf-moq-transport-21 section 9, Table 5. Reserved historical
// message types are intentionally absent.
constexpr std::array<MessageTypeInfo, 19> kTypes{
    MessageTypeInfo{0x2f00, MessageKind::Setup, true, false, false},
    MessageTypeInfo{0x10, MessageKind::Goaway, true, true, false},
    MessageTypeInfo{0x03, MessageKind::Subscribe, false, true, true},
    MessageTypeInfo{0x04, MessageKind::SubscribeOk, false, true, false},
    MessageTypeInfo{0x22, MessageKind::PublishStateNotify, false, true, false},
    MessageTypeInfo{0x1d, MessageKind::Publish, false, true, true},
    MessageTypeInfo{0x0b, MessageKind::PublishDone, false, true, false},
    MessageTypeInfo{0x16, MessageKind::Fetch, false, true, true},
    MessageTypeInfo{0x18, MessageKind::FetchOk, false, true, false},
    MessageTypeInfo{0x0d, MessageKind::TrackStatus, false, true, true},
    MessageTypeInfo{0x06, MessageKind::PublishNamespace, false, true, true},
    MessageTypeInfo{0x50, MessageKind::SubscribeNamespace, false, true, true},
    MessageTypeInfo{0x51, MessageKind::SubscribeTracks, false, true, true},
    MessageTypeInfo{0x08, MessageKind::Namespace, false, true, false},
    MessageTypeInfo{0x0e, MessageKind::NamespaceDone, false, true, false},
    MessageTypeInfo{0x0f, MessageKind::PublishSkipped, false, true, false},
    MessageTypeInfo{0x02, MessageKind::RequestUpdate, false, true, false},
    MessageTypeInfo{0x07, MessageKind::RequestOk, false, true, false},
    MessageTypeInfo{0x05, MessageKind::RequestError, false, true, false},
};

}  // namespace

std::optional<MessageTypeInfo> classify_message_type(std::uint64_t type) {
    for (const auto& info : kTypes) {
        if (info.type == type) return info;
    }
    return std::nullopt;
}

DecodeResult<MessageTypeInfo> decode_message_type(
    Cursor& input, StreamRole role, bool first_on_stream) {
    Cursor working = input;
    const auto decoded = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
    const auto info = classify_message_type(std::get<std::uint64_t>(decoded));
    if (!info) {
        return DecodeError{DecodeErrorCode::ProtocolViolation, input.offset(),
                           "unknown or reserved draft-21 message type"};
    }
    const bool valid = role == StreamRole::Control
        ? info->control &&
              (first_on_stream == (info->kind == MessageKind::Setup))
        : info->request &&
              (first_on_stream == info->request_starter);
    if (!valid) {
        return DecodeError{DecodeErrorCode::ProtocolViolation, input.offset(),
                           "draft-21 message is not valid at this stream position"};
    }
    input = working;
    return *info;
}

}  // namespace moq::interop::wire::draft21
