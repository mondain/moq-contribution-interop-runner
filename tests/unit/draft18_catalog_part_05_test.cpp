#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft18CatalogPart05Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(18, "draft18-lines-4936-6108.json"));
    }
};

TEST_F(Draft18CatalogPart05Test, CoversAllOwnedOccurrencesWithoutConflictsOrForeignAnchors) {
    expect_partition_coverage(4936, 6108, 78);
    const std::map<Strength, std::string> tokens = {
        {Strength::Must, "MUST"}, {Strength::MustNot, "MUST-NOT"},
        {Strength::Should, "SHOULD"}, {Strength::ShouldNot, "SHOULD-NOT"},
        {Strength::May, "MAY"}};
    for (const auto& row : catalog_.requirements) {
        auto section = row.source.section;
        std::replace(section.begin(), section.end(), '.', '-');
        EXPECT_TRUE(row.id.starts_with("D18-" + section + "-" + tokens.at(row.strength) + "-"))
            << row.id;
    }
}

TEST_F(Draft18CatalogPart05Test, RequirementIdsAreUniqueAcrossAllFivePartitions) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    std::set<std::string> ids;
    for (const auto* filename : {"draft18-lines-0001-1254.json", "draft18-lines-1255-2564.json",
                                "draft18-lines-2565-3508.json", "draft18-lines-3509-4935.json",
                                "draft18-lines-4936-6108.json"}) {
        const auto part = RequirementCatalog::load(
            source_, root / "requirements/parts" / filename, CatalogLoadMode::AllowIncomplete);
        for (const auto& row : part.requirements) {
            EXPECT_TRUE(ids.insert(row.id).second) << row.id;
        }
    }
}

TEST_F(Draft18CatalogPart05Test, AliasUniquenessAppliesToPublishersButCollisionHandlingToSubscribers) {
    const auto sender = at(4962);
    ASSERT_EQ(sender.size(), 1u);
    EXPECT_EQ(sender.front()->strength, Strength::MustNot);
    EXPECT_EQ(sender.front()->actor, "publisher");
    EXPECT_EQ(sender.front()->evaluators,
              std::vector<std::string>{"no-simultaneous-track-alias-reuse-for-distinct-tracks"});
    for (const auto line : {4965u, 4969u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "subscriber");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    EXPECT_NE(at(4965).front()->summary.find("DUPLICATE_TRACK_ALIAS"), std::string::npos);
}

TEST_F(Draft18CatalogPart05Test, ObjectValidationSeparatesOutgoingPayloadFromSubscriberProcessing) {
    const auto payload = at(5057);
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload.front()->strength, Strength::Must);
    EXPECT_EQ(payload.front()->evaluators,
              std::vector<std::string>{"non-normal-object-status-has-empty-payload"});
    for (const auto line : {5053u, 5055u, 5056u, 5063u, 5142u, 5187u, 5197u, 5340u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
    }
    const auto metadata = at(5067);
    ASSERT_EQ(metadata.size(), 1u);
    EXPECT_EQ(metadata.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(metadata.front()->testability, Testability::NotTestable);
}

TEST_F(Draft18CatalogPart05Test, PriorDeliveryEstablishesAnObjectsSubscriptionForwardingPreference) {
    const auto preference = at(5007);
    ASSERT_EQ(preference.size(), 1u);
    EXPECT_EQ(preference.front()->strength, Strength::Must);
    EXPECT_EQ(preference.front()->actor, "publisher");
    EXPECT_EQ(preference.front()->testability, Testability::Testable);
    EXPECT_EQ(preference.front()->evaluators,
              std::vector<std::string>{"subscription-delivery-preserves-observed-object-forwarding-preference"});
    EXPECT_NE(preference.front()->rationale.find("11.3"), std::string::npos);
}

TEST_F(Draft18CatalogPart05Test, WireTypeValidationKeepsDedicatedPaddingAndOtherStreamTypes) {
    const auto unknown = at(4948);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown.front()->evaluators, std::vector<std::string>{"session-closed"});
    for (const auto line : {5176u, 5308u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u);
        for (const auto* row : rows) {
            EXPECT_EQ(row->strength, Strength::Must);
            EXPECT_EQ(row->actor, "endpoint");
            EXPECT_EQ(row->evaluators,
                      std::vector<std::string>{"session-closed-protocol-violation"});
        }
    }
    EXPECT_NE(at(5176)[1]->rationale.find("11.5.2"), std::string::npos);
    EXPECT_NE(at(5308)[1]->rationale.find("3.4"), std::string::npos);
}

TEST_F(Draft18CatalogPart05Test, SenderConcurrencyAndFlowAllocationAreNotInferredFromArrivalOrder) {
    const auto concurrency = at(5199);
    ASSERT_EQ(concurrency.size(), 1u);
    EXPECT_EQ(concurrency.front()->strength, Strength::ShouldNot);
    EXPECT_EQ(concurrency.front()->actor, "publisher");
    EXPECT_EQ(concurrency.front()->testability, Testability::NotTestable);
    const auto allocation = at(5236);
    ASSERT_EQ(allocation.size(), 1u);
    EXPECT_EQ(allocation.front()->strength, Strength::Must);
    EXPECT_EQ(allocation.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(allocation.front()->testability, Testability::NotTestable);
}

TEST_F(Draft18CatalogPart05Test, CancellationPermissionsKeepPublisherAndSubscriberRolesDistinct) {
    const auto cancellation = at(5215);
    ASSERT_EQ(cancellation.size(), 4u);
    EXPECT_EQ(cancellation[0]->evaluators,
              std::vector<std::string>{"data-stream-cancelled-with-subscription-still-established"});
    EXPECT_EQ(cancellation[1]->evaluators,
              std::vector<std::string>{"request-stream-cancellation-terminates-only-that-request"});
    EXPECT_EQ(cancellation[2]->applicability, Applicability::NotApplicable);
    EXPECT_EQ(cancellation[3]->applicability, Applicability::NotApplicable);
    const auto done = at(5221);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done.front()->evaluators,
              std::vector<std::string>{"voluntary-subscription-termination-sends-publish-done"});
    for (const auto line : {5232u, 5234u}) {
        const auto rows = at(line);
        ASSERT_FALSE(rows.empty());
        for (const auto* row : rows) {
            EXPECT_EQ(row->applicability, Applicability::NotApplicable);
        }
    }
}

TEST_F(Draft18CatalogPart05Test, SubgroupCompletionAndEarlyTerminationRequireDifferentSignals) {
    const auto completed = at(5368);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed.front()->evaluators,
              std::vector<std::string>{"complete-subgroup-ends-with-fin"});
    for (const auto line : {5371u, 5443u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->strength, Strength::Must);
        EXPECT_EQ(rows.front()->evaluators,
                  std::vector<std::string>{"incomplete-subgroup-closure-uses-reset"});
    }
    const auto reliable = at(5392);
    ASSERT_EQ(reliable.size(), 1u);
    EXPECT_EQ(reliable.front()->evaluators,
              std::vector<std::string>{"reset-stream-at-reliable-size-covers-subgroup-header"});
    const auto empty = at(5406);
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty.front()->strength, Strength::May);
    EXPECT_EQ(empty.front()->evaluators,
              std::vector<std::string>{"empty-subgroup-reset-reliable-size-equals-header-length"});
}

