#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft18CatalogPart01Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(18, "draft18-lines-0001-1254.json"));
    }
};

TEST_F(Draft18CatalogPart01Test, CoversAllOwnedOccurrencesWithoutConflictsOrForeignAnchors) {
    expect_partition_coverage(1, 1254, 62);
}

TEST_F(Draft18CatalogPart01Test, QuotedBcp14VocabularyHasNoRuntimeObligations) {
    unsigned count = 0;
    for (const auto& occurrence : scan_normative_occurrences(source_)) {
        if (occurrence.first_line < 412 || occurrence.first_line > 414) continue;
        ++count;
        ASSERT_TRUE(occurrence.quoted_bcp14_vocabulary);
        const auto rows = at(occurrence.first_line, occurrence.occurrence_on_line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Informative);
        EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
        EXPECT_TRUE(rows.front()->scenarios.empty());
        EXPECT_TRUE(rows.front()->evaluators.empty());
        EXPECT_NE(rows.front()->rationale.find("BCP 14"), std::string::npos);
    }
    EXPECT_EQ(count, 11u);
}

TEST_F(Draft18CatalogPart01Test, WrappedProhibitionsDistinguishNameParsingFromObjectImmutability) {
    const auto names = at(737);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names.front()->strength, Strength::MustNot);
    EXPECT_GE(names.front()->source.last_line, 738u);
    EXPECT_EQ(names.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(names.front()->testability, Testability::NotTestable);
    const auto objects = at(797);
    ASSERT_EQ(objects.size(), 1u);
    EXPECT_EQ(objects.front()->strength, Strength::MustNot);
    EXPECT_GE(objects.front()->source.last_line, 798u);
    EXPECT_EQ(objects.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(objects.front()->testability, Testability::Testable);
    EXPECT_EQ(objects.front()->evaluators, std::vector<std::string>{"object-payload-immutable"});
}

TEST_F(Draft18CatalogPart01Test, EndpointReceiveValidationAppliesToPublishingEndpoints) {
    const auto rows = at(632);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->actor, "endpoint");
    EXPECT_EQ(rows.front()->strength, Strength::Must);
    EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(rows.front()->testability, Testability::Testable);
    EXPECT_EQ(rows.front()->scenarios, std::vector<std::string>{"receive-key-value-type-overflow"});
    EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"session-closed-protocol-violation"});
}

TEST_F(Draft18CatalogPart01Test, FirstObjectRuleSeparatesOriginalPublisherFromRelay) {
    const auto publisher = at(901);
    ASSERT_EQ(publisher.size(), 1u);
    EXPECT_EQ(publisher.front()->actor, "original-publisher");
    EXPECT_EQ(publisher.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(publisher.front()->testability, Testability::Testable);
    EXPECT_EQ(publisher.front()->evaluators, std::vector<std::string>{"new-subgroup-first-object-bit-set"});
    const auto relay = at(905);
    ASSERT_EQ(relay.size(), 1u);
    EXPECT_EQ(relay.front()->actor, "relay");
    EXPECT_EQ(relay.front()->applicability, Applicability::NotApplicable);
    EXPECT_EQ(relay.front()->testability, Testability::NotApplicable);
}

TEST_F(Draft18CatalogPart01Test, SubscriberRangeAdviceDoesNotConstrainPublisherBehavior) {
    const auto rows = at(933);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->actor, "subscriber");
    EXPECT_EQ(rows.front()->strength, Strength::ShouldNot);
    EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
}

TEST_F(Draft18CatalogPart01Test, OptionalSubgroupIdReuseRemainsAnObservableCapability) {
    const auto rows = at(875);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->actor, "original-publisher");
    EXPECT_EQ(rows.front()->strength, Strength::May);
    EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(rows.front()->testability, Testability::Testable);
    EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"subgroup-id-reused-across-groups"});
}

TEST_F(Draft18CatalogPart01Test, UnsupportedMandatoryPropertyAdviceHasThreeIndependentPublisherActions) {
    const auto rows = at(1245);
    ASSERT_EQ(rows.size(), 3u);
    const std::vector<std::string> evaluators = {
        "subscribe-rejected-unsupported-extension", "fetch-rejected-unsupported-extension",
        "unsupported-track-not-published"};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        EXPECT_EQ(rows[index]->source.clause, index + 1);
        EXPECT_EQ(rows[index]->actor, "publisher");
        EXPECT_EQ(rows[index]->strength, Strength::Should);
        EXPECT_EQ(rows[index]->applicability, Applicability::Applicable);
        EXPECT_EQ(rows[index]->testability, Testability::Testable);
        EXPECT_EQ(rows[index]->evaluators, std::vector<std::string>{evaluators[index]});
    }
}

}  // namespace
}  // namespace moq::interop::requirements
