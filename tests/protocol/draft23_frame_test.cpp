#include "moq/interop/wire/draft23/frame.h"
#include <gtest/gtest.h>
#include <vector>
using namespace moq::interop::wire;
using namespace moq::interop::wire::draft23;
TEST(Draft23Frame, ReadsVariableLengthAndLeavesFollowingMessage) {
    std::vector<std::byte> data{std::byte{1}, std::byte{0x80}, std::byte{0x80}};
    data.insert(data.end(), 128, std::byte{0xaa});
    data.push_back(std::byte{0xff});
    Cursor cursor(data);
    auto result = decode_frame(cursor, 65535);
    ASSERT_TRUE(std::holds_alternative<Frame>(result));
    EXPECT_EQ(std::get<Frame>(result).body.size(), 128u);
    EXPECT_EQ(cursor.remaining(), 1u);
}
TEST(Draft23Frame, RejectsOversizeBeforeBodyArrivesWithoutAdvancing) {
    std::vector<std::byte> data{std::byte{1}, std::byte{0xc1}, std::byte{0}, std::byte{0}};
    Cursor cursor(data);
    auto result = decode_frame(cursor, 65535);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(cursor.offset(), 0u);
}
TEST(Draft23Frame, IncompleteBodyDoesNotAdvance) {
    std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{0}};
    Cursor cursor(data);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_frame(cursor, 65535)));
    EXPECT_EQ(cursor.offset(), 0u);
}

#include "moq/interop/wire/draft23/setup.h"
TEST(Draft23Setup, ReadsMaximumControlMessageSize) {
    // SETUP with option 9 containing vi64 65535.
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{5},
        std::byte{9}, std::byte{3}, std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}};
    Cursor cursor(data);
    auto result = decode_setup(cursor);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    EXPECT_EQ(std::get<SetupMessage>(result).maximum_control_message_size, 65535u);
}
TEST(Draft23Setup, RejectsMaximumBelowMinimum) {
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{3},
        std::byte{9}, std::byte{1}, std::byte{63}};
    Cursor cursor(data);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_setup(cursor)));
    EXPECT_EQ(cursor.offset(), 0u);
}
TEST(Draft23Setup, UsesDefaultWhenOptionAbsent) {
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{0}};
    Cursor cursor(data);
    auto result = decode_setup(cursor);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    EXPECT_EQ(std::get<SetupMessage>(result).maximum_control_message_size, 65535u);
}
TEST(Draft23Setup, RejectsDuplicateMaximum) {
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{10},
        std::byte{9}, std::byte{3}, std::byte{0xc0}, std::byte{0xff}, std::byte{0xff},
        std::byte{0}, std::byte{3}, std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}};
    Cursor cursor(data);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_setup(cursor)));
    EXPECT_EQ(cursor.offset(), 0u);
}