TEST_F(Draft18CatalogPart05Test, StopSendingResetAndReopenExceptionAreObservable) {
    const auto reset = at(5470);
    ASSERT_EQ(reset.size(), 1u);
    EXPECT_EQ(reset.front()->evaluators,
              std::vector<std::string>{"stopped-subgroup-stream-reset"});
    const auto reopen = at(5479);
    ASSERT_EQ(reopen.size(), 1u);
    EXPECT_EQ(reopen.front()->strength, Strength::ShouldNot);
    EXPECT_EQ(reopen.front()->source.last_line, 5481u);
    EXPECT_EQ(reopen.front()->evaluators,
              std::vector<std::string>{"no-reopened-stopped-subgroup-before-forward-renewal"});
    const auto renewal = at(5482);
    ASSERT_EQ(renewal.size(), 1u);
    EXPECT_EQ(renewal.front()->strength, Strength::May);
    EXPECT_EQ(renewal.front()->evaluators,
              std::vector<std::string>{"stopped-subgroup-reopened-after-forward-zero-to-one"});
}

TEST_F(Draft18CatalogPart05Test, FetchFirstObjectAndDatagramFlagsBelongToPublisherSerialization) {
    const auto first = at(5628);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0]->evaluators,
              std::vector<std::string>{"first-fetch-object-has-absolute-group-id-delta"});
    EXPECT_EQ(first[1]->evaluators,
              std::vector<std::string>{"first-fetch-object-has-absolute-object-id-delta"});
    const auto datagram = at(5663, 1);
    ASSERT_EQ(datagram.size(), 1u);
    EXPECT_EQ(datagram.front()->strength, Strength::Must);
    EXPECT_EQ(datagram.front()->evaluators,
              std::vector<std::string>{"fetch-datagram-object-sets-bit-0x40"});
    const auto low_bits = at(5663, 2);
    ASSERT_EQ(low_bits.size(), 1u);
    EXPECT_EQ(low_bits.front()->strength, Strength::Should);
    EXPECT_EQ(low_bits.front()->evaluators,
              std::vector<std::string>{"fetch-datagram-object-clears-two-low-bits"});
    for (const auto line : {5631u, 5640u, 5649u, 5664u}) {
        const auto rows = at(line);
        ASSERT_FALSE(rows.empty());
        for (const auto* row : rows) {
            EXPECT_EQ(row->actor, "subscriber");
            EXPECT_EQ(row->applicability, Applicability::NotApplicable);
        }
    }
    EXPECT_EQ(at(5640).size(), 2u);
}

