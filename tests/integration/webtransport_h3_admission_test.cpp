#include <gtest/gtest.h>

extern "C" {
#include <h3zero_common.h>
int h3zero_check_connect_protocol(const picohttp_server_path_item_t*, h3zero_stream_ctx_t*);
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace {

bool take_varint(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
    if (p == end) return false;
    const unsigned width = 1u << (*p >> 6u);
    if (static_cast<std::size_t>(end - p) < width) return false;
    value = *p++ & 0x3fu;
    for (unsigned i = 1; i < width; ++i) value = (value << 8u) | *p++;
    return true;
}

TEST(WebTransportH3Admission, DefaultSettingsWireHasOnlyCurrentDraftIdentifier) {
    const uint8_t* p = h3zero_default_setting_frame;
    const uint8_t* const end = p + h3zero_default_setting_frame_size;
    uint64_t stream_type = 0, frame_type = 0, frame_length = 0;
    ASSERT_TRUE(take_varint(p, end, stream_type));
    ASSERT_TRUE(take_varint(p, end, frame_type));
    ASSERT_TRUE(take_varint(p, end, frame_length));
    EXPECT_EQ(stream_type, 0u);
    EXPECT_EQ(frame_type, 4u);
    ASSERT_EQ(static_cast<uint64_t>(end - p), frame_length);
    std::map<uint64_t, uint64_t> settings;
    while (p < end) {
        uint64_t id = 0, value = 0;
        ASSERT_TRUE(take_varint(p, end, id));
        ASSERT_TRUE(take_varint(p, end, value));
        ASSERT_TRUE(settings.emplace(id, value).second);
    }
    EXPECT_EQ(settings.at(0x2c7cf000), 1u);
    EXPECT_EQ(settings.at(0x8), 1u);
    EXPECT_EQ(settings.at(0x33), 1u);
    EXPECT_EQ(settings.count(0x14e9cd29), 0u);
    EXPECT_EQ(settings.count(0xc671706a), 0u);
    EXPECT_EQ(settings.count(0x2b603742), 0u);
}

TEST(WebTransportH3Admission, LegacySettingsNeverEnableDraftWebTransport) {
    for (uint64_t old_id : {0x14e9cd29ULL, 0xc671706aULL, 0x2b603742ULL}) {
        uint8_t encoded[16]{};
        uint8_t* p = encoded;
        const unsigned width = old_id > 0x3fffffff ? 8u : 4u;
        *p++ = static_cast<uint8_t>((width == 8 ? 0xc0 : 0x80) |
                                    ((old_id >> ((width - 1u) * 8u)) & 0x3fu));
        for (unsigned i = width - 1u; i > 0; --i)
            *p++ = static_cast<uint8_t>(old_id >> ((i - 1u) * 8u));
        *p++ = 1;
        h3zero_settings_t settings{};
        ASSERT_EQ(h3zero_settings_components_decode(encoded, p, &settings), p);
        EXPECT_EQ(settings.webtransport_enabled, 0u);
        EXPECT_EQ(settings.webtransport_max_sessions, 0u);
    }
}

TEST(WebTransportH3Admission, LegacyUpgradeTokenRejectedExactly) {
    picohttp_server_path_item_t route{};
    route.connect_protocol = "webtransport-h3";
    route.connect_protocol_length = std::strlen(route.connect_protocol);
    h3zero_stream_ctx_t stream{};
    const char* old = "webtransport";
    stream.ps.stream_state.header.protocol = reinterpret_cast<const uint8_t*>(old);
    stream.ps.stream_state.header.protocol_length = std::strlen(old);
    EXPECT_NE(h3zero_check_connect_protocol(&route, &stream), 0);
    const char* current = "webtransport-h3";
    stream.ps.stream_state.header.protocol = reinterpret_cast<const uint8_t*>(current);
    stream.ps.stream_state.header.protocol_length = std::strlen(current);
    EXPECT_EQ(h3zero_check_connect_protocol(&route, &stream), 0);
}

}  // namespace
