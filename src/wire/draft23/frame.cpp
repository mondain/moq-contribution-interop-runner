#include "moq/interop/wire/draft23/frame.h"

#include <limits>

namespace moq::interop::wire::draft23 {

DecodeResult<Frame> decode_frame(Cursor& input, std::uint64_t maximum_body_size) {
    Cursor working = input;
    const auto type = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&type)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type)) return *error;
    const auto length = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&length)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length)) return *error;
    const auto size = std::get<std::uint64_t>(length);
    if (size > maximum_body_size)
        return DecodeError{DecodeErrorCode::LengthExceedsLimit, working.offset(), "control message exceeds maximum size"};
    if (size > std::numeric_limits<std::size_t>::max())
        return DecodeError{DecodeErrorCode::LengthNotRepresentable, working.offset(), "control message length is not representable"};
    const auto body = read_bytes(working, static_cast<std::size_t>(size));
    if (const auto* need = std::get_if<NeedMore>(&body)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&body)) return *error;
    const auto bytes = std::get<std::span<const std::byte>>(body);
    Frame result{std::get<std::uint64_t>(type), {bytes.begin(), bytes.end()}};
    input = working;
    return result;
}

}  // namespace moq::interop::wire::draft23
