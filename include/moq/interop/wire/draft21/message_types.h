#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstdint>
#include <optional>

namespace moq::interop::wire::draft21 {

enum class StreamRole { Control, Request };

enum class MessageKind {
    Setup,
    Goaway,
    Subscribe,
    SubscribeOk,
    PublishStateNotify,
    Publish,
    PublishDone,
    Fetch,
    FetchOk,
    TrackStatus,
    PublishNamespace,
    SubscribeNamespace,
    SubscribeTracks,
    Namespace,
    NamespaceDone,
    PublishSkipped,
    RequestUpdate,
    RequestOk,
    RequestError,
};

struct MessageTypeInfo {
    std::uint64_t type;
    MessageKind kind;
    bool control;
    bool request;
    bool request_starter;
};

std::optional<MessageTypeInfo> classify_message_type(std::uint64_t type);
DecodeResult<MessageTypeInfo> decode_message_type(
    Cursor& input, StreamRole role, bool first_on_stream);

}  // namespace moq::interop::wire::draft21
