#include <picoquic.h>
#include <h3zero.h>

#include <array>
#include <cstdint>

int main() {
    const std::array<std::uint8_t, 1> encoded{0x25};
    std::uint64_t decoded = 0;
    if (h3zero_varint_decode(encoded.data(), encoded.size(), &decoded) != 1 ||
        decoded != 37 || picoquic_current_time() == 0) {
        return 1;
    }

    picoquic_quic_t* quic = picoquic_create(
        1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, picoquic_current_time(), nullptr, nullptr, nullptr,
        0);
    if (quic == nullptr) {
        return 2;
    }
    picoquic_free(quic);
    return 0;
}
