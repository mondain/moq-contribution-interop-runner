#include "session/draft18_dispatch_internal.h"

#include "moq/interop/wire/cursor.h"

namespace moq::interop::session::detail {

StreamTypeResult classify_unidirectional_stream(
    std::span<const std::byte> bytes) {
    wire::Cursor cursor(bytes);
    const auto decoded = wire::read_vi64(cursor);
    if (std::holds_alternative<wire::NeedMore>(decoded)) {
        StreamTypeResult result;
        result.need_more = true;
        return result;
    }
    if (!std::holds_alternative<std::uint64_t>(decoded)) return {};
    const auto type = std::get<std::uint64_t>(decoded);
    StreamTypeResult result;
    result.raw_type = type;
    result.consumed = cursor.offset();
    if (type == 0x2f00) {
        result.kind = PeerStreamKind::Control;
    } else if (type == 0x05) {
        result.kind = PeerStreamKind::Fetch;
    } else if (type == 0x132b3e28) {
        result.kind = PeerStreamKind::Padding;
    } else if (type < 0x80u && (type & 0x10u) != 0u) {
        result.kind = PeerStreamKind::Subgroup;
    }
    return result;
}

bool is_known_nonrepeatable_setup_option(std::uint64_t type) noexcept {
    switch (type) {
        case 0x01:
        case 0x04:
        case 0x05:
        case 0x07:
            return true;
        default:
            return false;
    }
}

}  // namespace moq::interop::session::detail
