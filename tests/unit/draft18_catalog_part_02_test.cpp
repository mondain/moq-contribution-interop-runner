#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft18CatalogPart02Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(18, "draft18-lines-1255-2564.json"));
    }
};

TEST_F(Draft18CatalogPart02Test, CoversAllOwnedOccurrencesWithoutConflictsOrForeignAnchors) {
    expect_partition_coverage(1255, 2564, 97);
}

TEST_F(Draft18CatalogPart02Test, SetupNegotiationAppliesButSpecificationAuthorDutiesDoNot) {
    for (const auto line : {1265u, 1393u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "endpoint");
        EXPECT_EQ(rows.front()->strength, Strength::Must);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
    }
    for (const auto line : {1410u, 1411u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "specification-author");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft18CatalogPart02Test, WrongStreamTypesHaveDistinctClosureRequirements) {
    const auto request = at(1480);
    ASSERT_EQ(request.size(), 1u);
    EXPECT_EQ(request.front()->strength, Strength::Must);
    EXPECT_EQ(request.front()->evaluators,
              std::vector<std::string>{"session-closed-protocol-violation"});
    const auto unknown = at(1657);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown.front()->evaluators, std::vector<std::string>{"session-closed"});
}

TEST_F(Draft18CatalogPart02Test, SubscriptionResponsesDistinguishPublisherAndSubscriberRoles) {
    const auto publisher = at(1936);
    ASSERT_EQ(publisher.size(), 1u);
    EXPECT_EQ(publisher.front()->actor, "publisher");
    EXPECT_EQ(publisher.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(publisher.front()->evaluators,
              std::vector<std::string>{"exactly-one-subscribe-ok-or-request-error"});
    const auto subscriber = at(1937);
    ASSERT_EQ(subscriber.size(), 1u);
    EXPECT_EQ(subscriber.front()->actor, "subscriber");
    EXPECT_EQ(subscriber.front()->applicability, Applicability::NotApplicable);
    const auto duplicate = at(1939);
    ASSERT_EQ(duplicate.size(), 1u);
    EXPECT_EQ(duplicate.front()->scenarios,
              std::vector<std::string>{"receive-duplicate-publish-response"});
}

TEST_F(Draft18CatalogPart02Test, JoiningLocationRetentionIsTestedBySubsequentFetchRange) {
    const auto rows = at(1950);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->actor, "publisher");
    EXPECT_EQ(rows.front()->testability, Testability::Testable);
    EXPECT_EQ(rows.front()->scenarios,
              std::vector<std::string>{"joining-fetch-after-forward-enabled-and-track-advanced"});
    EXPECT_EQ(rows.front()->evaluators,
              std::vector<std::string>{"joining-fetch-ends-at-saved-joining-location"});
}

TEST_F(Draft18CatalogPart02Test, FiltersProhibitOutOfRangeObjectsAndRejectOverflow) {
    const auto range = at(2103);
    ASSERT_EQ(range.size(), 1u);
    EXPECT_EQ(range.front()->strength, Strength::MustNot);
    EXPECT_EQ(range.front()->evaluators,
              std::vector<std::string>{"all-delivered-objects-within-subscription-range"});
    const auto overflow = at(2094);
    ASSERT_EQ(overflow.size(), 1u);
    EXPECT_EQ(overflow.front()->scenarios,
              std::vector<std::string>{"absolute-range-end-group-overflow"});
    EXPECT_EQ(overflow.front()->evaluators,
              std::vector<std::string>{"session-closed-protocol-violation"});
}

TEST_F(Draft18CatalogPart02Test, FetchCancellationSeparatesBothPublisherStreamResets) {
    const auto rows = at(2170);
    ASSERT_EQ(rows.size(), 2u);
    const std::vector<std::string> evaluators = {
        "fetch-request-stream-reset", "fetch-data-stream-reset"};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        EXPECT_EQ(rows[index]->source.clause, index + 1);
        EXPECT_EQ(rows[index]->actor, "publisher");
        EXPECT_EQ(rows[index]->strength, Strength::Must);
        EXPECT_EQ(rows[index]->testability, Testability::Testable);
        EXPECT_EQ(rows[index]->evaluators, std::vector<std::string>{evaluators[index]});
    }
    const auto subscriber = at(2166);
    ASSERT_EQ(subscriber.size(), 1u);
    EXPECT_EQ(subscriber.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft18CatalogPart02Test, NamespacePublicationSeparatesResponseSenderAndDuplicateReceiver) {
    const auto response = at(2275);
    ASSERT_EQ(response.size(), 2u);
    for (const auto* row : response) {
        EXPECT_EQ(row->actor, "subscriber");
        EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    }
    const auto duplicate = at(2277);
    ASSERT_EQ(duplicate.size(), 1u);
    EXPECT_EQ(duplicate.front()->actor, "publisher");
    EXPECT_EQ(duplicate.front()->strength, Strength::Should);
    EXPECT_EQ(duplicate.front()->scenarios,
              std::vector<std::string>{"receive-duplicate-publish-namespace-response"});
    EXPECT_EQ(duplicate.front()->evaluators,
              std::vector<std::string>{"session-closed-protocol-violation"});
}

TEST_F(Draft18CatalogPart02Test, SchedulingAdviceDoesNotInventAnExternallyGuaranteedWireOrder) {
    const auto rows = at(2379);
    ASSERT_EQ(rows.size(), 5u);
    for (const auto* row : rows) {
        EXPECT_EQ(row->actor, "publisher");
        EXPECT_EQ(row->strength, Strength::Should);
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
        EXPECT_TRUE(row->scenarios.empty());
        EXPECT_TRUE(row->evaluators.empty());
    }
}

TEST_F(Draft18CatalogPart02Test, DeliveryTimeoutsSeparateInternalTrackingFromWireOutcomes) {
    for (const auto line : {2486u, 2491u, 2498u, 2500u, 2507u, 2513u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto expired_subgroup = at(2494);
    ASSERT_EQ(expired_subgroup.size(), 1u);
    EXPECT_EQ(expired_subgroup.front()->evaluators,
              std::vector<std::string>{"subgroup-reset-delivery-timeout"});
    // The datagram drop has no wire action: the age runs from an internal
    // application event and a dropped datagram looks like path loss.
    const auto expired_datagram = at(2505);
    ASSERT_EQ(expired_datagram.size(), 1u);
    EXPECT_EQ(expired_datagram.front()->testability, Testability::NotTestable);
    EXPECT_TRUE(expired_datagram.front()->scenarios.empty());
    EXPECT_TRUE(expired_datagram.front()->evaluators.empty());
    const auto uncommitted_subgroup = at(2528);
    ASSERT_EQ(uncommitted_subgroup.size(), 1u);
    EXPECT_EQ(uncommitted_subgroup.front()->evaluators,
              std::vector<std::string>{"uncommitted-subgroup-stream-reset-after-timeout"});
}

TEST_F(Draft18CatalogPart02Test, DefaultPriorityAdviceDoesNotMakeItsExampleAMandatoryValue) {
    const auto rows = at(2469);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->strength, Strength::Should);
    EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    EXPECT_TRUE(rows.front()->scenarios.empty());
    EXPECT_TRUE(rows.front()->evaluators.empty());
}

}  // namespace
}  // namespace moq::interop::requirements
