#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft21CatalogPart02Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(21, "draft21-lines-1648-2430.json"));
    }
};

TEST_F(Draft21CatalogPart02Test, CoversEveryOwnedAnchorWithContiguousClausesAndDraft21Ids) {
    expect_partition_coverage(1648, 2430, 53);
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

TEST_F(Draft21CatalogPart02Test, IdsAreUniqueAcrossBothDraftCatalogsAndEveryExistingPartition) {
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

TEST_F(Draft21CatalogPart02Test, SchedulingAdviceIsApplicableButNotAWireOrderingGuarantee) {
    const std::map<std::size_t, std::size_t> counts = {
        {1694, 1}, {1719, 8}, {1780, 2}, {1782, 1}, {1820, 1}};
    for (const auto& [line, count] : counts) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), count) << line;
        for (const auto* row : rows) {
            EXPECT_EQ(row->applicability, Applicability::Applicable);
            EXPECT_EQ(row->testability, Testability::NotTestable);
        }
    }
    const auto algorithm = at(1719);
    ASSERT_EQ(algorithm.size(), 8u);
    EXPECT_NE(algorithm[3]->summary.find("Group Order differs"), std::string::npos);
    EXPECT_NE(algorithm[4]->summary.find("fill-delivered"), std::string::npos);
    EXPECT_NE(algorithm[7]->summary.find("datagram"), std::string::npos);
}

TEST_F(Draft21CatalogPart02Test, RelayPriorityAndReplayPoliciesAreOutsidePublisherScope) {
    for (const auto line : {1799u, 1802u, 2205u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u) << line;
        for (const auto* row : rows) {
            EXPECT_EQ(row->actor, "relay");
            EXPECT_EQ(row->applicability, Applicability::NotApplicable);
        }
    }
}

TEST_F(Draft21CatalogPart02Test, TimerBookkeepingAndTransportQueueAdviceRemainInternal) {
    for (const auto line : {1854u, 1859u, 1865u, 1867u, 1874u, 1880u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto timestamp = at(1854);
    ASSERT_EQ(timestamp.size(), 1u);
    EXPECT_NE(timestamp.front()->summary.find("last header byte"), std::string::npos);
    const auto start = at(1880);
    ASSERT_EQ(start.size(), 1u);
    EXPECT_NE(start.front()->summary.find("application"), std::string::npos);
}

TEST_F(Draft21CatalogPart02Test, ExpiredDeliveryHasDistinctResetDropAndReopenOutcomes) {
    const std::map<std::size_t, std::string> evaluators = {
        {1862, "d21-expired-object-subgroup-delivery-timeout-reset"},
        {1863, "d21-no-reopen-after-object-delivery-timeout"},
        {1872, "d21-expired-object-datagram-dropped"},
        {1887, "d21-uncommitted-subgroup-timeout-reset"}};
    for (const auto& [line, evaluator] : evaluators) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{evaluator});
    }
    const auto subgroup = at(1887);
    ASSERT_EQ(subgroup.size(), 1u);
    EXPECT_NE(subgroup.front()->rationale.find("does not specify a reset code"), std::string::npos);
    const auto object = at(1862);
    ASSERT_EQ(object.size(), 1u);
    EXPECT_NE(object.front()->rationale.find("flow control alone cannot"), std::string::npos);
    const auto datagram = at(1872);
    ASSERT_EQ(datagram.size(), 1u);
    EXPECT_NE(datagram.front()->rationale.find("independent admission evidence"), std::string::npos);
}

TEST_F(Draft21CatalogPart02Test, SlowSubscriberTerminationUsesPublishDoneTooFarBehind) {
    const auto rows = at(1933);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->strength, Strength::May);
    EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"d21-too-far-behind-publish-done"});
    EXPECT_NE(rows.front()->summary.find("resource limits"), std::string::npos);
}

