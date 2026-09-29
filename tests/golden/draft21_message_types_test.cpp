#include "moq/interop/wire/draft21/message_types.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

TEST(Draft21MessageTypes, ClassifiesEveryActiveTypeFromTable5) {
    // draft-ietf-moq-transport-21 section 9, Table 5.
    constexpr std::array<std::uint64_t, 19> active{
        0x2f00, 0x10, 0x03, 0x04, 0x22, 0x1d, 0x0b,
        0x16, 0x18, 0x0d, 0x06, 0x50, 0x51, 0x08,
        0x0e, 0x0f, 0x02, 0x07, 0x05};
    for (const auto type : active) {
        const auto info = classify_message_type(type);
        ASSERT_TRUE(info.has_value()) << type;
        EXPECT_EQ(info->type, type);
    }
    EXPECT_EQ(classify_message_type(0x0f)->kind,
              MessageKind::PublishSkipped);
    EXPECT_EQ(classify_message_type(0x22)->kind,
              MessageKind::PublishStateNotify);
}

TEST(Draft21MessageTypes, RejectsEveryReservedLegacyType) {
    // draft-ietf-moq-transport-21 section 9, Table 5 reserves these values.
    for (const auto type : {0x01u, 0x40u, 0x41u, 0x20u, 0x21u, 0x1eu}) {
        EXPECT_FALSE(classify_message_type(type).has_value()) << type;
        const auto wire = bytes({type});
        Cursor input(wire);
        const auto result = decode_message_type(
            input, StreamRole::Request, true);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
        EXPECT_EQ(std::get<DecodeError>(result).code,
                  DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft21MessageTypes, EnforcesControlAndRequestPlacement) {
    // draft-ietf-moq-transport-21 sections 6.3 and 9, Table 5.
    struct Case {
        std::vector<std::byte> encoded;
        StreamRole role;
        bool first;
        bool valid;
    };
    const std::array cases{
        Case{bytes({0xaf, 0x00}), StreamRole::Control, true, true},
        Case{bytes({0xaf, 0x00}), StreamRole::Control, false, false},
        Case{bytes({0x10}), StreamRole::Control, false, true},
        Case{bytes({0x10}), StreamRole::Control, true, false},
        Case{bytes({0x03}), StreamRole::Request, true, true},
        Case{bytes({0x03}), StreamRole::Request, false, false},
        Case{bytes({0x07}), StreamRole::Request, true, false},
        Case{bytes({0x07}), StreamRole::Request, false, true},
        Case{bytes({0x0f}), StreamRole::Request, false, true},
        Case{bytes({0xaf, 0x00}), StreamRole::Request, true, false},
        Case{bytes({0x03}), StreamRole::Control, false, false},
    };
    for (const auto& item : cases) {
        Cursor input(item.encoded);
        const auto result = decode_message_type(input, item.role,
                                                item.first);
        EXPECT_EQ(std::holds_alternative<MessageTypeInfo>(result), item.valid);
        EXPECT_EQ(input.offset(), item.valid ? item.encoded.size() : 0u);
    }
}

TEST(Draft21MessageTypes, PartialVarintDoesNotConsumeInput) {
    const auto wire = bytes({0xaf});
    Cursor input(wire, 42);
    const auto result = decode_message_type(input, StreamRole::Control, true);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(result));
    EXPECT_EQ(input.offset(), 42u);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
