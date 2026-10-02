#pragma once

#include "moq/interop/wire/cursor.h"

namespace moq::interop::wire::draft21 {
struct PublishDoneMessage {
    std::uint64_t status_code;
    std::uint64_t stream_count;
    std::vector<std::byte> reason;
};

bool valid_reason_phrase(std::span<const std::byte> reason);
DecodeResult<PublishDoneMessage> decode_publish_done(Cursor& input);
}  // namespace moq::interop::wire::draft21
