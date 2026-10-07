// Cross-check of the moq-lite-06 codecs against vectors extracted from the moq.dev implementation.
//
// The vectors live in tests/golden/data/moqlite06_moqdev_vectors.txt (provenance in its header). The moq.dev
// checkout is read-only reference: when present, the test also confirms that each cited source line still
// contains the literal bytes; when absent that one test skips and the rest still run from the data file.
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "moq/interop/wire/moqlite06/varint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace moq::interop::wire::moqlite06 {
namespace {

using Bytes = std::vector<std::byte>;

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;
const std::filesystem::path kVectorFile = kRoot / "tests/golden/data/moqlite06_moqdev_vectors.txt";
const std::filesystem::path kMoqDevCheckout = "/media/mondain/terrorbyte/workspace/github-moq/moq";

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(text);
    while (std::getline(stream, part, separator)) parts.push_back(trim(part));
    return parts;
}

struct Vector {
    std::string name;
    std::string message;
    std::vector<unsigned> hex;
    std::string source;
    std::map<std::string, std::string> fields;
    bool derived = false;

    Bytes bytes() const {
        Bytes out;
        for (const auto value : hex) out.push_back(static_cast<std::byte>(value));
        return out;
    }
};

std::vector<Vector> load_vectors() {
    std::ifstream input(kVectorFile);
    std::vector<Vector> vectors;
    for (std::string line; std::getline(input, line);) {
        if (trim(line).empty() || trim(line)[0] == '#') continue;
        const auto columns = split(line, '|');
        if (columns.size() != 5) {
            ADD_FAILURE() << "malformed vector line: " << line;
            continue;
        }
        Vector vector{columns[0], columns[1], {}, columns[3], {}, false};
        std::istringstream hex(columns[2]);
        for (std::string token; hex >> token;) vector.hex.push_back(std::stoul(token, nullptr, 16));
        for (const auto& pair : split(columns[4], ';')) {
            const auto eq = pair.find('=');
            vector.fields[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        vector.derived = vector.fields.contains("derived");
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

std::string hex_of(std::span<const std::byte> data) {
    std::string out;
    static const char digits[] = "0123456789abcdef";
    for (const auto byte : data) {
        out += digits[static_cast<unsigned>(byte) >> 4];
        out += digits[static_cast<unsigned>(byte) & 0xF];
    }
    return out;
}

TEST(MoqLite06CrossCheck, DataFileIsPresentAndCoversTheExtractedLiterals) {
    const auto vectors = load_vectors();
    std::map<std::string, int> per_message;
    for (const auto& vector : vectors) ++per_message[vector.message];
    // 1 SETUP literal, 3 varint literals, 1 derived ANNOUNCE_END (see the data file header).
    EXPECT_EQ(per_message, (std::map<std::string, int>{{"SETUP", 1}, {"VARINT", 3}, {"ANNOUNCE_END", 1}}));
}

TEST(MoqLite06CrossCheck, VarintVectorsDecodeAndReEncode) {
    std::size_t checked = 0;
    for (const auto& vector : load_vectors()) {
        if (vector.message != "VARINT") continue;
        SCOPED_TRACE(vector.name);
        const auto data = vector.bytes();
        Cursor input(data);
        const auto result = read_varint(input);
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(result));
        EXPECT_EQ(std::get<std::uint64_t>(result), std::stoull(vector.fields.at("value")));
        EXPECT_EQ(input.remaining(), 0u);
        ByteWriter writer(16);
        ASSERT_TRUE(write_varint(std::get<std::uint64_t>(result), writer));
        EXPECT_EQ(hex_of(writer.bytes()), hex_of(data));
        ++checked;
    }
    EXPECT_EQ(checked, 3u);
}

TEST(MoqLite06CrossCheck, SetupVectorDecodesAndReEncodes) {
    std::size_t checked = 0;
    for (const auto& vector : load_vectors()) {
        if (vector.message != "SETUP") continue;
        SCOPED_TRACE(vector.name);
        const auto data = vector.bytes();
        Cursor input(data);
        const auto result = decode_setup(input);
        ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
        const auto& setup = std::get<SetupMessage>(result);
        EXPECT_EQ(input.remaining(), 0u);
        EXPECT_EQ(setup.parameters.size(), std::stoull(vector.fields.at("parameter_count")));
        ASSERT_EQ(setup.parameters.size(), 1u);
        EXPECT_EQ(setup.parameters[0].id, kParamCost);
        EXPECT_EQ(hex_of(setup.parameters[0].value), vector.fields.at("parameter.4"));

        const auto capabilities = read_capabilities(setup);
        ASSERT_TRUE(std::holds_alternative<SetupCapabilities>(capabilities));
        const auto& caps = std::get<SetupCapabilities>(capabilities);
        ASSERT_TRUE(caps.cost.has_value());
        EXPECT_EQ(*caps.cost, std::stoull(vector.fields.at("cost")));
        EXPECT_FALSE(caps.probe || caps.path || caps.role || caps.hop_id);

        // Re-encode the raw message and the typed build; both reproduce moq.dev's bytes.
        ByteWriter raw(64);
        ASSERT_FALSE(encode_setup(setup, raw).has_value());
        EXPECT_EQ(hex_of(raw.bytes()), hex_of(data));
        const auto built = build_setup(caps);
        ASSERT_TRUE(built.has_value());
        ByteWriter typed(64);
        ASSERT_FALSE(encode_setup(*built, typed).has_value());
        EXPECT_EQ(hex_of(typed.bytes()), hex_of(data));
        ++checked;
    }
    EXPECT_EQ(checked, 1u);
}

TEST(MoqLite06CrossCheck, AnnounceEndDerivedVectorMatchesTheThreeByteLength) {
    std::size_t checked = 0;
    for (const auto& vector : load_vectors()) {
        if (vector.message != "ANNOUNCE_END") continue;
        SCOPED_TRACE(vector.name);
        EXPECT_TRUE(vector.derived);
        const auto data = vector.bytes();
        // moq.dev announce.rs:829 pins only the length: type byte, size prefix, id varint.
        EXPECT_EQ(data.size(), 3u);
        Cursor input(data);
        const auto result = decode_announce_message(input);
        ASSERT_TRUE(std::holds_alternative<AnnounceMessage>(result));
        const auto& message = std::get<AnnounceMessage>(result);
        ASSERT_TRUE(std::holds_alternative<AnnounceEnd>(message));
        EXPECT_EQ(std::get<AnnounceEnd>(message).announce_id, std::stoull(vector.fields.at("announce_id")));
        ByteWriter writer(16);
        ASSERT_FALSE(encode_announce_message(message, writer).has_value());
        EXPECT_EQ(hex_of(writer.bytes()), hex_of(data));
        ++checked;
    }
    EXPECT_EQ(checked, 1u);
}

// moq.dev subscribe.rs:736-762 (subscribe_drops_the_retired_ordered_byte_on_lite06): a Lite06 SUBSCRIBE with
// no frame bounds is the pre-06 layout minus the retired "ordered" byte, plus two defaulted frame varints, so
// it ends in 00 00. That agrees with the draft 7.9 figure (no ordered field; Frame Start and Frame End last).
TEST(MoqLite06CrossCheck, SubscribeWithoutFrameBoundsEndsInTwoZeroVarints) {
    Subscribe message;
    message.subscribe_id = 1;
    message.broadcast_path = "room";
    message.track_name = "video";
    message.range.subscriber_priority = 3;
    message.range.subscriber_max_age_ms = 250;
    ByteWriter writer(128);
    ASSERT_FALSE(encode_subscribe(message, writer).has_value());
    const auto wire = writer.bytes();
    ASSERT_GE(wire.size(), 3u);
    EXPECT_EQ(wire[wire.size() - 1], std::byte{0});
    EXPECT_EQ(wire[wire.size() - 2], std::byte{0});
    // Priority is one raw byte, directly after the Track Name (no ordered byte in between).
    const std::string track = "video";
    const auto track_end = 1 /*length*/ + 1 /*id*/ + 1 + 4 /*path*/ + 1 + track.size();
    EXPECT_EQ(wire[track_end], std::byte{3});
}

// Not a vector disagreement: moq.dev announce.rs:801 (unknown_announce_type_is_skipped) skips an ANNOUNCE
// message with an unknown Type by its Message Length so an older build survives a newer peer. The draft is
// silent on the receiving rule for the ANNOUNCE stream, and this codec reports an unknown Type as
// InvalidValue (without consuming it) so the caller chooses. Pinned so a change to either is deliberate.
TEST(MoqLite06CrossCheck, UnknownAnnounceTypeIsInvalidValueWhereMoqDevSkips) {
    const Bytes data = {std::byte{4}, std::byte{1}, std::byte{0}};
    Cursor input(data);
    const auto result = decode_announce_message(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::InvalidValue);
    EXPECT_EQ(input.remaining(), data.size());
}

// Every vector's cited source line still contains its literal bytes (skipped without the checkout).
TEST(MoqLite06CrossCheck, CitedMoqDevLinesStillHoldTheLiterals) {
    if (!std::filesystem::exists(kMoqDevCheckout / "rs/moq-net/src/lite/setup.rs")) {
        GTEST_SKIP() << "the moq.dev checkout is absent at " << kMoqDevCheckout.string();
    }
    std::size_t checked = 0;
    for (const auto& vector : load_vectors()) {
        if (vector.derived) continue;
        SCOPED_TRACE(vector.name);
        const auto colon = vector.source.rfind(':');
        ASSERT_NE(colon, std::string::npos);
        std::ifstream source(kMoqDevCheckout / vector.source.substr(0, colon));
        ASSERT_TRUE(source.good()) << vector.source;
        const auto wanted = std::stoul(vector.source.substr(colon + 1));
        std::string line;
        for (unsigned long i = 0; i < wanted && std::getline(source, line);) ++i;
        std::string lowered = line;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto value : vector.hex) {
            char token[8];
            std::snprintf(token, sizeof token, "0x%02x", value);
            EXPECT_NE(lowered.find(token), std::string::npos) << token << " not on " << vector.source;
        }
        ++checked;
    }
    EXPECT_EQ(checked, 4u);
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
