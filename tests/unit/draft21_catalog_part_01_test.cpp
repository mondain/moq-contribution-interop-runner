#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft21CatalogPart01Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(21, "draft21-lines-0001-1647.json"));
    }
};

TEST_F(Draft21CatalogPart01Test, CoversEveryOwnedAnchorWithContiguousClausesAndDraft21Ids) {
    expect_partition_coverage(1, 1647, 85);
    const std::map<Strength, std::string> tokens = {
        {Strength::Must, "MUST"}, {Strength::MustNot, "MUST-NOT"},
        {Strength::Should, "SHOULD"}, {Strength::ShouldNot, "SHOULD-NOT"},
        {Strength::May, "MAY"}};
    for (const auto& row : catalog_.requirements) {
        auto section = row.source.section;
        std::replace(section.begin(), section.end(), '.', '-');
        EXPECT_TRUE(row.id.starts_with("D21-" + section + "-" + tokens.at(row.strength) + "-"))
            << row.id;
    }
}

TEST_F(Draft21CatalogPart01Test, IdsAreUniqueAcrossBothDraftCatalogsAndEveryExistingPartition) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    std::set<std::string> ids;
    for (const auto draft : {18u, 21u}) {
        const auto source = load_draft_source(draft, root / "docs",
                                              root / "requirements/draft-digests.json");
        std::vector<std::filesystem::path> paths = {
            root / "requirements" / ("draft" + std::to_string(draft) + ".json")};
        for (const auto& entry : std::filesystem::directory_iterator(root / "requirements/parts")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json" &&
                entry.path().filename().string().starts_with("draft" + std::to_string(draft) + "-")) {
                paths.push_back(entry.path());
            }
        }
        for (const auto& path : paths) {
            const auto part = RequirementCatalog::load(source, path, CatalogLoadMode::AllowIncomplete);
            for (const auto& row : part.requirements) {
                EXPECT_TRUE(ids.insert(row.id).second) << path << ": " << row.id;
            }
        }
    }
}

TEST_F(Draft21CatalogPart01Test, Bcp14VocabularyIsInformativeAndNormalizesAllElevenKeywords) {
    const std::vector<Strength> first = {Strength::Must, Strength::MustNot, Strength::Must,
                                       Strength::Must, Strength::MustNot};
    const std::vector<Strength> second = {Strength::Should, Strength::ShouldNot,
                                        Strength::Should, Strength::ShouldNot, Strength::May};
    for (const auto& [line, strengths] :
         std::map<std::size_t, std::vector<Strength>>{{515, first}, {516, second}, {517, {Strength::May}}}) {
        for (unsigned occurrence = 1; occurrence <= strengths.size(); ++occurrence) {
            const auto rows = at(line, occurrence);
            ASSERT_EQ(rows.size(), 1u);
            EXPECT_EQ(rows.front()->strength, strengths[occurrence - 1]);
            EXPECT_EQ(rows.front()->applicability, Applicability::Informative);
            EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
        }
    }
}

