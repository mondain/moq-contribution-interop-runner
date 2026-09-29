#include "moq/interop/wire/draft21/request_ok.h"

#include <array>

namespace moq::interop::wire::draft21 {

bool encode_empty_publish_ok(ByteWriter& output) {
    // draft-ietf-moq-transport-21 section 9.3, Figure 7.
    constexpr std::array<std::byte, 4> frame{
        std::byte{0x07}, std::byte{0x00},
        std::byte{0x01}, std::byte{0x00}};
    return output.append_bytes(frame);
}

}  // namespace moq::interop::wire::draft21
