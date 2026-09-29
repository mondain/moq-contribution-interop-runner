#include <gtest/gtest.h>

extern "C" {
#include <h3zero.h>
}

#include <array>
#include <cstdint>
#include <cstring>

TEST(WebTransportScheme, DecodedConnectRetainsHttps) {
    std::array<uint8_t, 512> encoded{};
    const auto* const path = reinterpret_cast<const uint8_t*>("/moq");
    auto* end = h3zero_create_connect_header_frame(
        encoded.data(), encoded.data() + encoded.size(), "runner.test:4433",
        path, 4, "webtransport-h3", "https://publisher.test", nullptr,
        "\"moqt-21\"");
    ASSERT_NE(end, nullptr);
    h3zero_header_parts_t parts{};
    EXPECT_EQ(h3zero_parse_qpack_header_frame(encoded.data(), end, &parts), end);
    EXPECT_TRUE(parts.scheme_present);
    EXPECT_TRUE(parts.scheme_https);
    h3zero_release_header_parts(&parts);
}

TEST(WebTransportScheme, LiteralWrongSchemeIsRetainedAsInvalid) {
    std::array<uint8_t, 64> encoded{};
    encoded[0] = 0;
    encoded[1] = 0;
    const auto* value = reinterpret_cast<const uint8_t*>("http");
    auto* end = h3zero_qpack_literal_plus_ref_encode(
        encoded.data() + 2, encoded.data() + encoded.size(),
        H3ZERO_QPACK_SCHEME_HTTPS, value, 4);
    ASSERT_NE(end, nullptr);
    h3zero_header_parts_t parts{};
    EXPECT_EQ(h3zero_parse_qpack_header_frame(encoded.data(), end, &parts), end);
    EXPECT_TRUE(parts.scheme_present);
    EXPECT_FALSE(parts.scheme_https);
    h3zero_release_header_parts(&parts);
}

TEST(WebTransportScheme, DuplicateSchemeIsRejected) {
    std::array<uint8_t, 512> encoded{};
    const auto* const path = reinterpret_cast<const uint8_t*>("/moq");
    auto* end = h3zero_create_connect_header_frame(
        encoded.data(), encoded.data() + encoded.size(), "runner.test:4433",
        path, 4, "webtransport-h3", nullptr, nullptr, "\"moqt-21\"");
    ASSERT_NE(end, nullptr);
    end = h3zero_qpack_code_encode(end, encoded.data() + encoded.size(),
                                   0xc0, 0x3f, H3ZERO_QPACK_SCHEME_HTTPS);
    ASSERT_NE(end, nullptr);
    h3zero_header_parts_t parts{};
    EXPECT_EQ(h3zero_parse_qpack_header_frame(encoded.data(), end, &parts), nullptr);
    h3zero_release_header_parts(&parts);
}
