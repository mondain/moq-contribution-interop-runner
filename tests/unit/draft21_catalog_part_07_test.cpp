#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft21CatalogPart07Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(21, "draft21-lines-6711-8904.json"));
    }
};

TEST_F(Draft21CatalogPart07Test, CoversEveryOwnedAnchorWithContiguousClausesAndMatchingIds) {
    expect_partition_coverage(6711, 8904, 33);
    EXPECT_EQ(catalog_.draft, 21u);
    EXPECT_EQ(catalog_.source_sha256, source_.sha256);
    const std::map<Strength, std::string> tokens = {
        {Strength::Must, "MUST"}, {Strength::MustNot, "MUST-NOT"},
        {Strength::Should, "SHOULD"}, {Strength::ShouldNot, "SHOULD-NOT"},
        {Strength::May, "MAY"}};
    for (const auto& row : catalog_.requirements) {
        SCOPED_TRACE(row.id);
        auto section = row.source.section;
        std::replace(section.begin(), section.end(), '.', '-');
        EXPECT_TRUE(row.id.starts_with("D21-" + section + "-" + tokens.at(row.strength) + "-"));
        EXPECT_FALSE(row.actor.empty());
        EXPECT_FALSE(row.summary.empty());
        EXPECT_FALSE(row.rationale.empty());
    }
}

TEST_F(Draft21CatalogPart07Test, IdsRemainGloballyUniqueAcrossDraft21CatalogAndPartitions) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    std::set<std::string> ids;
    std::vector<std::filesystem::path> paths = {root / "requirements/draft21.json"};
    for (const auto& entry : std::filesystem::directory_iterator(root / "requirements/parts")) {
        if (entry.is_regular_file() && entry.path().extension() == ".json" &&
            entry.path().filename().string().starts_with("draft21-")) {
            paths.push_back(entry.path());
        }
    }
    for (const auto& path : paths) {
        const auto part = RequirementCatalog::load(source_, path, CatalogLoadMode::AllowIncomplete);
        for (const auto& row : part.requirements) EXPECT_TRUE(ids.insert(row.id).second) << row.id;
    }
}

