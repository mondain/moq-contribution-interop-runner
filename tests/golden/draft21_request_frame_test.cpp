#include "moq/interop/wire/draft21/request_frame.h"

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

TEST(Draft21RequestFrame, ParsesFirstPublishAndSubsequentResponse) {
    // draft-ietf-moq-transport-21 sections 6.3 and 9, Table 5.
    const auto wire = bytes({0x1d, 0x00, 0x02, 0x11, 0x22,
                             0x07, 0x00, 0x01, 0x00});
    Cursor input(wire);
    const auto publish = decode_request_frame(input, true);
    ASSERT_TRUE(std::holds_alternative<RequestFrame>(publish));
    EXPECT_EQ(std::get<RequestFrame>(publish).type.kind, MessageKind::Publish);
    EXPECT_EQ(std::get<RequestFrame>(publish).body, bytes({0x11, 0x22}));
    EXPECT_EQ(input.offset(), 5u);
    const auto response = decode_request_frame(input, false);
    ASSERT_TRUE(std::holds_alternative<RequestFrame>(response));
    EXPECT_EQ(std::get<RequestFrame>(response).type.kind, MessageKind::RequestOk);
    EXPECT_EQ(std::get<RequestFrame>(response).body, bytes({0x00}));
    EXPECT_EQ(input.offset(), wire.size());
}

TEST(Draft21RequestFrame, RejectsReservedControlAndInvalidFirstTypes) {
    // draft-ietf-moq-transport-21 section 9 reserves 0x1e.
    for (const auto type : {0x1eu, 0x10u, 0x07u, 0x2fu}) {
        const auto wire = bytes({type, 0x00, 0x00});
        Cursor input(wire);
        EXPECT_TRUE(std::holds_alternative<DecodeError>(
            decode_request_frame(input, true))) << type;
        EXPECT_EQ(input.offset(), 0u);
    }
    const auto publish = bytes({0x1d, 0x00, 0x00});
    Cursor late(publish);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(
        decode_request_frame(late, false)));
    EXPECT_EQ(late.offset(), 0u);
}

TEST(Draft21RequestFrame, IncompleteBodyDoesNotAdvanceInput) {
    const auto wire = bytes({0x1d, 0x00, 0x02, 0x11});
    Cursor input(wire, 100);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(
        decode_request_frame(input, true)));
    EXPECT_EQ(input.offset(), 100u);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
