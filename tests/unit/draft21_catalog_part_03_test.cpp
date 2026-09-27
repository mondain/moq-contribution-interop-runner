#include "../support/catalog_partition.h"

namespace moq::interop::requirements {
namespace {

class Draft21CatalogPart03Test : public test::CatalogPartitionTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(load_partition(21, "draft21-lines-2431-3338.json"));
    }
};

TEST_F(Draft21CatalogPart03Test, CoversEveryOwnedAnchorWithContiguousClausesAndDraft21Ids) {
    expect_partition_coverage(2431, 3338, 89);
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

TEST_F(Draft21CatalogPart03Test, IdsAreUniqueAcrossBothDraftCatalogsAndEveryExistingPartition) {
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

TEST_F(Draft21CatalogPart03Test, RelayChapterOnlyAppliesPublisherAnnouncementAndSwitchover) {
    for (const auto& row : catalog_.requirements) {
        if (row.source.first_line >= 2766) continue;
        const bool publisher = row.source.first_line == 2639 || row.source.first_line == 2724;
        EXPECT_EQ(row.applicability, publisher ? Applicability::Applicable
                                               : Applicability::NotApplicable) << row.id;
    }
    const auto announce = at(2639);
    ASSERT_EQ(announce.size(), 1u);
    EXPECT_EQ(announce.front()->evaluators,
              std::vector<std::string>{"d21-explicit-namespace-publication-for-routing"});
    const auto migrate = at(2724);
    ASSERT_EQ(migrate.size(), 1u);
    EXPECT_EQ(migrate.front()->strength, Strength::May);
    EXPECT_NE(migrate.front()->summary.find("continue"), std::string::npos);
    const auto duplicate = at(2470);
    ASSERT_EQ(duplicate.size(), 1u);
    EXPECT_EQ(duplicate.front()->actor, "endpoint receiving Objects");
}

TEST_F(Draft21CatalogPart03Test, IndependentRelayActionsRemainSeparateClauses) {
    const std::map<std::pair<std::size_t, unsigned>, std::size_t> counts = {
        {{2449, 2}, 2}, {{2481, 1}, 2}, {{2533, 1}, 2}, {{2563, 1}, 2},
        {{2625, 1}, 2}, {{2627, 1}, 2}, {{2669, 1}, 2}, {{2717, 1}, 2},
        {{2761, 2}, 3}, {{2984, 1}, 3}};
    for (const auto& [anchor, count] : counts) {
        const auto rows = at(anchor.first, anchor.second);
        ASSERT_EQ(rows.size(), count) << anchor.first << ':' << anchor.second;
        for (const auto* row : rows) EXPECT_EQ(row->applicability, Applicability::NotApplicable);
    }
    const auto publish_ok = at(2712);
    ASSERT_EQ(publish_ok.size(), 1u);
    EXPECT_NE(publish_ok.front()->summary.find("PUBLISH_OK"), std::string::npos);
    EXPECT_NE(publish_ok.front()->rationale.find("reserved"), std::string::npos);
}

TEST_F(Draft21CatalogPart03Test, KeyValueOverflowAndMalformedSerializationUseDifferentErrors) {
    const auto outgoing = at(2900);
    ASSERT_EQ(outgoing.size(), 1u);
    EXPECT_EQ(outgoing.front()->strength, Strength::MustNot);
    EXPECT_NE(outgoing.front()->summary.find("2^64 - 1"), std::string::npos);
    const std::map<std::size_t, std::string> evaluators = {
        {2901, "d21-key-value-type-overflow-protocol-violation"},
        {2933, "d21-key-value-length-overflow-protocol-violation"},
        {2940, "d21-known-key-value-formatting-error"}};
    for (const auto& [line, evaluator] : evaluators) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{evaluator});
        EXPECT_NE(rows.front()->rationale.find("SETUP"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart03Test, PropertyPreservationIsRelayOnlyButPrivatePropertyUseIsPublisherCapability) {
    for (const auto& row : catalog_.requirements) {
        if (row.source.section != "8.4") continue;
        const bool application = row.source.first_line == 3008;
        EXPECT_EQ(row.applicability, application ? Applicability::Applicable
                                                 : Applicability::NotApplicable) << row.id;
    }
    for (const auto anchor : {std::pair{2973u, 1u}, {2973u, 2u}, {2974u, 1u}}) {
        const auto rows = at(anchor.first, anchor.second);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_NE(rows.front()->summary.find("Mandatory Track Property"), std::string::npos);
    }
    const auto private_use = at(3008);
    ASSERT_EQ(private_use.size(), 1u);
    EXPECT_EQ(private_use.front()->strength, Strength::May);
    EXPECT_EQ(private_use.front()->testability, Testability::Testable);
    EXPECT_NE(private_use.front()->summary.find("0x78-0x7F"), std::string::npos);
    EXPECT_NE(private_use.front()->summary.find("0x3800-0x3FFF"), std::string::npos);
}

TEST_F(Draft21CatalogPart03Test, ReasonAndRangeErrorsKeepSessionAndRequestScopes) {
    const auto reason = at(3037);
    ASSERT_EQ(reason.size(), 1u);
    EXPECT_NE(reason.front()->summary.find("1024"), std::string::npos);
    EXPECT_EQ(reason.front()->evaluators,
              std::vector<std::string>{"d21-oversized-reason-phrase-protocol-violation"});
    const auto range = at(3067);
    ASSERT_EQ(range.size(), 1u);
    EXPECT_EQ(range.front()->evaluators,
              std::vector<std::string>{"d21-range-delta-overflow-invalid-filter"});
    EXPECT_NE(range.front()->summary.find("Start"), std::string::npos);
    EXPECT_NE(range.front()->summary.find("End"), std::string::npos);
    EXPECT_NE(range.front()->summary.find("request"), std::string::npos);
}

TEST_F(Draft21CatalogPart03Test, NamespaceBoundsSeparateEmittedFieldsAndReceiverFailures) {
    const auto emitted = at(3107);
    ASSERT_EQ(emitted.size(), 1u);
    EXPECT_EQ(emitted.front()->evaluators,
              std::vector<std::string>{"d21-emitted-namespace-fields-nonempty"});
    const std::map<std::size_t, std::string> evaluators = {
        {3109, "d21-empty-namespace-field-protocol-violation"},
        {3113, "d21-too-many-namespace-fields-protocol-violation"}};
    for (const auto& [line, evaluator] : evaluators) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{evaluator});
    }
    const auto lengths = at(3121);
    ASSERT_EQ(lengths.size(), 2u);
    EXPECT_EQ(lengths[0]->evaluators,
              std::vector<std::string>{"d21-oversized-namespace-protocol-violation"});
    EXPECT_EQ(lengths[1]->evaluators,
              std::vector<std::string>{"d21-oversized-full-track-name-protocol-violation"});
    for (const auto* row : lengths) {
        EXPECT_NE(row->summary.find("4096"), std::string::npos);
        EXPECT_NE(row->rationale.find("length-prefix"), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart03Test, SerializedNamesUseCanonicalLocalRulesWithoutInventedWireErrors) {
    for (const auto& row : catalog_.requirements) {
        if (row.source.section != "8.8" && row.source.section != "8.8.1") continue;
        EXPECT_EQ(row.applicability, Applicability::Applicable);
        EXPECT_EQ(row.testability, Testability::NotTestable);
    }
    const auto format = at(3131);
    ASSERT_EQ(format.size(), 2u);
    EXPECT_EQ(format[0]->strength, Strength::Should);
    EXPECT_NE(format[0]->summary.find("--"), std::string::npos);
    EXPECT_NE(format[1]->summary.find("exactly two lowercase"), std::string::npos);
    const auto escape = at(3161);
    ASSERT_EQ(escape.size(), 1u);
    EXPECT_NE(escape.front()->summary.find("exactly two"), std::string::npos);
    const auto redundant = at(3169);
    ASSERT_EQ(redundant.size(), 1u);
    EXPECT_EQ(redundant.front()->strength, Strength::MustNot);
    const auto invalid = at(3180);
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_NE(invalid.front()->rationale.find("application-defined"), std::string::npos);
}

TEST_F(Draft21CatalogPart03Test, TokenLifetimeRetirementAndRegistrationFailuresAreObservableThroughAliasUse) {
    const std::map<std::size_t, std::string> evaluators = {
        {3223, "d21-delete-retires-token-alias"},
        {3228, "d21-register-preserves-token-association"},
        {3274, "d21-register-on-nonsession-message-error"},
        {3288, "d21-expired-token-alias-retained-until-delete"}};
    for (const auto& [line, evaluator] : evaluators) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{evaluator});
    }
    const auto register_error = at(3274);
    ASSERT_EQ(register_error.size(), 1u);
    EXPECT_NE(register_error.front()->summary.find("Session error"), std::string::npos);
    EXPECT_NE(register_error.front()->summary.find("Unauthorized"), std::string::npos);
    EXPECT_NE(register_error.front()->rationale.find("SETUP"), std::string::npos);
}

TEST_F(Draft21CatalogPart03Test, TokenDecodeDuplicateUnknownAndInvalidFailuresHaveExactErrorScopes) {
    const std::map<std::size_t, std::pair<std::string, std::string>> cases = {
        {3262, {"close the Session", "KEY_VALUE_FORMATTING_ERROR"}},
        {3264, {"close the Session", "DUPLICATE_AUTH_TOKEN_ALIAS"}},
        {3266, {"reject the message", "UNKNOWN_AUTH_TOKEN_ALIAS"}},
        {3270, {"reject the message", "MALFORMED_AUTH_TOKEN"}}};
    for (const auto& [line, expected] : cases) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->testability, Testability::Testable);
        EXPECT_NE(rows.front()->summary.find(expected.first), std::string::npos);
        EXPECT_NE(rows.front()->summary.find(expected.second), std::string::npos);
    }
}

