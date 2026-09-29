#include "moq/interop/wire/draft21/publisher_request.h"

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

TEST(Draft21PublisherRequest, DispatchesPublishStarter) {
    // draft-ietf-moq-transport-21 sections 6.3 and 9.8.
    const auto wire = bytes({0x1d, 0x00, 0x0f,
                             0x00, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                             0x04, 't', 'e', 's', 't', 0x02, 0x00});
    Cursor input(wire);
    const auto decoded = decode_publisher_request_message(input, true);
    ASSERT_TRUE(std::holds_alternative<PublisherRequestMessage>(decoded));
    const auto& message = std::get<PublisherRequestMessage>(decoded);
    ASSERT_TRUE(std::holds_alternative<PublishMessage>(message));
    EXPECT_EQ(std::get<PublishMessage>(message).request_id, 0u);
    EXPECT_EQ(input.offset(), wire.size());
}

TEST(Draft21PublisherRequest, DispatchesRequestErrorResponse) {
    // draft-ietf-moq-transport-21 sections 6.4.2 and 9.4.
    const auto wire = bytes({0x05, 0x00, 0x03, 0x33, 0x00, 0x00});
    Cursor input(wire);
    const auto decoded = decode_publisher_request_message(input, false);
    ASSERT_TRUE(std::holds_alternative<PublisherRequestMessage>(decoded));
    EXPECT_TRUE(std::holds_alternative<RequestErrorMessage>(
        std::get<PublisherRequestMessage>(decoded)));
    EXPECT_EQ(input.offset(), wire.size());
}

TEST(Draft21PublisherRequest, RejectsUnsupportedAndWrongPosition) {
    // draft-ietf-moq-transport-21 section 9, Table 5.
    for (const auto wire : {
             bytes({0x1e, 0x00, 0x00}),
             bytes({0x03, 0x00, 0x00}),
             bytes({0x1d, 0x00, 0x00})}) {
        Cursor input(wire);
        EXPECT_TRUE(std::holds_alternative<DecodeError>(
            decode_publisher_request_message(input, true)));
        EXPECT_EQ(input.offset(), 0u);
    }
    const auto error = bytes({0x05, 0x00, 0x03, 0x33, 0x00, 0x00});
    Cursor first(error);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(
        decode_publisher_request_message(first, true)));
    EXPECT_EQ(first.offset(), 0u);
}

TEST(Draft21PublisherRequest, PartialFrameIsTransactional) {
    const auto wire = bytes({0x1d, 0x00, 0x0f, 0x00});
    Cursor input(wire, 40);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(
        decode_publisher_request_message(input, true)));
    EXPECT_EQ(input.offset(), 40u);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
