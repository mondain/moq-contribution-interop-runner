#include "moq/interop/wire/draft21/request_ok.h"
#include "moq/interop/wire/draft21/request_frame.h"

#include <gtest/gtest.h>

#include <algorithm>
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

TEST(Draft21RequestOk, EmptyPublishResponseHasExactDraft21Bytes) {
    // draft-ietf-moq-transport-21 section 9.3, Figure 7: REQUEST_OK
    // replaces the reserved legacy PUBLISH_OK type 0x1e.
    const auto wire = bytes({0x07, 0x00, 0x01, 0x00});
    ByteWriter output(8);
    ASSERT_TRUE(encode_empty_publish_ok(output));
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
    Cursor input(wire);
    const auto decoded = decode_request_frame(input, false);
    ASSERT_TRUE(std::holds_alternative<RequestFrame>(decoded));
    EXPECT_EQ(std::get<RequestFrame>(decoded).type.kind, MessageKind::RequestOk);
    EXPECT_EQ(std::get<RequestFrame>(decoded).body, bytes({0x00}));
    EXPECT_EQ(input.offset(), wire.size());
}

TEST(Draft21RequestOk, FullOutputIsTransactional) {
    ByteWriter output(3);
    EXPECT_FALSE(encode_empty_publish_ok(output));
    EXPECT_EQ(output.size(), 0u);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