TEST_F(Draft18CatalogPart05Test, FinCompleteFetchWithoutUnknownPortionsAvoidsRedundantNonexistentRangeMarkers) {
    const auto range = at(5674);
    ASSERT_EQ(range.size(), 1u);
    EXPECT_EQ(range.front()->id, "D18-11-4-4-2-SHOULD-NOT-001");
    EXPECT_EQ(range.front()->strength, Strength::ShouldNot);
    EXPECT_EQ(range.front()->actor, "publisher");
    EXPECT_EQ(range.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(range.front()->testability, Testability::Testable);
    EXPECT_EQ(range.front()->scenarios,
              std::vector<std::string>{"fetch-fin-complete-known-gap-without-unknown-portion"});
    EXPECT_EQ(range.front()->evaluators,
              std::vector<std::string>{"no-redundant-0x8c-in-fin-complete-fetch-without-unknown-portion"});
}

TEST_F(Draft18CatalogPart05Test, PaddingBytesAreObservableButMemoryDiscardAndSchedulingAreNot) {
    EXPECT_EQ(at(5697).size(), 2u);
    for (const auto line : {5721u, 5740u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->strength, Strength::Must);
        EXPECT_EQ(rows.front()->evaluators,
                  std::vector<std::string>{"padding-data-bytes-all-zero"});
    }
    for (const auto line : {5702u, 5730u, 5749u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto cancellation = at(5733);
    ASSERT_EQ(cancellation.size(), 2u);
    for (const auto* row : cancellation) {
        EXPECT_EQ(row->evaluators,
                  std::vector<std::string>{"padding-cancellation-preserves-active-request-behavior"});
    }
}

TEST_F(Draft18CatalogPart05Test, DefaultGroupOrderAndDynamicGroupsValidateTrackStatusReplies) {
    for (const auto line : {5921u, 5948u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "endpoint");
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->evaluators,
                  std::vector<std::string>{"session-closed-protocol-violation"});
        EXPECT_NE(rows.front()->rationale.find("TRACK_STATUS_OK"), std::string::npos);
    }
}

TEST_F(Draft18CatalogPart05Test, ImmutablePropertiesPreserveOriginalPublisherImmutabilityAndLookup) {
    const auto property = at(5965);
    ASSERT_EQ(property.size(), 2u);
    for (const auto* row : property) {
        EXPECT_EQ(row->strength, Strength::MustNot);
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::Testable);
    }
    const auto bytes = at(5967, 1);
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes.front()->evaluators,
              std::vector<std::string>{"immutable-property-key-value-serialization-unchanged"});
    const auto placement = at(5973);
    ASSERT_EQ(placement.size(), 2u);
    for (const auto* row : placement) {
        EXPECT_EQ(row->strength, Strength::May);
        EXPECT_EQ(row->actor, "original-publisher");
        EXPECT_EQ(row->testability, Testability::Testable);
    }
    const auto search = at(5975);
    ASSERT_EQ(search.size(), 2u);
    for (const auto* row : search) {
        EXPECT_EQ(row->actor, "property-processor");
        EXPECT_EQ(row->evaluators,
                  std::vector<std::string>{"session-closed-protocol-violation"});
    }
    const auto extension = at(5978);
    ASSERT_EQ(extension.size(), 1u);
    EXPECT_EQ(extension.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(extension.front()->testability, Testability::NotTestable);
}

TEST_F(Draft18CatalogPart05Test, ObjectPropertySingletonRulesDoNotInventTrackRestrictions) {
    for (const auto line : {6013u, 6059u, 6094u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->strength, Strength::MustNot);
        EXPECT_EQ(rows.front()->actor, "original-publisher");
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_NE(rows.front()->summary.find("Object"), std::string::npos);
    }
}

TEST_F(Draft18CatalogPart05Test, RelayCacheForwardAndPriorGapMutationRulesRemainOutsidePublisherScope) {
    for (const auto [line, occurrence] :
         {std::pair{5022u, 1u}, {5412u, 1u}, {5437u, 1u}, {5449u, 1u}, {5890u, 1u},
          {5964u, 1u}, {5967u, 2u}, {5968u, 1u}, {5969u, 1u}, {6053u, 1u},
          {6054u, 1u}, {6057u, 1u}, {6088u, 1u}, {6089u, 1u}, {6092u, 1u}}) {
        const auto rows = at(line, occurrence);
        ASSERT_FALSE(rows.empty());
        for (const auto* row : rows) {
            EXPECT_EQ(row->actor, "relay");
            EXPECT_EQ(row->applicability, Applicability::NotApplicable);
            EXPECT_TRUE(row->scenarios.empty());
            EXPECT_TRUE(row->evaluators.empty());
        }
    }
    EXPECT_EQ(at(5437).size(), 2u);
    EXPECT_EQ(at(6057).size(), 2u);
    EXPECT_EQ(at(6092).size(), 2u);
}

}  // namespace
}  // namespace moq::interop::requirements
