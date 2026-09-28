#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft21CatalogPart06Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(21, "draft21-lines-5365-6710.json"));
    }
};

TEST_F(Draft21CatalogPart06Test, CoversEveryOwnedAnchorWithContiguousClausesAndMatchingIds) {
    expect_partition_coverage(5365, 6710, 93);
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

TEST_F(Draft21CatalogPart06Test, IdsRemainGloballyUniqueAcrossCatalogsAndPartitions) {
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

TEST_F(Draft21CatalogPart06Test, PropertyMutationAndSingletonRulesKeepOriginalPublisherDuties) {
    for (const auto line : {5469u, 5561u, 5596u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u) << line;
        for (const auto* row : rows) {
            EXPECT_EQ(row->applicability, Applicability::Applicable);
            EXPECT_EQ(row->testability, Testability::Testable);
            EXPECT_EQ(row->strength, Strength::MustNot);
        }
        EXPECT_NE(rows[0]->summary.find("modify"), std::string::npos);
        EXPECT_NE(rows[1]->summary.find("remove"), std::string::npos);
    }
    for (const auto line : {5517u, 5563u, 5605u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_NE(rows.front()->summary.find("one"), std::string::npos);
    }
    const auto encoding = at(5471);
    ASSERT_EQ(encoding.size(), 1u);
    EXPECT_NE(encoding.front()->summary.find("serialization"), std::string::npos);
}

TEST_F(Draft21CatalogPart06Test, PropertyReceiversRequireAReachablePublisherContext) {
    for (const auto line : {5402u, 5440u, 5451u, 5468u, 5472u, 5473u, 5557u, 5558u, 5592u, 5593u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    const auto cache = at(5471, 2);
    ASSERT_EQ(cache.size(), 2u);
    for (const auto* row : cache) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    const auto search = at(5479);
    ASSERT_EQ(search.size(), 2u);
    for (const auto* row : search) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_NE(row->rationale.find("filter"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart06Test, ObjectPayloadStatusAndMetadataRetainTheirActorAndIntentBoundaries) {
    for (const auto line : {5673u, 5697u, 5703u, 5704u, 5721u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    const auto payload = at(5705);
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload.front()->applicability, Applicability::Applicable);
    EXPECT_NE(payload.front()->rationale.find("0x0"), std::string::npos);
    for (const auto line : {5725u, 5730u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
        EXPECT_NE(rows.front()->rationale.find("intent"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart06Test, DatagramAndSubgroupFlagsSeparateSendersFromDataReceivers) {
    for (const auto line : {5850u, 5968u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_NE(rows.front()->summary.find("bit 4"), std::string::npos);
    }
    for (const auto line : {5808u, 5856u, 5985u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    for (const auto line : {5798u, 5866u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u);
        for (const auto* row : rows) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    }
    for (const auto line : {5844u, 5962u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 3u);
        for (const auto* row : rows) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    }
    const auto unknown = at(5619);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown.front()->applicability, Applicability::Applicable);
    EXPECT_NE(unknown.front()->rationale.find("padding"), std::string::npos);
}

TEST_F(Draft21CatalogPart06Test, SenderInternalOrderingAndFinVisibilityAreNotInferredFromArrival) {
    for (const auto line : {5868u, 5890u, 6352u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto finish = at(6021);
    ASSERT_EQ(finish.size(), 1u);
    EXPECT_EQ(finish.front()->testability, Testability::Testable);
    EXPECT_NE(finish.front()->rationale.find("Start Location"), std::string::npos);
    const auto incomplete = at(6024);
    ASSERT_EQ(incomplete.size(), 1u);
    EXPECT_NE(incomplete.front()->rationale.find("sender"), std::string::npos);
    EXPECT_NE(incomplete.front()->rationale.find("subsequent reset"), std::string::npos);
    const auto reliable = at(6038);
    ASSERT_EQ(reliable.size(), 1u);
    EXPECT_NE(reliable.front()->summary.find("reliable_size"), std::string::npos);
}

TEST_F(Draft21CatalogPart06Test, StopSendingRetainsResetAndForwardStateException) {
    const auto subscriber = at(6123);
    ASSERT_EQ(subscriber.size(), 1u);
    EXPECT_EQ(subscriber.front()->applicability, Applicability::NotApplicable);
    for (const auto line : {6125u, 6137u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
    }
    const auto no_reopen = at(6134);
    ASSERT_EQ(no_reopen.size(), 1u);
    EXPECT_EQ(no_reopen.front()->strength, Strength::ShouldNot);
    EXPECT_EQ(no_reopen.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(no_reopen.front()->testability, Testability::NotTestable);
    EXPECT_EQ(at(6137).front()->strength, Strength::May);
    EXPECT_NE(at(6137).front()->summary.find("0 to 1"), std::string::npos);
}

TEST_F(Draft21CatalogPart06Test, FetchFirstObjectAndDatagramFlagsKeepSeparateObligations) {
    const auto first = at(6277);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_NE(first[0]->summary.find("Group ID Delta"), std::string::npos);
    EXPECT_NE(first[1]->summary.find("Object ID Delta"), std::string::npos);
    for (const auto* row : first) EXPECT_EQ(row->applicability, Applicability::Applicable);
    for (const auto line : {6280u, 6298u, 6306u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
    const auto group_bounds = at(6289);
    ASSERT_EQ(group_bounds.size(), 2u);
    for (const auto* row : group_bounds) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    const auto datagram = at(6305);
    ASSERT_EQ(datagram.size(), 1u);
    EXPECT_EQ(datagram.front()->strength, Strength::Must);
    EXPECT_NE(datagram.front()->summary.find("0x40"), std::string::npos);
    const auto low_bits = at(6305, 2);
    ASSERT_EQ(low_bits.size(), 1u);
    EXPECT_EQ(low_bits.front()->strength, Strength::Should);
    const auto range = at(6316);
    ASSERT_EQ(range.size(), 1u);
    EXPECT_EQ(range.front()->strength, Strength::ShouldNot);
    EXPECT_NE(range.front()->summary.find("unknown or timed out"), std::string::npos);
}

TEST_F(Draft21CatalogPart06Test, PaddingCanReachPublisherInBothDirectionsWithoutObjectState) {
    const auto permission = at(6347);
    ASSERT_EQ(permission.size(), 2u);
    const auto cancellation = at(6372);
    ASSERT_EQ(cancellation.size(), 2u);
    for (const auto line : {6358u, 6360u, 6369u, 6377u, 6379u, 6396u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
    }
    for (const auto* row : cancellation) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_NE(row->summary.find("state"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart06Test, MalformedTrackHandlingRemainsSubscriberAndRelayWork) {
    for (const auto line : {6445u, 6448u, 6504u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 2u);
        for (const auto* row : rows) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    }
    for (const auto line : {6447u, 6451u, 6583u, 6585u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    }
}

TEST_F(Draft21CatalogPart06Test, ErrorAdviceIsConditionalAndRedirectsIncludePublishingRequesters) {
    for (const auto line : {6141u, 6456u, 6534u, 6568u, 6638u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->strength, Strength::Should);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
    }
    const auto goaway = at(6564);
    ASSERT_EQ(goaway.size(), 1u);
    EXPECT_EQ(goaway.front()->strength, Strength::May);
    const auto redirect = at(6576);
    ASSERT_EQ(redirect.size(), 2u);
    for (const auto* row : redirect) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_NE(row->rationale.find("PUBLISH"), std::string::npos);
    }
    const auto retry = at(6578);
    ASSERT_EQ(retry.size(), 1u);
    EXPECT_EQ(retry.front()->strength, Strength::ShouldNot);
    EXPECT_NE(retry.front()->summary.find("as sent"), std::string::npos);
    const auto reset = at(6677);
    ASSERT_EQ(reset.size(), 2u);
    EXPECT_NE(reset[0]->summary.find("reset"), std::string::npos);
    EXPECT_NE(reset[1]->summary.find("STOP_SENDING"), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::requirements
