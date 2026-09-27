#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft18CatalogPart03Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(18, "draft18-lines-2565-3508.json"));
    }
};

TEST_F(Draft18CatalogPart03Test, CoversAllOwnedOccurrencesWithoutConflictsOrForeignAnchors) {
    expect_partition_coverage(2565, 3508, 107);
}

TEST_F(Draft18CatalogPart03Test, RequirementIdsAreUniqueAcrossAllThreePartitions) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    std::set<std::string> ids;
    for (const auto* filename : {"draft18-lines-0001-1254.json", "draft18-lines-1255-2564.json",
                                "draft18-lines-2565-3508.json"}) {
        const auto part = RequirementCatalog::load(
            source_, root / "requirements/parts" / filename, CatalogLoadMode::AllowIncomplete);
        for (const auto& row : part.requirements) {
            EXPECT_TRUE(ids.insert(row.id).second) << row.id;
        }
    }
}

TEST_F(Draft18CatalogPart03Test, CachingAndOpaqueForwardingRemainRelayObligations) {
    for (const auto line : {2586u, 2614u, 2876u, 2879u, 2880u}) {
        const auto rows = at(line);
        ASSERT_FALSE(rows.empty());
        for (const auto* row : rows) {
            EXPECT_EQ(row->actor, "relay");
            EXPECT_EQ(row->applicability, Applicability::NotApplicable);
            EXPECT_EQ(row->testability, Testability::NotApplicable);
        }
    }
    const auto late_object = at(2614);
    ASSERT_EQ(late_object.size(), 2u);
    EXPECT_EQ(late_object[0]->strength, Strength::ShouldNot);
    EXPECT_EQ(late_object[1]->strength, Strength::ShouldNot);
    const auto modification = at(2879, 2);
    ASSERT_EQ(modification.size(), 3u);
    for (const auto* row : modification) EXPECT_EQ(row->strength, Strength::MustNot);
}

TEST_F(Draft18CatalogPart03Test, MultiplePublisherAndAuthorizationRulesDoNotReassignRelayDuties) {
    for (const auto line : {2652u, 2654u, 2661u, 2737u, 2739u, 2778u, 2781u, 2786u}) {
        const auto rows = at(line);
        ASSERT_FALSE(rows.empty());
        for (const auto* row : rows) {
            EXPECT_EQ(row->actor, "relay");
            EXPECT_EQ(row->applicability, Applicability::NotApplicable);
        }
    }
    const auto namespace_route = at(2751);
    ASSERT_EQ(namespace_route.size(), 1u);
    EXPECT_EQ(namespace_route.front()->actor, "publisher");
    EXPECT_EQ(namespace_route.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(namespace_route.front()->evaluators,
              std::vector<std::string>{"explicit-publish-namespace-sent"});
}

TEST_F(Draft18CatalogPart03Test, PublisherFacingRelayForwardUpdatesKeepTheirRelayActor) {
    for (const auto line : {2622u, 2729u, 2808u, 2813u, 2830u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "relay");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    const auto forward = at(3434);
    ASSERT_EQ(forward.size(), 1u);
    EXPECT_EQ(forward.front()->actor, "publisher");
    EXPECT_EQ(forward.front()->strength, Strength::May);
    EXPECT_EQ(forward.front()->evaluators,
              std::vector<std::string>{"publish-forward-value-controls-initial-object-delivery"});
    const auto invalid = at(3439);
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_EQ(invalid.front()->scenarios,
              std::vector<std::string>{"receive-forward-outside-zero-one"});
    EXPECT_EQ(invalid.front()->evaluators,
              std::vector<std::string>{"session-closed-protocol-violation"});
}

TEST_F(Draft18CatalogPart03Test, FirstRequestMessagesHaveRoleSpecificStreamPlacement) {
    const auto rows = at(2900);
    ASSERT_EQ(rows.size(), 7u);
    const std::vector<Applicability> applicability = {
        Applicability::NotApplicable, Applicability::Applicable, Applicability::NotApplicable,
        Applicability::Applicable, Applicability::Applicable, Applicability::NotApplicable,
        Applicability::NotApplicable};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        EXPECT_EQ(rows[index]->applicability, applicability[index]);
        EXPECT_EQ(rows[index]->strength, Strength::Must);
        if (applicability[index] == Applicability::Applicable) {
            EXPECT_EQ(rows[index]->evaluators,
                      std::vector<std::string>{"request-is-first-message-on-new-bidirectional-stream"});
        }
    }
}

TEST_F(Draft18CatalogPart03Test, RequestIdParityAndSessionWideReuseAreSeparateViolations) {
    const auto rows = at(2998);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]->scenarios, std::vector<std::string>{"receive-request-id-wrong-peer-parity"});
    EXPECT_EQ(rows[1]->scenarios,
              std::vector<std::string>{"receive-duplicate-request-id-across-request-streams"});
    for (const auto* row : rows) {
        EXPECT_EQ(row->actor, "endpoint");
        EXPECT_EQ(row->evaluators, std::vector<std::string>{"session-closed-invalid-request-id"});
    }
}