TEST_F(Draft21CatalogPart02Test, UriAndFragmentValidationIsLocalAndDoesNotInventWireErrors) {
    for (const auto line : {1946u, 1970u, 1974u, 1980u, 2022u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto authority = at(1946);
    ASSERT_EQ(authority.size(), 1u);
    EXPECT_EQ(authority.front()->strength, Strength::MustNot);
    EXPECT_NE(authority.front()->summary.find("empty host"), std::string::npos);
    const auto fragment = at(1974);
    ASSERT_EQ(fragment.size(), 1u);
    EXPECT_NE(fragment.front()->summary.find("registered"), std::string::npos);
    EXPECT_NE(fragment.front()->rationale.find("initially empty"), std::string::npos);
    const auto alphabet = at(1980);
    ASSERT_EQ(alphabet.size(), 1u);
    EXPECT_NE(alphabet.front()->summary.find("a-z, 0-9, -"), std::string::npos);
}

TEST_F(Draft21CatalogPart02Test, TransportChoicesAndQuicDatagramNegotiationUseDraft21Constraints) {
    const auto choices = at(2030);
    ASSERT_EQ(choices.size(), 2u);
    EXPECT_EQ(choices[0]->evaluators, std::vector<std::string>{"d21-native-quic-session-capability"});
    EXPECT_EQ(choices[1]->evaluators, std::vector<std::string>{"d21-webtransport-session-capability"});
    const auto datagram = at(2048);
    ASSERT_EQ(datagram.size(), 2u);
    for (const auto* row : datagram) {
        EXPECT_EQ(row->strength, Strength::Must);
        EXPECT_EQ(row->testability, Testability::Testable);
        EXPECT_NE(row->summary.find("QUIC DATAGRAM"), std::string::npos);
        EXPECT_NE(row->rationale.find("HTTP/3"), std::string::npos);
    }
    EXPECT_NE(choices[1]->rationale.find("TCP+TLS"), std::string::npos);
}

TEST_F(Draft21CatalogPart02Test, RequestStreamOpenersAndViolationResponsesAreSeparate) {
    const auto opener = at(2111);
    ASSERT_EQ(opener.size(), 1u);
    EXPECT_EQ(opener.front()->strength, Strength::MustNot);
    for (const auto* type : {"TRACK_STATUS", "SUBSCRIBE", "PUBLISH", "FETCH",
                             "PUBLISH_NAMESPACE", "SUBSCRIBE_NAMESPACE", "SUBSCRIBE_TRACKS"}) {
        EXPECT_NE(opener.front()->summary.find(type), std::string::npos);
    }
    EXPECT_NE(opener.front()->summary.find("negotiated"), std::string::npos);
    const auto rejection = at(2112);
    ASSERT_EQ(rejection.size(), 1u);
    EXPECT_EQ(rejection.front()->evaluators,
              std::vector<std::string>{"d21-invalid-request-stream-opener-protocol-violation"});
}

TEST_F(Draft21CatalogPart02Test, EarlyBufferingSeparatesSubscriberObjectsFromPublisherRequests) {
    const auto buffer = at(2123);
    ASSERT_EQ(buffer.size(), 2u);
    EXPECT_EQ(buffer[0]->applicability, Applicability::NotApplicable);
    EXPECT_EQ(buffer[1]->applicability, Applicability::Applicable);
    EXPECT_EQ(buffer[1]->testability, Testability::NotTestable);
    const auto reset = at(2133);
    ASSERT_EQ(reset.size(), 1u);
    EXPECT_EQ(reset.front()->strength, Strength::May);
    EXPECT_EQ(reset.front()->evaluators, std::vector<std::string>{"d21-pre-setup-request-stream-reset"});
}

TEST_F(Draft21CatalogPart02Test, ControlStreamLifetimeAndSetupNegotiationRetainTheirConditions) {
    const auto lifetime = at(2137);
    ASSERT_EQ(lifetime.size(), 1u);
    EXPECT_EQ(lifetime.front()->strength, Strength::MustNot);
    EXPECT_EQ(lifetime.front()->evaluators, std::vector<std::string>{"d21-control-stream-open-during-session"});
    const auto early = at(2143);
    ASSERT_EQ(early.size(), 1u);
    EXPECT_EQ(early.front()->strength, Strength::ShouldNot);
    EXPECT_NE(early.front()->summary.find("application requires"), std::string::npos);
    EXPECT_NE(early.front()->summary.find("knows"), std::string::npos);
    const auto setup = at(2216);
    ASSERT_EQ(setup.size(), 1u);
    EXPECT_EQ(setup.front()->evaluators, std::vector<std::string>{"d21-required-version-setup-options"});
    for (const auto line : {2224u, 2225u}) {
        const auto authors = at(line);
        ASSERT_EQ(authors.size(), 1u);
        EXPECT_EQ(authors.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft21CatalogPart02Test, UnknownStreamClosureDoesNotInventAnErrorAndInvalidIdsDoSpecifyOne) {
    const auto stream = at(2259);
    ASSERT_EQ(stream.size(), 1u);
    EXPECT_EQ(stream.front()->evaluators, std::vector<std::string>{"d21-unknown-stream-type-session-close"});
    EXPECT_NE(stream.front()->rationale.find("does not name"), std::string::npos);
    const auto ids = at(2285);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0]->evaluators, std::vector<std::string>{"d21-wrong-parity-invalid-request-id"});
    EXPECT_EQ(ids[1]->evaluators, std::vector<std::string>{"d21-duplicate-invalid-request-id"});
}

TEST_F(Draft21CatalogPart02Test, RequiredMessagesPrecedeFinAndPublishCannotUseImmediateRequesterFin) {
    for (const auto line : {2303u, 2305u, 2307u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_NE(rows.front()->summary.find("FIN"), std::string::npos);
    }
    const auto done = at(2307);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done.front()->evaluators, std::vector<std::string>{"d21-publish-done-before-request-fin"});
    const auto reciprocal = at(2310);
    ASSERT_EQ(reciprocal.size(), 1u);
    EXPECT_EQ(reciprocal.front()->evaluators, std::vector<std::string>{"d21-requester-fin-after-responder-completion"});
    const auto prompt = at(2315);
    ASSERT_EQ(prompt.size(), 1u);
    EXPECT_EQ(prompt.front()->testability, Testability::NotTestable);
    const auto immediate = at(2318);
    ASSERT_EQ(immediate.size(), 1u);
    EXPECT_NE(immediate.front()->summary.find("except a sender of PUBLISH"), std::string::npos);
    EXPECT_NE(immediate.front()->summary.find("REQUEST_UPDATE"), std::string::npos);
}

TEST_F(Draft21CatalogPart02Test, CancellationAndUnprocessedRejectionKeepIndependentTransportActions) {
    const auto cancel = at(2323);
    ASSERT_EQ(cancel.size(), 2u);
    EXPECT_EQ(cancel[0]->evaluators, std::vector<std::string>{"d21-cancel-request-reset-open-send-direction"});
    EXPECT_EQ(cancel[1]->evaluators, std::vector<std::string>{"d21-cancel-request-stop-open-receive-direction"});
    EXPECT_NE(cancel[1]->summary.find("already sent FIN"), std::string::npos);
    const auto reject = at(2334);
    ASSERT_EQ(reject.size(), 2u);
    EXPECT_EQ(reject[0]->evaluators, std::vector<std::string>{"d21-unprocessed-rejection-request-error"});
    EXPECT_EQ(reject[1]->evaluators, std::vector<std::string>{"d21-unprocessed-rejection-fin"});
}

TEST_F(Draft21CatalogPart02Test, SessionNamespacePublicationAndRejectionsHaveDistinctActors) {
    const auto application = at(2346);
    ASSERT_EQ(application.size(), 2u);
    for (const auto* row : application) {
        EXPECT_EQ(row->strength, Strength::MustNot);
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::Testable);
    }
    const auto relay = at(2347);
    ASSERT_EQ(relay.size(), 2u);
    for (const auto* row : relay) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    const auto empty = at(2359);
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty.front()->evaluators, std::vector<std::string>{"d21-session-empty-track-does-not-exist"});
    const auto unknown = at(2363);
    ASSERT_EQ(unknown.size(), 4u);
    EXPECT_EQ(unknown[0]->evaluators, std::vector<std::string>{"d21-session-unknown-track-does-not-exist"});
    EXPECT_EQ(unknown[1]->evaluators, std::vector<std::string>{"d21-session-unknown-namespace-does-not-exist"});
    EXPECT_EQ(unknown[2]->testability, Testability::NotTestable);
    EXPECT_EQ(unknown[3]->testability, Testability::NotTestable);
}

