#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft18CatalogPart04Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(18, "draft18-lines-3509-4935.json"));
    }
};

TEST_F(Draft18CatalogPart04Test, CoversAllOwnedOccurrencesWithoutConflictsOrForeignAnchors) {
    expect_partition_coverage(3509, 4935, 100);
}

TEST_F(Draft18CatalogPart04Test, RequirementIdsAreUniqueAcrossAllFourPartitions) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    std::set<std::string> ids;
    for (const auto* filename : {"draft18-lines-0001-1254.json", "draft18-lines-1255-2564.json",
                                "draft18-lines-2565-3508.json", "draft18-lines-3509-4935.json"}) {
        const auto part = RequirementCatalog::load(
            source_, root / "requirements/parts" / filename, CatalogLoadMode::AllowIncomplete);
        for (const auto& row : part.requirements) {
            EXPECT_TRUE(ids.insert(row.id).second) << row.id;
        }
    }
}

TEST_F(Draft18CatalogPart04Test, UnknownSetupOptionsAndTheirDuplicatesAreAccepted) {
    for (const auto line : {3521u, 3544u, 3547u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->strength, Strength::Must);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_EQ(rows.front()->evaluators,
                  std::vector<std::string>{"setup-continues-with-unknown-options-ignored"});
    }
    const auto duplicates = at(3545);
    ASSERT_EQ(duplicates.size(), 1u);
    EXPECT_EQ(duplicates.front()->strength, Strength::MustNot);
    EXPECT_NE(duplicates.front()->summary.find("unless"), std::string::npos);
}

TEST_F(Draft18CatalogPart04Test, AuthorityAndPathValidationPreserveTransportRoles) {
    for (const auto line : {3558u, 3574u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u);
        EXPECT_EQ(rows[0]->actor, "server");
        EXPECT_EQ(rows[0]->applicability, Applicability::Applicable);
        EXPECT_EQ(rows[0]->testability, Testability::NotTestable);
        EXPECT_EQ(rows[1]->testability, Testability::Testable);
    }
    for (const auto line : {3562u, 3579u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 3u);
        EXPECT_EQ(rows[0]->testability, Testability::Testable);
        EXPECT_EQ(rows[1]->testability, Testability::Testable);
        EXPECT_EQ(rows[2]->actor, "server");
        EXPECT_EQ(rows[2]->applicability, Applicability::Applicable);
        EXPECT_EQ(rows[2]->testability, Testability::NotTestable);
    }
    for (const auto line : {3569u, 3594u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
        EXPECT_NE(rows.front()->rationale.find("server"), std::string::npos);
    }
    EXPECT_EQ(at(3591).size(), 1u);
    EXPECT_EQ(at(3592).size(), 1u);
}

TEST_F(Draft18CatalogPart04Test, SetupTokenOverflowHasItsOwnExceptionAndAliasCleanup) {
    const auto overflow = at(3620);
    ASSERT_EQ(overflow.size(), 1u);
    EXPECT_EQ(overflow.front()->strength, Strength::MustNot);
    EXPECT_EQ(overflow.front()->evaluators,
              std::vector<std::string>{"no-auth-token-cache-overflow-session-error"});
    const auto use_value = at(3621);
    ASSERT_EQ(use_value.size(), 1u);
    EXPECT_EQ(use_value.front()->evaluators,
              std::vector<std::string>{"setup-token-processed-as-value-without-alias-registration"});
    const auto purge = at(3623);
    ASSERT_EQ(purge.size(), 1u);
    EXPECT_EQ(purge.front()->testability, Testability::NotTestable);
}

TEST_F(Draft18CatalogPart04Test, GoawayMigrationAndRejectionKeepStreamAndRoleConditions) {
    const auto migration = at(3667);
    ASSERT_EQ(migration.size(), 2u);
    EXPECT_EQ(migration[0]->evaluators,
              std::vector<std::string>{"same-publisher-request-reissued-at-goaway-destination"});
    EXPECT_EQ(migration[1]->evaluators,
              std::vector<std::string>{"old-publisher-request-stream-closed"});
    const auto duplicate = at(3688);
    ASSERT_EQ(duplicate.size(), 2u);
    for (const auto* row : duplicate) {
        EXPECT_EQ(row->evaluators, std::vector<std::string>{"session-closed-protocol-violation"});
    }
    const auto reject = at(3743);
    ASSERT_EQ(reject.size(), 2u);
    for (const auto* row : reject) {
        EXPECT_EQ(row->actor, "goaway-sender");
        EXPECT_EQ(row->strength, Strength::Must);
        EXPECT_EQ(row->evaluators, std::vector<std::string>{"request-error-going-away"});
    }
    const auto server = at(3723);
    ASSERT_EQ(server.size(), 1u);
    EXPECT_EQ(server.front()->testability, Testability::NotTestable);
    const auto urgency = at(3731);
    ASSERT_EQ(urgency.size(), 1u);
    EXPECT_EQ(urgency.front()->testability, Testability::NotTestable);
}

TEST_F(Draft18CatalogPart04Test, RequestOkPropertyRulesUseTheDefinedShorthand) {
    const auto rows = at(3786);
    ASSERT_EQ(rows.size(), 4u);
    const std::vector<Applicability> applicability = {
        Applicability::Applicable, Applicability::Applicable,
        Applicability::NotApplicable, Applicability::Applicable};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        EXPECT_EQ(rows[index]->applicability, applicability[index]);
        if (applicability[index] == Applicability::Applicable) {
            EXPECT_EQ(rows[index]->testability, Testability::Testable);
            EXPECT_EQ(rows[index]->evaluators,
                      std::vector<std::string>{"session-closed-protocol-violation"});
        }
        EXPECT_NE(rows[index]->rationale.find("10.5"), std::string::npos);
    }
}