TEST_F(Draft18CatalogPart03Test, UnknownMessageAndParameterErrorsRemainDistinctFromSetupOptions) {
    const auto unknown_message = at(2973);
    ASSERT_EQ(unknown_message.size(), 1u);
    EXPECT_EQ(unknown_message.front()->evaluators, std::vector<std::string>{"session-closed"});
    for (const auto line : {2978u, 3017u, 3043u, 3366u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->evaluators,
                  std::vector<std::string>{"session-closed-protocol-violation"});
    }
    const auto parameter = at(3043);
    EXPECT_NE(parameter.front()->rationale.find("Setup Options"), std::string::npos);
    const auto scope = at(3076);
    ASSERT_EQ(scope.size(), 1u);
    EXPECT_EQ(scope.front()->evaluators,
              std::vector<std::string>{"connection-closed-protocol-violation"});
    const auto setup_alias = at(3142);
    ASSERT_EQ(setup_alias.size(), 2u);
    for (const auto* row : setup_alias) {
        EXPECT_EQ(row->actor, "server");
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
        EXPECT_TRUE(row->scenarios.empty());
        EXPECT_TRUE(row->evaluators.empty());
    }
}

TEST_F(Draft18CatalogPart03Test, AuthorizationStateIsObservedThroughSubsequentRequests) {
    const auto rejected_registration = at(3172);
    ASSERT_EQ(rejected_registration.size(), 1u);
    EXPECT_EQ(rejected_registration.front()->scenarios,
              std::vector<std::string>{"register-token-in-rejected-request-then-use-alias"});
    EXPECT_EQ(rejected_registration.front()->evaluators,
              std::vector<std::string>{"rejected-request-token-alias-still-registered"});
    const auto cache = at(3221);
    ASSERT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.front()->evaluators,
              std::vector<std::string>{"session-closed-auth-token-cache-overflow"});
    EXPECT_NE(cache.front()->rationale.find("10.3.1.4"), std::string::npos);
    for (const auto line : {3176u, 3199u, 3211u, 3212u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
}

TEST_F(Draft18CatalogPart03Test, AliasStreamWordingAmbiguityDoesNotInventAWireOracle) {
    for (const auto line : {3232u, 3234u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
        EXPECT_NE(rows.front()->rationale.find("control streams"), std::string::npos);
        EXPECT_NE(rows.front()->rationale.find("request streams"), std::string::npos);
    }
}

TEST_F(Draft18CatalogPart03Test, PayloadInvarianceAndPublishedLocationArePublisherObligations) {
    const auto payload = at(3061);
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload.front()->actor, "publisher");
    EXPECT_EQ(payload.front()->evaluators,
              std::vector<std::string>{"same-object-payload-independent-of-message-parameters"});
    const auto location = at(3411);
    ASSERT_EQ(location.size(), 4u);
    for (std::size_t index = 0; index < location.size(); ++index) {
        EXPECT_EQ(location[index]->actor, "publisher");
        EXPECT_EQ(location[index]->applicability, Applicability::Applicable);
        if (index < 2) {
            EXPECT_EQ(location[index]->evaluators,
                      std::vector<std::string>{"largest-object-included-after-object-publication"});
        } else {
            EXPECT_EQ(location[index]->testability, Testability::NotTestable);
        }
    }
    EXPECT_NE(location.front()->rationale.find("REQUEST_UPDATE_OK"), std::string::npos);
}

TEST_F(Draft18CatalogPart03Test, NamespacePrefixUpdateValidationAppliesToPublishingResponder) {
    const auto rows = at(3506);
    ASSERT_EQ(rows.size(), 2u);
    for (const auto* row : rows) {
        EXPECT_EQ(row->actor, "publisher");
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->evaluators, std::vector<std::string>{"request-error-prefix-overlap"});
    }
}

}  // namespace
}  // namespace moq::interop::requirements