TEST_F(Draft21CatalogPart01Test, LimitedPublisherPolicyAndUnsupportedMessageResponseHaveDifferentObservability) {
    const auto subset = at(597);
    ASSERT_EQ(subset.size(), 1u);
    EXPECT_EQ(subset.front()->actor, "non-relay endpoint");
    EXPECT_EQ(subset.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(subset.front()->testability, Testability::NotTestable);
    const auto unsupported = at(600);
    ASSERT_EQ(unsupported.size(), 1u);
    EXPECT_EQ(unsupported.front()->strength, Strength::Should);
    EXPECT_EQ(unsupported.front()->evaluators,
              std::vector<std::string>{"d21-unsupported-request-not-supported"});
    const auto relay = at(603);
    ASSERT_EQ(relay.size(), 2u);
    for (const auto* row : relay) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart01Test, SubscriptionSubgroupMappingPreservesExceptionsAndFirstObjectRoles) {
    const auto separate = at(719);
    ASSERT_EQ(separate.size(), 1u);
    EXPECT_EQ(separate.front()->strength, Strength::MustNot);
    EXPECT_EQ(separate.front()->evaluators,
              std::vector<std::string>{"d21-no-mixed-subgroups-on-subscription-stream"});
    const auto same = at(720);
    ASSERT_EQ(same.size(), 1u);
    EXPECT_NE(same.front()->summary.find("reset"), std::string::npos);
    EXPECT_NE(same.front()->summary.find("out of Object ID order"), std::string::npos);
    const auto original = at(752);
    ASSERT_EQ(original.size(), 1u);
    EXPECT_EQ(original.front()->actor, "original publisher");
    EXPECT_EQ(original.front()->evaluators, std::vector<std::string>{"d21-new-subgroup-first-object-bit"});
    const auto relay = at(756);
    ASSERT_EQ(relay.size(), 1u);
    EXPECT_EQ(relay.front()->actor, "relay");
    EXPECT_EQ(relay.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart01Test, GroupMediaSemanticsRemainInternallyObservedAndSubscriberFiltersExcluded) {
    for (const auto line : {761u, 762u, 770u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto subscriber = at(795);
    ASSERT_EQ(subscriber.size(), 1u);
    EXPECT_EQ(subscriber.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart01Test, ReservedNamespacesRetainRegistrationApplicationAndSinglePeriodRules) {
    const auto registered = at(848);
    ASSERT_EQ(registered.size(), 1u);
    EXPECT_NE(registered.front()->summary.find("IANA"), std::string::npos);
    const auto application = at(851);
    ASSERT_EQ(application.size(), 1u);
    EXPECT_EQ(application.front()->testability, Testability::NotTestable);
    EXPECT_NE(application.front()->rationale.find(".session"), std::string::npos);
    const auto prohibited = at(857, 1);
    ASSERT_EQ(prohibited.size(), 2u);
    EXPECT_EQ(prohibited[0]->evaluators, std::vector<std::string>{"d21-no-tracks-under-single-period"});
    EXPECT_EQ(prohibited[1]->evaluators, std::vector<std::string>{"d21-no-namespaces-under-single-period"});
    const auto reject = at(857, 2);
    ASSERT_EQ(reject.size(), 1u);
    EXPECT_EQ(reject.front()->evaluators, std::vector<std::string>{"d21-single-period-request-does-not-exist"});
}

TEST_F(Draft21CatalogPart01Test, PublisherResponseExclusivityUsesDraft21RequestOkShorthand) {
    const auto subscribe = at(984);
    ASSERT_EQ(subscribe.size(), 1u);
    EXPECT_EQ(subscribe.front()->evaluators, std::vector<std::string>{"d21-one-subscribe-ok-or-request-error"});
    const auto subscriber = at(985);
    ASSERT_EQ(subscriber.size(), 1u);
    EXPECT_EQ(subscriber.front()->applicability, Applicability::NotApplicable);
    EXPECT_NE(subscriber.front()->rationale.find("REQUEST_OK"), std::string::npos);
    const auto duplicate = at(987);
    ASSERT_EQ(duplicate.size(), 1u);
    EXPECT_EQ(duplicate.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(duplicate.front()->evaluators,
              std::vector<std::string>{"d21-duplicate-publish-response-session-protocol-error"});
    const auto fetch = at(1133);
    ASSERT_EQ(fetch.size(), 1u);
    EXPECT_EQ(fetch.front()->evaluators, std::vector<std::string>{"d21-one-fetch-ok-or-request-error"});
}

TEST_F(Draft21CatalogPart01Test, ConcurrentSubscriptionsKeepUniqueRequestsAliasChoicesAndPerSubscriptionFanout) {
    const auto multiple = at(1019);
    ASSERT_EQ(multiple.size(), 1u);
    EXPECT_NE(multiple.front()->summary.find("unique Request ID"), std::string::npos);
    const auto aliases = at(1020);
    ASSERT_EQ(aliases.size(), 2u);
    for (const auto* row : aliases) {
        EXPECT_EQ(row->strength, Strength::May);
        EXPECT_EQ(row->testability, Testability::Testable);
    }
    const auto fanout = at(1024);
    ASSERT_EQ(fanout.size(), 1u);
    EXPECT_EQ(fanout.front()->evaluators, std::vector<std::string>{"d21-object-per-matching-subscription"});
    EXPECT_NE(fanout.front()->summary.find("same Track Alias"), std::string::npos);
    const auto incomplete = at(1032);
    ASSERT_EQ(incomplete.size(), 1u);
    EXPECT_EQ(incomplete.front()->evaluators,
              std::vector<std::string>{"d21-object-prefix-sent-before-production-completes"});
}

TEST_F(Draft21CatalogPart01Test, SubscriptionCancellationAndErrorCleanupKeepTheirDistinctTriggers) {
    const auto stop = at(1054);
    ASSERT_EQ(stop.size(), 1u);
    EXPECT_EQ(stop.front()->evaluators, std::vector<std::string>{"d21-subscribe-stop-sending-resets-open-streams"});
    const auto done = at(1058);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(done.front()->testability, Testability::NotTestable);
    EXPECT_NE(done.front()->rationale.find("late"), std::string::npos);
    const auto error = at(1070);
    ASSERT_EQ(error.size(), 1u);
    EXPECT_EQ(error.front()->strength, Strength::MustNot);
    EXPECT_NE(error.front()->rationale.find("ambiguity"), std::string::npos);
    EXPECT_NE(error.front()->rationale.find("3.1"), std::string::npos);
}

TEST_F(Draft21CatalogPart01Test, AliasUniquenessAndReuseRetainSessionAndInflightBoundaries) {
    const auto concurrent = at(1080);
    ASSERT_EQ(concurrent.size(), 1u);
    EXPECT_EQ(concurrent.front()->evaluators, std::vector<std::string>{"d21-distinct-active-tracks-have-distinct-aliases"});
    const auto subscriber = at(1083);
    ASSERT_EQ(subscriber.size(), 1u);
    EXPECT_EQ(subscriber.front()->applicability, Applicability::NotApplicable);
    const auto reuse = at(1098);
    ASSERT_EQ(reuse.size(), 1u);
    EXPECT_NE(reuse.front()->summary.find("in flight"), std::string::npos);
    EXPECT_NE(reuse.front()->summary.find("completely closed"), std::string::npos);
}

TEST_F(Draft21CatalogPart01Test, FetchCancellationSeparatesRequestAndDataStreamResets) {
    const auto resets = at(1144);
    ASSERT_EQ(resets.size(), 2u);
    EXPECT_EQ(resets[0]->evaluators, std::vector<std::string>{"d21-fetch-cancel-resets-bidi-request-stream"});
    EXPECT_EQ(resets[1]->evaluators, std::vector<std::string>{"d21-fetch-cancel-resets-unidirectional-data-stream"});
    for (const auto line : {1139u, 1140u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "subscriber");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft21CatalogPart01Test, FilterValidationRetainsInclusiveLocationsDuplicateKeysAndNegotiatedLimits) {
    const auto location = at(1192);
    ASSERT_EQ(location.size(), 1u);
    EXPECT_NE(location.front()->rationale.find("inclusive"), std::string::npos);
    EXPECT_NE(location.front()->rationale.find("asynchronous"), std::string::npos);
    const auto duplicate = at(1225);
    ASSERT_EQ(duplicate.size(), 1u);
    EXPECT_NE(duplicate.front()->summary.find("SetID"), std::string::npos);
    EXPECT_NE(duplicate.front()->summary.find("Property Type"), std::string::npos);
    EXPECT_EQ(duplicate.front()->evaluators, std::vector<std::string>{"d21-duplicate-range-filter-invalid-filter"});
    const auto limit = at(1245);
    ASSERT_EQ(limit.size(), 1u);
    EXPECT_EQ(limit.front()->evaluators, std::vector<std::string>{"d21-excess-filter-ranges-invalid-filter"});
    EXPECT_NE(limit.front()->rationale.find("Section 9.1.6"), std::string::npos);
    const auto combined = at(1264);
    ASSERT_EQ(combined.size(), 1u);
    EXPECT_NE(combined.front()->rationale.find("SetID"), std::string::npos);
    EXPECT_NE(combined.front()->rationale.find("ambiguity"), std::string::npos);
}

TEST_F(Draft21CatalogPart01Test, FillFailureOpensAndResetsHeaderStreamWithoutEndingSubscription) {
    const auto cancel = at(1352);
    ASSERT_EQ(cancel.size(), 1u);
    EXPECT_EQ(cancel.front()->evaluators, std::vector<std::string>{"d21-cancel-subscription-resets-all-fill-streams"});
    const auto failure = at(1359);
    ASSERT_EQ(failure.size(), 2u);
    EXPECT_EQ(failure[0]->evaluators, std::vector<std::string>{"d21-fill-failure-opens-fetch-header-stream"});
    EXPECT_EQ(failure[1]->evaluators, std::vector<std::string>{"d21-fill-failure-resets-after-fetch-header"});
    for (const auto* row : failure) {
        EXPECT_EQ(row->testability, Testability::Testable);
        EXPECT_NE(row->rationale.find("subscription"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart01Test, MandatoryPropertySenderScopeAndKnownUnsupportedPeerActionsAreSeparate) {
    const auto scope = at(1432);
    ASSERT_EQ(scope.size(), 1u);
    EXPECT_EQ(scope.front()->applicability, Applicability::Applicable);
    EXPECT_NE(scope.front()->summary.find("0x4000-0x7FFF"), std::string::npos);
    for (const auto line : {1441u, 1444u, 1447u, 1449u, 1461u, 1463u, 1466u}) {
        const auto rows = at(line);
        ASSERT_FALSE(rows.empty());
        for (const auto* row : rows) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    }
    const auto publisher = at(1469);
    ASSERT_EQ(publisher.size(), 3u);
    const std::vector<std::string> evaluators = {
        "d21-known-unsupported-mandatory-property-subscribe-error",
        "d21-known-unsupported-mandatory-property-fetch-error",
        "d21-known-unsupported-mandatory-property-no-publish"};
    for (std::size_t index = 0; index < publisher.size(); ++index) {
        EXPECT_EQ(publisher[index]->strength, Strength::Should);
        EXPECT_EQ(publisher[index]->evaluators, std::vector<std::string>{evaluators[index]});
        EXPECT_NE(publisher[index]->rationale.find("known"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart01Test, NamespaceDiscoverySeparatesRequestKindsAndOriginalPublisherAnnouncements) {
    const auto responses = at(1532);
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_EQ(responses[0]->evaluators, std::vector<std::string>{"d21-subscribe-namespace-single-first-response"});
    EXPECT_EQ(responses[1]->evaluators, std::vector<std::string>{"d21-subscribe-tracks-single-first-response"});
    const auto skipped = at(1540);
    ASSERT_EQ(skipped.size(), 1u);
    EXPECT_NE(skipped.front()->summary.find("single PUBLISH"), std::string::npos);
    const auto announce = at(1576);
    ASSERT_EQ(announce.size(), 2u);
    EXPECT_EQ(announce[0]->actor, "original publisher");
    EXPECT_EQ(announce[0]->evaluators, std::vector<std::string>{"d21-original-publisher-matching-namespace-notification"});
    EXPECT_EQ(announce[1]->actor, "relay");
    EXPECT_EQ(announce[1]->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart01Test, PublishNamespaceResponseHandlingKeepsApplicationBoundaryAndSubscriberRoles) {
    const auto token = at(1585);
    ASSERT_EQ(token.size(), 1u);
    EXPECT_EQ(token.front()->actor, "publisher");
    EXPECT_EQ(token.front()->testability, Testability::Testable);
    const auto application = at(1589);
    ASSERT_EQ(application.size(), 2u);
    for (const auto* row : application) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
    }
    const auto response = at(1592);
    ASSERT_EQ(response.size(), 1u);
    EXPECT_EQ(response.front()->actor, "subscriber");
    EXPECT_EQ(response.front()->applicability, Applicability::NotApplicable);
    const auto duplicate = at(1594);
    ASSERT_EQ(duplicate.size(), 1u);
    EXPECT_EQ(duplicate.front()->evaluators,
              std::vector<std::string>{"d21-duplicate-publish-namespace-response-session-protocol-error"});
    for (const auto line : {1543u, 1554u, 1614u, 1616u, 1638u, 1640u}) {
        const auto rows = at(line);
        ASSERT_FALSE(rows.empty());
        for (const auto* row : rows) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    }
}

}  // namespace
}  // namespace moq::interop::requirements
