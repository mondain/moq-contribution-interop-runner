#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft18CatalogPart06Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(18, "draft18-lines-6109-7840.json"));
    }
};

TEST_F(Draft18CatalogPart06Test, CoversAllOwnedOccurrencesWithoutConflictsOrForeignAnchors) {
    expect_partition_coverage(6109, 7840, 27);
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

TEST_F(Draft18CatalogPart06Test, RequirementIdsAreUniqueAcrossAllSixPartitions) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    std::set<std::string> ids;
    for (const auto* filename : {"draft18-lines-0001-1254.json", "draft18-lines-1255-2564.json",
                                "draft18-lines-2565-3508.json", "draft18-lines-3509-4935.json",
                                "draft18-lines-4936-6108.json", "draft18-lines-6109-7840.json"}) {
        const auto part = RequirementCatalog::load(
            source_, root / "requirements/parts" / filename, CatalogLoadMode::AllowIncomplete);
        for (const auto& row : part.requirements) {
            EXPECT_TRUE(ids.insert(row.id).second) << row.id;
        }
    }
}

TEST_F(Draft18CatalogPart06Test, AggregatedSubscriberAuthorizationAndOtherRelayDutiesStayRelayOnly) {
    for (const auto [line, occurrence] :
         {std::pair{6131u, 1u}, {6137u, 1u}, {6138u, 1u}, {6151u, 2u}, {6304u, 1u}}) {
        const auto rows = at(line, occurrence);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "relay");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
    }
    const auto authorization = at(6131);
    ASSERT_EQ(authorization.size(), 1u);
    EXPECT_EQ(authorization.front()->strength, Strength::Must);
    EXPECT_NE(authorization.front()->summary.find("independently"), std::string::npos);
    EXPECT_NE(authorization.front()->rationale.find("aggregate"), std::string::npos);
}

