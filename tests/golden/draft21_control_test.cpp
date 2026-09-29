#include "moq/interop/wire/draft21/control.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

TEST(Draft21Control, ParsesSetupThenGoawayOnOneStream) {
    // draft-ietf-moq-transport-21 sections 6.3, 9.1, and 9.2.
    const auto wire = bytes({0xaf, 0x00, 0x00, 0x00,
                             0x10, 0x00, 0x02, 0x00, 0x05});
    Cursor input(wire);
    const auto setup = decode_control_message(input, true, true);
    ASSERT_TRUE(std::holds_alternative<ControlMessage>(setup));
    EXPECT_TRUE(std::holds_alternative<SetupMessage>(
        std::get<ControlMessage>(setup)));
    EXPECT_EQ(input.offset(), 4u);
    const auto goaway = decode_control_message(input, false, true);
    ASSERT_TRUE(std::holds_alternative<ControlMessage>(goaway));
    EXPECT_TRUE(std::holds_alternative<GoawayMessage>(
        std::get<ControlMessage>(goaway)));
    EXPECT_EQ(input.offset(), wire.size());
}

TEST(Draft21Control, RejectsWrongPositionAndRequestType) {
    // draft-ietf-moq-transport-21 section 9, Table 5.
    for (const auto& item : {
             bytes({0x10, 0x00, 0x02, 0x00, 0x05}),
             bytes({0x03, 0x00, 0x00}),
             bytes({0x1e, 0x00, 0x00})}) {
        Cursor input(item);
        EXPECT_TRUE(std::holds_alternative<DecodeError>(
            decode_control_message(input, true, true)));
        EXPECT_EQ(input.offset(), 0u);
    }
    const auto setup = bytes({0xaf, 0x00, 0x00, 0x00});
    Cursor late(setup);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(
        decode_control_message(late, false, true)));
    EXPECT_EQ(late.offset(), 0u);
}

TEST(Draft21Control, PartialFrameDoesNotAdvanceInput) {
    const auto wire = bytes({0xaf, 0x00, 0x00});
    Cursor input(wire, 20);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(
        decode_control_message(input, true, true)));
    EXPECT_EQ(input.offset(), 20u);
}

TEST(Draft21Control, RejectsClientSetupDeleteAndUseAliasTokens) {
    // draft-ietf-moq-transport-21 section 9.1.4 forbids a client
    // DELETE or USE_ALIAS Token in SETUP as received by the server.
    for (const auto alias_type : {0x00u, 0x02u}) {
        const auto wire = bytes({0xaf, 0x00, 0x00, 0x04,
                                 0x03, 0x02, alias_type, 0x07});
        Cursor input(wire);
        const auto decoded = decode_control_message(input, true, true);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
        EXPECT_EQ(std::get<DecodeError>(decoded).code,
                  DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

}  // namespace
}  // namespace moq::interop::wire::draft21
