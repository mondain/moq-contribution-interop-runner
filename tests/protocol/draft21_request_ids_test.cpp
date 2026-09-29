#include "moq/interop/session/draft21_request_ids.h"

#include <gtest/gtest.h>

namespace moq::interop::session::draft21 {
namespace {

TEST(Draft21RequestIds, AcceptsOutOfOrderDistinctClientIds) {
    // draft-ietf-moq-transport-21 section 6.4.2.1.
    RequestIds ids(4);
    EXPECT_EQ(ids.observe(0, Initiator::Client), RequestIdResult::Accepted);
    EXPECT_EQ(ids.observe(4, Initiator::Client), RequestIdResult::Accepted);
    EXPECT_EQ(ids.observe(2, Initiator::Client), RequestIdResult::Accepted);
    EXPECT_EQ(ids.size(), 3u);
}

TEST(Draft21RequestIds, RejectsParityAndDuplicateAcrossStreams) {
    RequestIds ids(4);
    EXPECT_EQ(ids.observe(1, Initiator::Client),
              RequestIdResult::InvalidRequestId);
    EXPECT_EQ(ids.size(), 0u);
    EXPECT_EQ(ids.observe(0, Initiator::Client), RequestIdResult::Accepted);
    EXPECT_EQ(ids.observe(0, Initiator::Client),
              RequestIdResult::InvalidRequestId);
    EXPECT_EQ(ids.observe(2, Initiator::Server),
              RequestIdResult::InvalidRequestId);
    EXPECT_EQ(ids.observe(std::uint64_t{1} << 62u, Initiator::Client),
              RequestIdResult::InvalidRequestId);
    EXPECT_EQ(ids.observe(1, Initiator::Server), RequestIdResult::Accepted);
}

TEST(Draft21RequestIds, DistinguishesHarnessCapacityFromPeerViolation) {
    RequestIds ids(2);
    EXPECT_EQ(ids.observe(0, Initiator::Client), RequestIdResult::Accepted);
    EXPECT_EQ(ids.observe(2, Initiator::Client), RequestIdResult::Accepted);
    EXPECT_EQ(ids.observe(4, Initiator::Client), RequestIdResult::HarnessLimit);
    EXPECT_EQ(ids.observe(0, Initiator::Client),
              RequestIdResult::InvalidRequestId);
    EXPECT_EQ(ids.size(), 2u);
}

}  // namespace
}  // namespace moq::interop::session::draft21
