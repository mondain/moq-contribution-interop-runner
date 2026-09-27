#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft21CatalogPart04Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(21, "draft21-lines-3339-4260.json"));
    }
};

TEST_F(Draft21CatalogPart04Test, CoversEveryOwnedAnchorWithContiguousClausesAndMatchingIds) {
    expect_partition_coverage(3339, 4260, 75);
    const std::map<Strength, std::string> tokens = {
        {Strength::Must, "MUST"}, {Strength::MustNot, "MUST-NOT"},
        {Strength::Should, "SHOULD"}, {Strength::ShouldNot, "SHOULD-NOT"},
        {Strength::May, "MAY"}};
    for (const auto& row : catalog_.requirements) {
        auto section = row.source.section;
        std::replace(section.begin(), section.end(), '.', '-');
        EXPECT_TRUE(row.id.starts_with("D21-" + section + "-" + tokens.at(row.strength) + "-"));
        EXPECT_FALSE(row.actor.empty());
        EXPECT_FALSE(row.summary.empty());
        EXPECT_FALSE(row.rationale.empty());
    }
}

TEST_F(Draft21CatalogPart04Test, IdsRemainGloballyUniqueAcrossCatalogsAndPartitions) {
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
            for (const auto& row : part.requirements) EXPECT_TRUE(ids.insert(row.id).second) << row.id;
        }
    }
}

TEST_F(Draft21CatalogPart04Test, FirstMessagesSeparatePublisherRequestsAndSubscriberRequests) {
    const auto rows = at(3368);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]->applicability, Applicability::Applicable);
    EXPECT_NE(rows[0]->summary.find("PUBLISH_NAMESPACE"), std::string::npos);
    EXPECT_NE(rows[0]->summary.find("new request stream"), std::string::npos);
    EXPECT_EQ(rows[1]->applicability, Applicability::NotApplicable);
    const auto pipeline = at(3448);
    ASSERT_EQ(pipeline.size(), 1u);
    EXPECT_NE(pipeline.front()->summary.find("not offering extensions"), std::string::npos);
    EXPECT_NE(pipeline.front()->summary.find("after SETUP"), std::string::npos);
}

TEST_F(Draft21CatalogPart04Test, UnknownAndTruncatedMessagesKeepDistinctClosureOracles) {
    const auto unknown = at(3435);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown.front()->evaluators,
              std::vector<std::string>{"d21-unknown-message-session-close"});
    EXPECT_EQ(unknown.front()->summary.find("PROTOCOL_VIOLATION"), std::string::npos);
    const auto length = at(3440);
    ASSERT_EQ(length.size(), 1u);
    EXPECT_EQ(length.front()->evaluators,
              std::vector<std::string>{"d21-message-body-length-protocol-violation"});
    EXPECT_NE(length.front()->rationale.find("fragment"), std::string::npos);
    EXPECT_NE(length.front()->rationale.find("FIN"), std::string::npos);
}