TEST_F(Draft21CatalogPart03Test, TokenStorageAndSenderIntentAreNotExternallyObservable) {
    for (const auto line : {3236u, 3278u, 3289u, 3309u, 3310u}) {
        const auto rows = at(line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
        EXPECT_EQ(rows.front()->testability, Testability::NotTestable);
        EXPECT_TRUE(rows.front()->scenarios.empty());
        EXPECT_TRUE(rows.front()->evaluators.empty());
    }
    const auto stored_error = at(3278);
    ASSERT_EQ(stored_error.size(), 1u);
    EXPECT_NE(stored_error.front()->summary.find("any future message"), std::string::npos);
    const auto discard = at(3289);
    ASSERT_EQ(discard.size(), 1u);
    EXPECT_NE(discard.front()->rationale.find("cache size"), std::string::npos);
}

TEST_F(Draft21CatalogPart03Test, CacheOverflowExcludesSetupFallbackAndRepeatedTokensCompareResolvedValues) {
    const auto overflow = at(3319);
    ASSERT_EQ(overflow.size(), 1u);
    EXPECT_NE(overflow.front()->summary.find("AUTH_TOKEN_CACHE_OVERFLOW"), std::string::npos);
    EXPECT_NE(overflow.front()->rationale.find("16 bytes"), std::string::npos);
    EXPECT_NE(overflow.front()->rationale.find("USE_VALUE"), std::string::npos);
    EXPECT_NE(overflow.front()->rationale.find("SETUP"), std::string::npos);
    const auto repeat = at(3321);
    ASSERT_EQ(repeat.size(), 1u);
    EXPECT_EQ(repeat.front()->strength, Strength::May);
    EXPECT_NE(repeat.front()->summary.find("Token Type and Token Value"), std::string::npos);
    EXPECT_NE(repeat.front()->summary.find("resolving"), std::string::npos);
}

TEST_F(Draft21CatalogPart03Test, SenderOrderingPreservesControlStreamWordingAndResponsePreconditions) {
    const auto cross = at(3330);
    ASSERT_EQ(cross.size(), 1u);
    EXPECT_EQ(cross.front()->strength, Strength::MustNot);
    EXPECT_EQ(cross.front()->testability, Testability::NotTestable);
    EXPECT_NE(cross.front()->summary.find("different control stream"), std::string::npos);
    EXPECT_NE(cross.front()->summary.find("response"), std::string::npos);
    EXPECT_NE(cross.front()->rationale.find("request streams"), std::string::npos);
    const auto same = at(3332);
    ASSERT_EQ(same.size(), 1u);
    EXPECT_EQ(same.front()->strength, Strength::May);
    EXPECT_EQ(same.front()->testability, Testability::NotTestable);
    EXPECT_NE(same.front()->summary.find("same control stream"), std::string::npos);
    const auto remove = at(3336);
    ASSERT_EQ(remove.size(), 1u);
    EXPECT_EQ(remove.front()->evaluators,
              std::vector<std::string>{"d21-no-delete-with-unanswered-alias-use"});
    EXPECT_NE(remove.front()->summary.find("any message"), std::string::npos);
}

}  // namespace
}  // namespace moq::interop::requirements