TEST_F(Draft18CatalogPart04Test, RedirectsSeparatePublisherRequestsFromRelayForwarding) {
    const auto empty_name = at(3833);
    ASSERT_EQ(empty_name.size(), 2u);
    EXPECT_EQ(empty_name[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(empty_name[1]->applicability, Applicability::NotApplicable);
    const auto bad_name = at(3835);
    ASSERT_EQ(bad_name.size(), 2u);
    EXPECT_EQ(bad_name[0]->applicability, Applicability::NotApplicable);
    EXPECT_EQ(bad_name[1]->evaluators,
              std::vector<std::string>{"session-closed-protocol-violation"});
    const auto retry = at(3927);
    ASSERT_EQ(retry.size(), 2u);
    EXPECT_EQ(retry[0]->evaluators,
              std::vector<std::string>{"publisher-connects-to-redirect-uri"});
    EXPECT_EQ(retry[1]->evaluators,
              std::vector<std::string>{"publish-namespace-retried-with-redirect-namespace"});
    for (const auto line : {3932u, 3933u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "relay");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft18CatalogPart04Test, UpdateResponsesAndFailureCleanupKeepTheCoalescingException) {
    const auto subscribe = at(4014);
    ASSERT_EQ(subscribe.size(), 2u);
    EXPECT_EQ(subscribe[0]->evaluators,
              std::vector<std::string>{"successful-subscribe-responds-subscribe-ok"});
    EXPECT_EQ(subscribe[1]->evaluators,
              std::vector<std::string>{"matching-new-object-delivered-on-established-subscription"});
    const auto exclusive = at(4063);
    ASSERT_EQ(exclusive.size(), 1u);
    EXPECT_EQ(exclusive.front()->evaluators,
              std::vector<std::string>{"exactly-one-request-ok-or-request-error"});
    EXPECT_NE(exclusive.front()->rationale.find("coalesced"), std::string::npos);
    const auto subscription = at(4114);
    ASSERT_EQ(subscription.size(), 1u);
    EXPECT_EQ(subscription.front()->evaluators,
              std::vector<std::string>{"publish-done-update-failed"});
    const auto fetch = at(4117);
    ASSERT_EQ(fetch.size(), 1u);
    EXPECT_EQ(fetch.front()->evaluators, std::vector<std::string>{"fetch-data-stream-reset"});
    const auto namespace_rows = at(4119);
    ASSERT_EQ(namespace_rows.size(), 2u);
    EXPECT_EQ(namespace_rows[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(namespace_rows[1]->applicability, Applicability::NotApplicable);
    const auto successful = at(4124);
    ASSERT_EQ(successful.size(), 1u);
    EXPECT_EQ(successful.front()->evaluators,
              std::vector<std::string>{"one-request-ok-per-successful-coalesced-update"});
}

TEST_F(Draft18CatalogPart04Test, PublishDoneOrderingDoesNotClaimInternalStateVisibility) {
    const auto ordering = at(4227);
    ASSERT_EQ(ordering.size(), 2u);
    for (const auto* row : ordering) {
        EXPECT_EQ(row->strength, Strength::MustNot);
        EXPECT_EQ(row->testability, Testability::NotTestable);
        EXPECT_NE(row->rationale.find("arrival"), std::string::npos);
    }
    const auto state = at(4234);
    ASSERT_EQ(state.size(), 1u);
    EXPECT_EQ(state.front()->testability, Testability::NotTestable);
    const auto fin = at(4236);
    ASSERT_EQ(fin.size(), 1u);
    EXPECT_EQ(fin.front()->evaluators,
              std::vector<std::string>{"publish-done-followed-by-request-stream-fin"});
    const auto no_streams = at(4281);
    ASSERT_EQ(no_streams.size(), 1u);
    EXPECT_EQ(no_streams.front()->evaluators, std::vector<std::string>{"publish-done-stream-count-zero"});
    const auto unknown_count = at(4284, 1);
    ASSERT_EQ(unknown_count.size(), 1u);
    EXPECT_EQ(unknown_count.front()->testability, Testability::NotTestable);
}

TEST_F(Draft18CatalogPart04Test, JoiningFetchValidatesStateAndProcessesUpdatesBeforeItsRange) {
    for (const auto line : {4430u, 4441u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"request-error-invalid-range"});
    }
    const auto pending_updates = at(4431);
    ASSERT_EQ(pending_updates.size(), 1u);
    EXPECT_EQ(pending_updates.front()->evaluators,
              std::vector<std::string>{"joining-fetch-uses-updated-forward-state-and-joining-location"});
    const auto bad_reference = at(4454);
    ASSERT_EQ(bad_reference.size(), 1u);
    EXPECT_EQ(bad_reference.front()->evaluators,
              std::vector<std::string>{"request-error-invalid-joining-request-id"});
    EXPECT_NE(bad_reference.front()->summary.find("Pending"), std::string::npos);
    const auto requested_range = at(4567);
    ASSERT_EQ(requested_range.size(), 2u);
    for (const auto* row : requested_range) {
        EXPECT_EQ(row->actor, "subscriber");
        EXPECT_EQ(row->applicability, Applicability::NotApplicable);
        EXPECT_TRUE(row->evaluators.empty());
    }
    const auto invalid_range = at(4577);
    ASSERT_EQ(invalid_range.size(), 2u);
    for (const auto* row : invalid_range) {
        EXPECT_EQ(row->evaluators, std::vector<std::string>{"request-error-invalid-range"});
    }
    const auto order = at(4580);
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order.front()->evaluators,
              std::vector<std::string>{"fetch-groups-follow-requested-ascending-or-descending-order"});
}

TEST_F(Draft18CatalogPart04Test, FetchOkTimingAndRelayTrackStatusArePreciselyScoped) {
    const auto early_objects = at(4607);
    ASSERT_EQ(early_objects.size(), 1u);
    EXPECT_EQ(early_objects.front()->strength, Strength::May);
    EXPECT_EQ(early_objects.front()->testability, Testability::NotTestable);
    const auto known_end = at(4609);
    ASSERT_EQ(known_end.size(), 1u);
    EXPECT_EQ(known_end.front()->strength, Strength::MustNot);
    EXPECT_EQ(known_end.front()->testability, Testability::NotTestable);
    for (const auto line : {4673u, 4674u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "relay");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    const auto backwards = at(4639);
    ASSERT_EQ(backwards.size(), 1u);
    EXPECT_EQ(backwards.front()->actor, "subscriber");
    EXPECT_EQ(backwards.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft18CatalogPart04Test, NamespaceDiscoveryErrorsAuthorizationAndOrderingApplyToPublishers) {
    for (const auto line : {4787u, 4860u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"session-closed-protocol-violation"});
    }
    for (const auto line : {4805u, 4884u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "publisher");
        EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"request-error-prefix-overlap"});
        EXPECT_NE(rows.front()->rationale.find("independent"), std::string::npos);
    }
    for (const auto line : {4809u, 4888u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->evaluators,
                  std::vector<std::string>{"unauthorized-namespace-subscription-not-accepted"});
    }
    const auto ordered = at(4821);
    ASSERT_EQ(ordered.size(), 1u);
    EXPECT_EQ(ordered.front()->evaluators,
              std::vector<std::string>{"namespace-precedes-corresponding-namespace-done"});
    for (const auto line : {4795u, 4823u, 4831u, 4868u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "subscriber");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft18CatalogPart04Test, ApplicationCodeAdviceHasNoInventedWireOracle) {
    for (const auto line : {3872u, 4294u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "application");
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
}

TEST_F(Draft18CatalogPart04Test, RetryableExcessiveLoadAdvertisesAPositiveRetryInterval) {
    const auto rows = at(3907);
    ASSERT_EQ(rows.size(), 1u);
    const auto* row = rows.front();
    EXPECT_EQ(row->id, "D18-10-6-2-SHOULD-003");
    EXPECT_EQ(row->strength, Strength::Should);
    EXPECT_EQ(row->applicability, Applicability::Applicable);
    EXPECT_EQ(row->testability, Testability::Testable);
    EXPECT_EQ(row->scenarios,
              std::vector<std::string>{"publisher-rejects-retryable-request-with-excessive-load"});
    EXPECT_EQ(row->evaluators,
              std::vector<std::string>{"retryable-excessive-load-has-positive-retry-interval"});
}

}  // namespace
}  // namespace moq::interop::requirements