TEST_F(Draft21CatalogPart04Test, AuthorityAndPathPreserveTransportRolesAndUriComponents) {
    for (const auto line : {3489u, 3505u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u);
        EXPECT_EQ(rows[0]->testability, Testability::NotTestable);
        EXPECT_EQ(rows[1]->testability, Testability::Testable);
        for (const auto* row : rows) EXPECT_EQ(row->applicability, Applicability::Applicable);
    }
    for (const auto line : {3493u, 3510u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 3u);
        EXPECT_EQ(rows[0]->testability, Testability::Testable);
        EXPECT_EQ(rows[1]->testability, Testability::Testable);
        EXPECT_EQ(rows[2]->testability, Testability::NotTestable);
        EXPECT_NE(rows[2]->rationale.find("listener"), std::string::npos);
    }
    for (const auto line : {3500u, 3517u, 3559u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto path = at(3514);
    ASSERT_EQ(path.size(), 1u);
    EXPECT_NE(path.front()->summary.find("path-abempty"), std::string::npos);
    const auto query = at(3515);
    ASSERT_EQ(query.size(), 1u);
    EXPECT_NE(query.front()->summary.find("?"), std::string::npos);
}

TEST_F(Draft21CatalogPart04Test, SetupTokenFallbackAndPrivateStateAreClassifiedSeparately) {
    const auto no_error = at(3563);
    ASSERT_EQ(no_error.size(), 1u);
    EXPECT_EQ(no_error.front()->strength, Strength::MustNot);
    EXPECT_EQ(no_error.front()->testability, Testability::Testable);
    const auto fallback = at(3564);
    ASSERT_EQ(fallback.size(), 1u);
    EXPECT_NE(fallback.front()->summary.find("USE_VALUE"), std::string::npos);
    EXPECT_NE(fallback.front()->rationale.find("16 bytes"), std::string::npos);
    const auto purge = at(3566);
    ASSERT_EQ(purge.size(), 1u);
    EXPECT_EQ(purge.front()->testability, Testability::NotTestable);
    EXPECT_NE(purge.front()->summary.find("default of 0"), std::string::npos);
    const auto identity = at(3589);
    ASSERT_EQ(identity.size(), 1u);
    EXPECT_NE(identity.front()->summary.find("configured"), std::string::npos);
    EXPECT_NE(identity.front()->rationale.find("privacy"), std::string::npos);
}

TEST_F(Draft21CatalogPart04Test, LimitsDistinguishZeroFilterSupportFromUnlimitedUpdates) {
    const auto filters = at(3605);
    ASSERT_EQ(filters.size(), 1u);
    EXPECT_EQ(filters.front()->applicability, Applicability::NotApplicable);
    const auto overflow = at(3607);
    ASSERT_EQ(overflow.size(), 1u);
    EXPECT_NE(overflow.front()->summary.find("INVALID_FILTER"), std::string::npos);
    EXPECT_NE(overflow.front()->summary.find("total"), std::string::npos);
    for (const auto line : {3617u, 3631u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_NE(rows.front()->summary.find("nonzero"), std::string::npos);
        EXPECT_NE(rows.front()->rationale.find("unlimited"), std::string::npos);
    }
    const auto limit = at(3631);
    EXPECT_NE(limit.front()->summary.find("TOO_MANY_REQUEST_UPDATES"), std::string::npos);
}

TEST_F(Draft21CatalogPart04Test, GoawaySeparatesRequestMigrationSessionDrainAndTransportRoles) {
    const auto client = at(3650);
    ASSERT_EQ(client.size(), 1u);
    EXPECT_NE(client.front()->summary.find("zero-length"), std::string::npos);
    const auto migration = at(3656);
    ASSERT_EQ(migration.size(), 2u);
    EXPECT_EQ(migration[0]->evaluators, std::vector<std::string>{"d21-request-goaway-reissue"});
    EXPECT_NE(migration[1]->summary.find("close"), std::string::npos);
    const auto duplicate = at(3679);
    ASSERT_EQ(duplicate.size(), 2u);
    EXPECT_NE(duplicate[0]->summary.find("control stream"), std::string::npos);
    EXPECT_NE(duplicate[1]->summary.find("single request stream"), std::string::npos);
    for (const auto line : {3705u, 3712u, 3720u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto early = at(3721);
    ASSERT_EQ(early.size(), 2u);
    for (const auto* row : early) EXPECT_EQ(row->strength, Strength::May);
}

TEST_F(Draft21CatalogPart04Test, RequestOkPropertiesAndRedirectsPreserveExactDraftConstraints) {
    const auto properties = at(3763);
    ASSERT_EQ(properties.size(), 1u);
    EXPECT_NE(properties.front()->rationale.find("SUBSCRIBE_TRACKS_OK"), std::string::npos);
    EXPECT_NE(properties.front()->rationale.find("omits"), std::string::npos);
    const auto server_redirect = at(3788);
    ASSERT_EQ(server_redirect.size(), 1u);
    EXPECT_EQ(server_redirect.front()->testability, Testability::NotTestable);
    for (const auto line : {3796u, 3797u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_NE(rows.front()->summary.find("Track Name"), std::string::npos);
    }
    const auto retry = at(3827);
    ASSERT_EQ(retry.size(), 1u);
    EXPECT_NE(retry.front()->summary.find("minus one"), std::string::npos);
    const auto no_retry = at(3828);
    ASSERT_EQ(no_retry.size(), 1u);
    EXPECT_NE(no_retry.front()->rationale.find("Redirect"), std::string::npos);
}

TEST_F(Draft21CatalogPart04Test, RequestUpdatesRequireLegalDirectionAndAccountForCoalescedResponses) {
    const auto direction = at(3854);
    ASSERT_EQ(direction.size(), 1u);
    EXPECT_NE(direction.front()->summary.find("PUBLISH"), std::string::npos);
    const auto responses = at(3856);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_NE(responses.front()->summary.find("exactly one"), std::string::npos);
    EXPECT_NE(responses.front()->summary.find("coalesced"), std::string::npos);
    const auto coalescing = at(3930);
    ASSERT_EQ(coalescing.size(), 1u);
    EXPECT_EQ(coalescing.front()->testability, Testability::NotTestable);
    const auto successes = at(3933);
    ASSERT_EQ(successes.size(), 1u);
    EXPECT_NE(successes.front()->summary.find("each successful"), std::string::npos);
}

TEST_F(Draft21CatalogPart04Test, UnsuccessfulUpdateCleanupRetainsEachRequestRole) {
    const auto subscription = at(3914);
    ASSERT_EQ(subscription.size(), 1u);
    EXPECT_NE(subscription.front()->summary.find("UPDATE_FAILED"), std::string::npos);
    const auto fetch = at(3925);
    ASSERT_EQ(fetch.size(), 1u);
    EXPECT_EQ(fetch.front()->evaluators,
              std::vector<std::string>{"d21-failed-fetch-update-resets-data-stream"});
    const auto namespaces = at(3927);
    ASSERT_EQ(namespaces.size(), 3u);
    EXPECT_EQ(namespaces[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(namespaces[1]->applicability, Applicability::Applicable);
    EXPECT_EQ(namespaces[2]->applicability, Applicability::NotApplicable);
    const auto prefix = at(3946);
    ASSERT_EQ(prefix.size(), 1u);
    EXPECT_EQ(prefix.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart04Test, SuccessfulSubscribeSeparatesResponseAndPermittedObjectDelivery) {
    const auto rows = at(4004);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_NE(rows[0]->summary.find("SUBSCRIBE_OK"), std::string::npos);
    EXPECT_NE(rows[1]->rationale.find("Forward"), std::string::npos);
    const auto uninterested = at(4094);
    ASSERT_EQ(uninterested.size(), 2u);
    for (const auto* row : uninterested) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart04Test, PublishDoneDoesNotMistakeArrivalOrderForSenderOrderingOrPrivateState) {
    const auto ordering = at(4122);
    ASSERT_EQ(ordering.size(), 2u);
    for (const auto* row : ordering) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
        EXPECT_NE(row->rationale.find("arrival"), std::string::npos);
    }
    for (const auto line : {4129u, 4180u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto no_streams = at(4177);
    ASSERT_EQ(no_streams.size(), 1u);
    EXPECT_EQ(no_streams.front()->testability, Testability::Testable);
    EXPECT_NE(no_streams.front()->summary.find("0"), std::string::npos);
    const auto timeout = at(4180, 2);
    ASSERT_EQ(timeout.size(), 1u);
    EXPECT_EQ(timeout.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart04Test, PublishStateNotifyValidatesDirectionSubscriberControlAndKnownLargestObject) {
    const auto direction = at(4217);
    ASSERT_EQ(direction.size(), 2u);
    for (const auto* row : direction) {
        EXPECT_EQ(row->testability, Testability::Testable);
        EXPECT_NE(row->summary.find("PROTOCOL_VIOLATION"), std::string::npos);
    }
    const auto control = at(4225);
    ASSERT_EQ(control.size(), 1u);
    EXPECT_NE(control.front()->summary.find("unless the subscriber requested"), std::string::npos);
    const auto largest = at(4229);
    ASSERT_EQ(largest.size(), 1u);
    EXPECT_NE(largest.front()->summary.find("if known"), std::string::npos);
    EXPECT_NE(largest.front()->rationale.find("first byte"), std::string::npos);
    EXPECT_NE(largest.front()->rationale.find("queued"), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::requirements
