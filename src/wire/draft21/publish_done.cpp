#include "moq/interop/wire/draft21/publish_done.h"
#include "moq/interop/wire/draft21/request_frame.h"

#include <optional>

namespace moq::interop::wire::draft21 {
namespace {
DecodeError violation(std::size_t offset, const char* detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, detail};
}
template<class T>
std::optional<DecodeError> bounded_error(const DecodeResult<T>& value) {
    if (const auto* need = std::get_if<NeedMore>(&value))
        return violation(need->offset, "truncated PUBLISH_DONE body");
    if (const auto* error = std::get_if<DecodeError>(&value)) {
        if (error->code == DecodeErrorCode::LengthExceedsLimit)
            return violation(error->offset, "PUBLISH_DONE reason phrase exceeds 1024 bytes");
        return *error;
    }
    return std::nullopt;
}
}  // namespace

bool valid_reason_phrase(std::span<const std::byte> reason) {
    if (reason.size() > 1024) return false;
    for (std::size_t index = 0; index < reason.size();) {
        const auto lead = std::to_integer<unsigned>(reason[index]);
        if (lead < 0x80) { ++index; continue; }
        const auto count = lead >= 0xc2 && lead <= 0xdf ? 1u :
                           lead >= 0xe0 && lead <= 0xef ? 2u :
                           lead >= 0xf0 && lead <= 0xf4 ? 3u : 0u;
        if (count == 0 || count > reason.size() - index - 1) return false;
        const auto second = std::to_integer<unsigned>(reason[index + 1]);
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
            (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90))
            return false;
        for (unsigned offset = 1; offset <= count; ++offset) {
            const auto octet = std::to_integer<unsigned>(reason[index + offset]);
            if (octet < 0x80 || octet > 0xbf) return false;
        }
        index += count + 1;
    }
    return true;
}

DecodeResult<PublishDoneMessage> decode_publish_done(Cursor& input) {
    Cursor working = input;
    const auto frame = decode_request_frame(working, false);
    if (const auto* need = std::get_if<NeedMore>(&frame)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&frame)) return *error;
    const auto& decoded = std::get<RequestFrame>(frame);
    if (decoded.type.kind != MessageKind::PublishDone)
        return violation(input.offset(), "expected PUBLISH_DONE");
    Cursor body(decoded.body, working.offset() - decoded.body.size());
    const auto status = read_vi64(body);
    if (const auto error = bounded_error(status)) return *error;
    const auto count = read_vi64(body);
    if (const auto error = bounded_error(count)) return *error;
    const auto reason = read_length_prefixed_bytes(body, 1024);
    if (const auto error = bounded_error(reason)) return *error;
    const auto bytes = std::get<std::span<const std::byte>>(reason);
    if (!valid_reason_phrase(bytes))
        return violation(body.offset() - bytes.size(), "PUBLISH_DONE reason is not UTF-8");
    if (body.remaining() != 0)
        return violation(body.offset(), "PUBLISH_DONE body length mismatch");
    input = working;
    return PublishDoneMessage{std::get<std::uint64_t>(status),
                              std::get<std::uint64_t>(count),
                              {bytes.begin(), bytes.end()}};
}
}  // namespace moq::interop::wire::draft21
