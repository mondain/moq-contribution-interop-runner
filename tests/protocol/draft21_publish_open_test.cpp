#include "moq/interop/session/draft21_publish_open.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <vector>

namespace moq::interop::session::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

std::vector<std::byte> publish(unsigned request_id) {
    // draft-ietf-moq-transport-21 section 9.8, Figure 12.
    return bytes({0x1d, 0x00, 0x0f,
                  request_id, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                  0x04, 't', 'e', 's', 't', 0x02, 0x00});
}

TEST(Draft21PublishOpen, ParsesFragmentedPublishAndValidatesRequestIds) {
    PublishOpenState state;
    const auto first = publish(0);
    EXPECT_FALSE(state.on_client_stream(0, {first.data(), 3}, false)
                     .publish.has_value());
    const auto second = state.on_client_stream(
        0, {first.data() + 3, first.size() - 3}, false);
    ASSERT_TRUE(second.publish.has_value());
    EXPECT_EQ(second.publish->request_id, 0u);
    EXPECT_FALSE(second.close_error.has_value());
    EXPECT_EQ(state.on_client_stream(4, publish(0), false).close_error, 0x4u);
}

TEST(Draft21PublishOpen, AcceptsOutOfOrderStreamsButRejectsOddClientId) {
    PublishOpenState state;
    EXPECT_TRUE(state.on_client_stream(4, publish(4), false).publish.has_value());
    EXPECT_TRUE(state.on_client_stream(0, publish(0), false).publish.has_value());
    EXPECT_EQ(state.on_client_stream(8, publish(1), false).close_error, 0x4u);
}

TEST(Draft21PublishOpen, UnsupportedActiveTypeIsNotConformanceFailure) {
    PublishOpenState state;
    const auto result = state.on_client_stream(
        0, bytes({0x03, 0x00, 0x00}), false);
    EXPECT_TRUE(result.unsupported_message);
    EXPECT_FALSE(result.close_error.has_value());
    EXPECT_FALSE(result.invalid_first_message);
    const auto reserved = state.on_client_stream(
        4, bytes({0x1e, 0x00, 0x00}), false);
    EXPECT_EQ(reserved.close_error, 0x3u);
    EXPECT_TRUE(reserved.invalid_first_message);
}

TEST(Draft21PublishOpen, IncompleteFinIsRequestFailureNotSessionViolation) {
    PublishOpenState state;
    const auto result = state.on_client_stream(0, bytes({0x1d, 0x00, 0x0f}), true);
    EXPECT_TRUE(result.incomplete_request);
    EXPECT_FALSE(result.close_error.has_value());
}

TEST(Draft21PublishOpen, BoundedStateUsesHarnessLimit) {
    PublishOpenState state(1, 4);
    const auto first = state.on_client_stream(0, bytes({0x1d}), false);
    EXPECT_FALSE(first.harness_limit);
    EXPECT_TRUE(state.on_client_stream(4, bytes({0x1d}), false).harness_limit);
    EXPECT_TRUE(state.on_client_stream(0, bytes({0x00, 0x0f, 0x00, 0x01}), false)
                    .harness_limit);
}

}  // namespace
}  // namespace moq::interop::session::draft21