TEST_F(Draft21CatalogPart07Test, GreaseRulesPreserveRegistryAndPublisherReceiverBoundaries) {
    for (const auto line : {6745u, 6746u, 6750u, 7273u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
    }
    EXPECT_NE(at(6745).front()->rationale.find("Auth Token Type"), std::string::npos);
    EXPECT_NE(at(6746).front()->rationale.find("Message Parameters"), std::string::npos);
    const auto properties = at(6753);
    ASSERT_EQ(properties.size(), 1u);
    EXPECT_EQ(properties.front()->applicability, Applicability::NotApplicable);
    EXPECT_NE(properties.front()->rationale.find("Mandatory Track"), std::string::npos);

    const auto errors = at(6756);
    ASSERT_EQ(errors.size(), 4u);
    EXPECT_NE(errors[0]->summary.find("Session Termination"), std::string::npos);
    EXPECT_NE(errors[1]->summary.find("REQUEST_ERROR"), std::string::npos);
    EXPECT_NE(errors[2]->summary.find("PUBLISH_DONE"), std::string::npos);
    EXPECT_NE(errors[3]->summary.find("stream"), std::string::npos);
    for (const auto index : {0u, 1u, 3u}) {
        EXPECT_EQ(errors[index]->applicability, Applicability::Applicable);
        EXPECT_EQ(errors[index]->testability, Testability::NotTestable);
        EXPECT_NE(errors[index]->summary.find("INTERNAL_ERROR"), std::string::npos);
    }
    EXPECT_EQ(errors[2]->applicability, Applicability::NotApplicable);
    const auto no_close = at(6758);
    ASSERT_EQ(no_close.size(), 2u);
    EXPECT_EQ(no_close[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(no_close[0]->testability, Testability::Testable);
    EXPECT_NE(no_close[0]->summary.find("REQUEST_ERROR"), std::string::npos);
    EXPECT_EQ(no_close[1]->applicability, Applicability::NotApplicable);
    EXPECT_NE(no_close[1]->summary.find("PUBLISH_DONE"), std::string::npos);
}

TEST_F(Draft21CatalogPart07Test, UnknownStopSendingExercisesBothGreaseRulesWithoutScoringInternalMapping) {
    const std::map<std::size_t, std::string> expected_evaluators = {
        {6745, "d21-unknown-stop-sending-graceful-handling"},
        {6746, "d21-unknown-stop-sending-preserves-session"}};
    for (const auto& [line, evaluator] : expected_evaluators) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        const auto& row = *rows.front();
        SCOPED_TRACE(row.id);
        EXPECT_NE(std::find(row.scenarios.begin(), row.scenarios.end(), "d21-grease-stop-sending"),
                  row.scenarios.end());
        EXPECT_NE(std::find(row.evaluators.begin(), row.evaluators.end(), evaluator),
                  row.evaluators.end());
        EXPECT_NE(row.rationale.find("STOP_SENDING"), std::string::npos);
        EXPECT_NE(row.rationale.find("subgroup"), std::string::npos);
    }
    const auto equivalence = at(6756);
    ASSERT_EQ(equivalence.size(), 4u);
    EXPECT_EQ(equivalence[3]->testability, Testability::NotTestable);
    EXPECT_TRUE(equivalence[3]->scenarios.empty());
    EXPECT_TRUE(equivalence[3]->evaluators.empty());
}

TEST_F(Draft21CatalogPart07Test, PublisherMonitoringDoesNotBecomeRelayRateLimitingOrErrorPolicy) {
    for (const auto line : {6859u, 6865u, 6866u, 6957u, 7079u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
        EXPECT_NE(rows.front()->actor.find("relay"), std::string::npos);
    }
    const auto authentication = at(6879, 2);
    ASSERT_EQ(authentication.size(), 1u);
    EXPECT_EQ(authentication.front()->applicability, Applicability::NotApplicable);
    const auto impersonation = at(6951);
    ASSERT_EQ(impersonation.size(), 2u);
    for (const auto* row : impersonation) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    const auto monitoring = at(6867);
    ASSERT_EQ(monitoring.size(), 2u);
    EXPECT_NE(monitoring[0]->summary.find("Monitor"), std::string::npos);
    EXPECT_NE(monitoring[1]->summary.find("Enforce"), std::string::npos);
    for (const auto* row : monitoring) {
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
    }
}

TEST_F(Draft21CatalogPart07Test, TransportAndResourceAdviceRetainsUnobservablePreconditions) {
    for (const auto line : {6879u, 6913u, 7024u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto stream_limit = at(7027);
    ASSERT_EQ(stream_limit.size(), 1u);
    EXPECT_EQ(stream_limit.front()->strength, Strength::May);
    EXPECT_EQ(stream_limit.front()->testability, Testability::Testable);
    const auto cancel = at(7033);
    ASSERT_EQ(cancel.size(), 2u);
    EXPECT_EQ(cancel[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(cancel[0]->testability, Testability::NotTestable);
    EXPECT_NE(cancel[0]->summary.find("after reaching a resource limit"), std::string::npos);
    EXPECT_NE(cancel[0]->rationale.find("preferably"), std::string::npos);
    EXPECT_EQ(cancel[1]->applicability, Applicability::NotApplicable);
}

TEST_F(Draft21CatalogPart07Test, PrivacyChoicesAndUserConfigurationHaveSeparateCapabilityRows) {
    ASSERT_EQ(at(7119).size(), 1u);
    const auto identifying_attributes = at(7123);
    ASSERT_EQ(identifying_attributes.size(), 3u);
    EXPECT_NE(identifying_attributes[0]->summary.find("detailed system information"), std::string::npos);
    EXPECT_NE(identifying_attributes[1]->summary.find("build numbers"), std::string::npos);
    EXPECT_NE(identifying_attributes[2]->summary.find("uniquely identify"), std::string::npos);
    for (const auto line : {7119u, 7123u}) {
        const auto rows = at(line);
        for (const auto* row : rows) {
            EXPECT_EQ(row->applicability, Applicability::Applicable);
            EXPECT_EQ(row->testability, Testability::NotTestable);
        }
    }
    const auto privacy = at(7127);
    ASSERT_EQ(privacy.size(), 2u);
    EXPECT_NE(privacy[0]->summary.find("Omit"), std::string::npos);
    EXPECT_NE(privacy[1]->summary.find("generic"), std::string::npos);
    const auto configuration = at(7130);
    ASSERT_EQ(configuration.size(), 2u);
    EXPECT_NE(configuration[0]->summary.find("configure"), std::string::npos);
    EXPECT_NE(configuration[1]->summary.find("disable"), std::string::npos);
    for (const auto line : {7127u, 7130u}) {
        for (const auto* row : at(line)) {
            EXPECT_EQ(row->strength, Strength::May);
            EXPECT_EQ(row->applicability, Applicability::Applicable);
            EXPECT_EQ(row->testability, Testability::Testable);
        }
    }
}

TEST_F(Draft21CatalogPart07Test, UntrustedStringSanitationIsConditionalLocalBehavior) {
    const auto rows = at(7142);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_NE(rows[0]->summary.find("Reason Phrase"), std::string::npos);
    EXPECT_NE(rows[1]->summary.find("MOQT_IMPLEMENTATION"), std::string::npos);
    for (const auto* row : rows) {
        EXPECT_EQ(row->strength, Strength::Should);
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
        EXPECT_NE(row->summary.find("logs or renders"), std::string::npos);
        EXPECT_NE(row->rationale.find("example"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart07Test, RegistryTextKeepsEndpointParsingSeparateFromRegistrantDuties) {
    const auto properties = at(7478);
    ASSERT_EQ(properties.size(), 2u);
    EXPECT_NE(properties[0]->summary.find("odd"), std::string::npos);
    EXPECT_NE(properties[0]->summary.find("length"), std::string::npos);
    EXPECT_NE(properties[1]->summary.find("even"), std::string::npos);
    EXPECT_NE(properties[1]->summary.find("variable-length integer"), std::string::npos);
    for (const auto* row : properties) {
        EXPECT_NE(row->actor.find("endpoint"), std::string::npos);
        EXPECT_EQ(row->applicability, Applicability::NotApplicable);
        EXPECT_NE(row->rationale.find("9.3"), std::string::npos);
        EXPECT_NE(row->rationale.find("3.6"), std::string::npos);
    }
    for (const auto line : {7290u, 7497u, 7498u, 7536u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
        EXPECT_NE(rows.front()->actor.find("registrant"), std::string::npos);
    }
    EXPECT_NE(at(7290).front()->summary.find("provisional"), std::string::npos);
    EXPECT_NE(at(7497).front()->summary.find("0x4000-0x7FFF"), std::string::npos);
    EXPECT_NE(at(7498).front()->summary.find("Object scope"), std::string::npos);
    EXPECT_EQ(at(7498).front()->strength, Strength::MustNot);
    EXPECT_NE(at(7536).front()->summary.find("payload"), std::string::npos);
}

TEST_F(Draft21CatalogPart07Test, ChangelogKeywordsAreInformativeRatherThanNewRelayDuties) {
    for (const auto line : {8130u, 8715u, 8772u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u) << line;
        EXPECT_EQ(rows.front()->applicability, Applicability::Informative);
        EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
        EXPECT_TRUE(rows.front()->source.section.starts_with("A."));
        EXPECT_NE(rows.front()->rationale.find("historical"), std::string::npos);
    }
    for (const auto& row : catalog_.requirements) {
        if (row.applicability == Applicability::Informative) {
            EXPECT_GE(row.source.first_line, 8035u);
        }
    }
}

}  // namespace
}  // namespace moq::interop::requirements
