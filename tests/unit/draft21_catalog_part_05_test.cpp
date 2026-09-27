#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft21CatalogPart05Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(21, "draft21-lines-4261-5364.json"));
    }
};

TEST_F(Draft21CatalogPart05Test, CoversEveryOwnedAnchorWithContiguousClausesAndMatchingIds) {
    expect_partition_coverage(4261, 5364, 79);
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

TEST_F(Draft21CatalogPart05Test, IdsRemainGloballyUniqueAcrossCatalogsAndPartitions) {
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

TEST_F(Draft21CatalogPart05Test, FetchSeparatesInvalidRangesOrderingAndPrivateResponseTiming) {
    const auto invalid_range = at(4353);
    ASSERT_EQ(invalid_range.size(), 2u);
    for (const auto* row : invalid_range) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::Testable);
        EXPECT_NE(row->summary.find("INVALID_RANGE"), std::string::npos);
    }
    const auto order = at(4356);
    ASSERT_EQ(order.size(), 1u);
    EXPECT_NE(order.front()->rationale.find("Object ID"), std::string::npos);
    EXPECT_NE(order.front()->summary.find("descending"), std::string::npos);
    const auto early_objects = at(4383);
    ASSERT_EQ(early_objects.size(), 1u);
    EXPECT_EQ(early_objects.front()->testability, Testability::NotTestable);
    const auto known_end = at(4385);
    ASSERT_EQ(known_end.size(), 1u);
    EXPECT_EQ(known_end.front()->testability, Testability::NotTestable);
    const auto subscriber = at(4407);
    ASSERT_EQ(subscriber.size(), 1u);
    EXPECT_EQ(subscriber.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart05Test, DiscoveryKeepsAuthorizationOrderingAndFlowControlRoles) {
    for (const auto line : {4512u, 4633u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_NE(rows.front()->summary.find("32"), std::string::npos);
    }
    for (const auto line : {4541u, 4663u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_NE(rows.front()->rationale.find("policy"), std::string::npos);
    }
    const auto ordering = at(4544);
    ASSERT_EQ(ordering.size(), 1u);
    EXPECT_NE(ordering.front()->summary.find("NAMESPACE_DONE"), std::string::npos);
    const auto reset = at(4551);
    ASSERT_EQ(reset.size(), 1u);
    EXPECT_EQ(reset.front()->strength, Strength::May);
    EXPECT_NE(reset.front()->rationale.find("flow control"), std::string::npos);
    for (const auto line : {4520u, 4546u, 4554u, 4643u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft21CatalogPart05Test, ParametersValidateOrderingUnknownTypesDuplicatesAndPayloadIdentity) {
    for (const auto line : {4732u, 4734u, 4765u, 4767u, 4776u, 4799u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
    }
    const auto duplicate = at(4778);
    ASSERT_EQ(duplicate.size(), 2u);
    EXPECT_EQ(duplicate[0]->testability, Testability::NotTestable);
    EXPECT_EQ(duplicate[1]->testability, Testability::Testable);
    EXPECT_NE(duplicate[1]->summary.find("PROTOCOL_VIOLATION"), std::string::npos);
    const auto immutable = at(4785);
    ASSERT_EQ(immutable.size(), 2u);
    EXPECT_NE(immutable[0]->summary.find("SUBSCRIBE"), std::string::npos);
    EXPECT_NE(immutable[1]->summary.find("FETCH"), std::string::npos);
}

TEST_F(Draft21CatalogPart05Test, OptionalParameterScopesKeepPublisherSendersSeparateFromSubscribers) {
    for (const auto line : {4824u, 4837u, 4847u, 4936u, 4946u, 4965u, 5215u, 5271u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u) << line;
        EXPECT_EQ(rows[0]->applicability, Applicability::Applicable);
        EXPECT_EQ(rows[1]->applicability, Applicability::NotApplicable);
    }
    for (const auto line : {4893u, 5128u, 5191u, 5292u, 5345u, 5355u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    for (const auto line : {4837u, 4847u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u);
        for (const auto* row : rows) {
            EXPECT_EQ(row->summary.find("subscription REQUEST_UPDATE"), std::string::npos);
            EXPECT_NE(row->summary.find("REQUEST_UPDATE"), std::string::npos);
        }
    }
    const auto token = at(4828);
    ASSERT_EQ(token.size(), 1u);
    EXPECT_NE(token.front()->summary.find("SUBSCRIBE_TRACKS"), std::string::npos);
    EXPECT_NE(token.front()->summary.find("PUBLISH"), std::string::npos);
}

TEST_F(Draft21CatalogPart05Test, FilterErrorsDistinguishSessionErrorsFromRequestErrors) {
    for (const auto line : {4956u, 5015u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_NE(rows.front()->summary.find("PROTOCOL_VIOLATION"), std::string::npos);
    }
    for (const auto line : {5068u, 5084u, 5114u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_NE(rows.front()->summary.find("INVALID_FILTER"), std::string::npos);
    }
    for (const auto line : {5083u, 5113u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft21CatalogPart05Test, FillScopeAllowsOnlyItsEightParametersAndIndependentDuplicates) {
    const auto allowed = at(5135);
    ASSERT_EQ(allowed.size(), 8u);
    const std::vector<std::string> names = {
        "FILL_TIMEOUT", "SUBSCRIBER_PRIORITY", "LOCATION_FILTER", "GROUP_ORDER",
        "SUBGROUP_FILTER", "OBJECTID_FILTER", "PRIORITY_FILTER", "OBJECT_PROPERTY_FILTER"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        EXPECT_NE(allowed[index]->summary.find(names[index]), std::string::npos);
        EXPECT_EQ(allowed[index]->applicability, Applicability::NotApplicable);
    }
    const auto invalid = at(5186);
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_NE(invalid.front()->summary.find("PROTOCOL_VIOLATION"), std::string::npos);
    const auto independent = at(5191);
    ASSERT_EQ(independent.size(), 1u);
    EXPECT_NE(independent.front()->rationale.find("not retained"), std::string::npos);
}

TEST_F(Draft21CatalogPart05Test, RelayTimeoutRulesDoNotBecomeOriginalPublisherDuties) {
    for (const auto line : {4331u, 4334u, 4443u, 4444u, 4841u, 4851u, 4861u,
                            4882u, 4883u, 4888u, 4900u, 4905u, 4908u, 4913u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft21CatalogPart05Test, ExpiresAndLargestObjectRespectAdvisoryTimeAndPublicationBoundary) {
    const auto expires = at(5215);
    ASSERT_EQ(expires.size(), 2u);
    EXPECT_NE(expires[0]->rationale.find("advisory"), std::string::npos);
    EXPECT_NE(expires[0]->rationale.find("SUBSCRIBE_OK has its own format"), std::string::npos);
    const auto renewal = at(5226);
    ASSERT_EQ(renewal.size(), 2u);
    EXPECT_EQ(renewal[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(renewal[1]->applicability, Applicability::NotApplicable);
    const auto largest = at(5243);
    ASSERT_EQ(largest.size(), 1u);
    EXPECT_EQ(largest.front()->strength, Strength::Must);
    EXPECT_EQ(largest.front()->testability, Testability::Testable);
    EXPECT_NE(largest.front()->summary.find("LARGEST_OBJECT"), std::string::npos);
    EXPECT_NE(largest.front()->rationale.find("first byte"), std::string::npos);
    EXPECT_NE(largest.front()->rationale.find("queued"), std::string::npos);
    const auto relay = at(5248);
    ASSERT_EQ(relay.size(), 1u);
    EXPECT_EQ(relay.front()->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart05Test, ForwardAndNewGroupRequestsRetainTheirConditions) {
    const auto forward = at(5277);
    ASSERT_EQ(forward.size(), 1u);
    EXPECT_NE(forward.front()->summary.find("0 or 1"), std::string::npos);
    EXPECT_NE(forward.front()->summary.find("PROTOCOL_VIOLATION"), std::string::npos);
    const auto next = at(5304);
    ASSERT_EQ(next.size(), 2u);
    for (const auto* row : next) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->strength, Strength::Should);
        EXPECT_NE(row->summary.find("dynamic Groups"), std::string::npos);
        EXPECT_NE(row->rationale.find("deadline"), std::string::npos);
    }
    const auto delay = at(5305);
    ASSERT_EQ(delay.size(), 1u);
    EXPECT_EQ(delay.front()->testability, Testability::NotTestable);
    for (const auto line : {5295u, 5297u, 5314u, 5327u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft21CatalogPart05Test, PrefixOverlapChecksRemainSeparateForBothDiscoveryTypes) {
    for (const auto line : {4530u, 4659u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_NE(rows.front()->summary.find("PREFIX_OVERLAP"), std::string::npos);
        EXPECT_NE(rows.front()->rationale.find("independent"), std::string::npos);
    }
    const auto updates = at(5349);
    ASSERT_EQ(updates.size(), 2u);
    EXPECT_NE(updates[0]->summary.find("SUBSCRIBE_NAMESPACE"), std::string::npos);
    EXPECT_NE(updates[1]->summary.find("SUBSCRIBE_TRACKS"), std::string::npos);
    for (const auto* row : updates) {
        EXPECT_NE(row->summary.find("same session"), std::string::npos);
        EXPECT_NE(row->summary.find("PREFIX_OVERLAP"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart05Test, IncludePropertiesSeparatesOkResponseAndResultingPublishBehavior) {
    const auto omit = at(5360);
    ASSERT_EQ(omit.size(), 2u);
    EXPECT_NE(omit[0]->summary.find("OK"), std::string::npos);
    EXPECT_NE(omit[1]->summary.find("PUBLISH"), std::string::npos);
    for (const auto* row : omit) EXPECT_EQ(row->strength, Strength::Should);
    const auto invalid = at(5362);
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_NE(invalid.front()->summary.find("0 or 1"), std::string::npos);
    EXPECT_NE(invalid.front()->summary.find("PROTOCOL_VIOLATION"), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::requirements