TEST_F(Draft21CatalogPart02Test, SessionErrorPolicyAndGracefulMigrationRetainRoleAndTimeoutBoundaries) {
    const auto policy = at(2381);
    ASSERT_EQ(policy.size(), 1u);
    EXPECT_EQ(policy.front()->strength, Strength::May);
    EXPECT_EQ(policy.front()->testability, Testability::NotTestable);
    const auto timeout = at(2413);
    ASSERT_EQ(timeout.size(), 1u);
    EXPECT_EQ(timeout.front()->evaluators, std::vector<std::string>{"d21-goaway-timeout-session-close"});
    EXPECT_NE(timeout.front()->rationale.find("zero"), std::string::npos);
    EXPECT_NE(timeout.front()->rationale.find("early"), std::string::npos);
    const auto relay = at(2417);
    ASSERT_EQ(relay.size(), 1u);
    EXPECT_EQ(relay.front()->applicability, Applicability::NotApplicable);
    const auto client = at(2421);
    ASSERT_EQ(client.size(), 1u);
    EXPECT_EQ(client.front()->actor, "client endpoint");
    EXPECT_EQ(client.front()->strength, Strength::Should);
    EXPECT_EQ(client.front()->evaluators, std::vector<std::string>{"d21-goaway-client-drain-before-no-error-close"});
}

}  // namespace
}  // namespace moq::interop::requirements