TEST_F(Draft18CatalogPart06Test, PublisherMonitoringAndLimitEnforcementHaveSeparatePolicyBoundaries) {
    const auto rows = at(6139);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_NE(rows[0]->summary.find("Monitor"), std::string::npos);
    EXPECT_NE(rows[1]->summary.find("Enforce"), std::string::npos);
    for (const auto* row : rows) {
        EXPECT_EQ(row->actor, "publisher");
        EXPECT_EQ(row->strength, Strength::Should);
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
    }
    const auto transport = at(6151, 1);
    ASSERT_EQ(transport.size(), 1u);
    EXPECT_EQ(transport.front()->actor, "implementation");
    EXPECT_EQ(transport.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(transport.front()->testability, Testability::NotTestable);
}

TEST_F(Draft18CatalogPart06Test, ResourceLimitsKeepPrivateThresholdsAndPublisherSubscriberRoles) {
    for (const auto line : {6250u, 6253u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "endpoint");
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
    }
    const auto cancellation = at(6259);
    ASSERT_EQ(cancellation.size(), 2u);
    EXPECT_EQ(cancellation[0]->actor, "publisher");
    EXPECT_EQ(cancellation[0]->strength, Strength::Must);
    EXPECT_EQ(cancellation[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(cancellation[0]->testability, Testability::NotTestable);
    EXPECT_NE(cancellation[0]->rationale.find("lowest priority"), std::string::npos);
    EXPECT_EQ(cancellation[1]->actor, "subscriber");
    EXPECT_EQ(cancellation[1]->applicability, Applicability::NotApplicable);
}

TEST_F(Draft18CatalogPart06Test, PrivacyGuidanceSeparatesObservableOmissionFromUnspecifiedPolicyAndControls) {
    const auto minimum = at(6349);
    ASSERT_EQ(minimum.size(), 1u);
    EXPECT_EQ(minimum.front()->testability, Testability::NotTestable);
    const auto details = at(6353);
    ASSERT_EQ(details.size(), 3u);
    for (const auto* row : details) {
        EXPECT_EQ(row->strength, Strength::ShouldNot);
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
    }
    const auto deployment = at(6357);
    ASSERT_EQ(deployment.size(), 2u);
    EXPECT_EQ(deployment[0]->strength, Strength::May);
    EXPECT_EQ(deployment[0]->testability, Testability::Testable);
    EXPECT_EQ(deployment[0]->evaluators,
              std::vector<std::string>{"setup-omits-moqt-implementation-option"});
    EXPECT_EQ(deployment[1]->testability, Testability::NotTestable);
    const auto controls = at(6360);
    ASSERT_EQ(controls.size(), 2u);
    for (const auto* row : controls) {
        EXPECT_EQ(row->strength, Strength::May);
        EXPECT_EQ(row->applicability, Applicability::Applicable);
        EXPECT_EQ(row->testability, Testability::NotTestable);
    }
}

TEST_F(Draft18CatalogPart06Test, GracefulUnknownValuesRetainRegistryContextsAndEndpointRoles) {
    const auto rows = at(6402);
    ASSERT_EQ(rows.size(), 7u);
    const std::vector<Testability> testability = {
        Testability::Testable, Testability::Testable, Testability::NotTestable,
        Testability::Testable, Testability::NotApplicable, Testability::Testable,
        Testability::Testable};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        EXPECT_EQ(rows[index]->strength, Strength::Must);
        EXPECT_EQ(rows[index]->testability, testability[index]);
    }
    EXPECT_EQ(rows[4]->actor, "subscriber");
    EXPECT_EQ(rows[5]->actor, "endpoint");
    EXPECT_EQ(rows[5]->applicability, Applicability::Applicable);
    EXPECT_EQ(rows[5]->evaluators,
              std::vector<std::string>{"session-survives-unknown-request-stream-reset-code"});
    EXPECT_EQ(rows[6]->evaluators,
              std::vector<std::string>{"unknown-auth-token-type-does-not-close-session"});
    const auto no_close = at(6403);
    ASSERT_EQ(no_close.size(), 1u);
    EXPECT_EQ(no_close.front()->strength, Strength::MustNot);
    EXPECT_EQ(no_close.front()->testability, Testability::Testable);
    EXPECT_EQ(no_close.front()->scenarios.size(), 5u);
    EXPECT_NE(no_close.front()->rationale.find("stream types"), std::string::npos);
}

TEST_F(Draft18CatalogPart06Test, UnknownSetupOptionsRemainProtocolRequirementsInsideTheRegistrySection) {
    for (const auto line : {6407u, 6541u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "endpoint");
        EXPECT_EQ(rows.front()->strength, Strength::Must);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_EQ(rows.front()->evaluators,
                  std::vector<std::string>{"setup-continues-with-unknown-options-ignored"});
    }
}

TEST_F(Draft18CatalogPart06Test, UnknownPropertiesPreserveMandatoryExceptionsAndLengthEncodingAmbiguity) {
    const auto crossref = at(6410);
    ASSERT_EQ(crossref.size(), 3u);
    EXPECT_EQ(crossref[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(crossref[0]->testability, Testability::Testable);
    EXPECT_EQ(crossref[1]->actor, "relay");
    EXPECT_EQ(crossref[1]->applicability, Applicability::NotApplicable);
    EXPECT_EQ(crossref[2]->actor, "subscriber");
    EXPECT_EQ(crossref[2]->applicability, Applicability::NotApplicable);
    const auto registry = at(6696);
    ASSERT_EQ(registry.size(), 1u);
    EXPECT_EQ(registry.front()->strength, Strength::Must);
    EXPECT_EQ(registry.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(registry.front()->testability, Testability::Testable);
    EXPECT_EQ(registry.front()->evaluators,
              std::vector<std::string>{"track-status-unknown-optional-properties-skipped"});
    EXPECT_NE(registry.front()->rationale.find("Mandatory"), std::string::npos);
    EXPECT_NE(registry.front()->rationale.find("even"), std::string::npos);
    EXPECT_NE(registry.front()->rationale.find("length"), std::string::npos);
}

TEST_F(Draft18CatalogPart06Test, UnknownPropertyStimuliKeepUnsignedTypeDeltasNondecreasing) {
    for (const auto line : {6402u, 6410u, 6696u}) {
        const auto rows = at(line);
        const auto clause = line == 6402u ? 2u : 1u;
        ASSERT_GE(rows.size(), clause);
        const auto& rationale = rows[clause - 1]->rationale;
        EXPECT_NE(rationale.find("0x00"), std::string::npos);
        EXPECT_NE(rationale.find("0x01"), std::string::npos);
        EXPECT_NE(rationale.find("0x22"), std::string::npos);
        EXPECT_NE(rationale.find("ascending"), std::string::npos);
        EXPECT_EQ(rows[clause - 1]->scenarios,
                  (std::vector<std::string>{
                      "publisher-recovery-track-status-unknown-optional-properties",
                      "publisher-recovery-track-status-unknown-before-invalid-property"}));
        EXPECT_NE(rationale.find("PROTOCOL_VIOLATION"), std::string::npos);
    }
}

TEST_F(Draft18CatalogPart06Test, UnknownErrorMappingIsDistinctFromObservableRequestErrorSessionSurvival) {
    const auto mapping = at(6413);
    ASSERT_EQ(mapping.size(), 4u);
    EXPECT_EQ(mapping[0]->testability, Testability::NotTestable);
    EXPECT_NE(mapping[0]->summary.find("Session Termination"), std::string::npos);
    EXPECT_EQ(mapping[1]->testability, Testability::NotTestable);
    EXPECT_NE(mapping[1]->summary.find("REQUEST_ERROR"), std::string::npos);
    for (const auto index : {2u, 3u}) {
        EXPECT_EQ(mapping[index]->actor, "subscriber");
        EXPECT_EQ(mapping[index]->applicability, Applicability::NotApplicable);
    }
    const auto no_close = at(6415);
    ASSERT_EQ(no_close.size(), 2u);
    EXPECT_EQ(no_close[0]->applicability, Applicability::Applicable);
    EXPECT_EQ(no_close[0]->evaluators,
              std::vector<std::string>{"session-survives-unknown-request-error"});
    EXPECT_EQ(no_close[1]->actor, "subscriber");
    EXPECT_EQ(no_close[1]->applicability, Applicability::NotApplicable);
}

TEST_F(Draft18CatalogPart06Test, RegistrationPolicyIsARegistrantDutyRatherThanPublisherWireBehavior) {
    for (const auto line : {6562u, 6714u, 6715u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->actor, "extension-registrant");
        EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
    }
    const auto advice = at(6562);
    ASSERT_EQ(advice.size(), 1u);
    EXPECT_EQ(advice.front()->strength, Strength::Should);
    EXPECT_NE(advice.front()->summary.find("provisional"), std::string::npos);
}

TEST_F(Draft18CatalogPart06Test, ChangelogKeywordsAreInformativeWithTheirTrueAppendixSections) {
    for (const auto [line, section] : {std::pair{7658u, "A.5"}, {7714u, "A.7"}}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->source.section, section);
        EXPECT_EQ(rows.front()->actor, "specification-reader");
        EXPECT_EQ(rows.front()->strength, Strength::Must);
        EXPECT_EQ(rows.front()->applicability, Applicability::Informative);
        EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
        EXPECT_NE(rows.front()->rationale.find("historical"), std::string::npos);
    }
}

TEST_F(Draft18CatalogPart06Test, PartitionCoverageAcceptsAppendixSectionIdentifiers) {
    std::erase_if(catalog_.requirements,
                  [](const auto& row) { return row.source.first_line < 7195; });
    expect_partition_coverage(7195, 7840, 2);
}

}  // namespace
}  // namespace moq::interop::requirements
