#include "moq/interop/requirements/catalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop::requirements {
namespace {

using Anchor = std::pair<std::size_t, unsigned>;

class Draft18CatalogPart01Test : public ::testing::Test {
protected:
    void SetUp() override {
        const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
        source_ = load_draft_source(18, root / "docs", root / "requirements/draft-digests.json");
        const auto path = root / "requirements/parts/draft18-lines-0001-1254.json";
        ASSERT_TRUE(std::filesystem::is_regular_file(path)) << "Missing reviewed catalog partition";
        catalog_ = RequirementCatalog::load(source_, path, CatalogLoadMode::AllowIncomplete);
    }

    std::vector<const Requirement*> at(std::size_t line, unsigned occurrence = 1) const {
        std::vector<const Requirement*> rows;
        for (const auto& row : catalog_.requirements) {
            if (row.source.first_line == line && row.source.occurrence == occurrence) {
                rows.push_back(&row);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const auto* left, const auto* right) {
            return left->source.clause < right->source.clause;
        });
        return rows;
    }

    DraftSource source_{};
    RequirementCatalog catalog_{};
};

TEST_F(Draft18CatalogPart01Test, CoversAllOwnedOccurrencesWithoutConflictsOrForeignAnchors) {
    std::map<Anchor, NormativeOccurrence> owned;
    for (const auto& occurrence : scan_normative_occurrences(source_)) {
        if (occurrence.first_line >= 1 && occurrence.first_line <= 1254) {
            owned.emplace(Anchor{occurrence.first_line, occurrence.occurrence_on_line}, occurrence);
        }
    }
    ASSERT_EQ(owned.size(), 62u);
    EXPECT_FALSE(catalog_.complete);
    const auto audit = audit_normative_occurrences(source_, catalog_);
    EXPECT_TRUE(audit.multiply_classified.empty());
    ASSERT_EQ(audit.errors.size(), 1u);
    EXPECT_EQ(audit.errors.front(), "Incomplete catalog cannot pass the full-corpus audit");
    for (const auto& missing : audit.missing) {
        EXPECT_GT(missing.first_line, 1254u) << "Missing anchor at " << missing.first_line;
    }
    std::map<Anchor, std::set<unsigned>> clauses;
    const std::regex id_pattern(R"(D18-[0-9]+(-[0-9]+)*-(MUST|MUST-NOT|SHOULD|SHOULD-NOT|MAY)-[0-9]{3})");
    for (const auto& row : catalog_.requirements) {
        SCOPED_TRACE(row.id);
        const Anchor anchor{row.source.first_line, row.source.occurrence};
        ASSERT_TRUE(owned.contains(anchor));
        EXPECT_LE(row.source.last_line, 1254u);
        EXPECT_EQ(row.strength, owned.at(anchor).normalized_strength);
        EXPECT_GE(row.source.last_line, owned.at(anchor).last_line);
        EXPECT_TRUE(std::regex_match(row.id, id_pattern));
        EXPECT_TRUE(clauses[anchor].insert(row.source.clause).second);
        if (row.applicability == Applicability::Applicable &&
            row.testability == Testability::Testable) {
            EXPECT_FALSE(row.scenarios.empty());
            EXPECT_FALSE(row.evaluators.empty());
        } else {
            EXPECT_TRUE(row.scenarios.empty());
            EXPECT_TRUE(row.evaluators.empty());
        }
    }
    ASSERT_EQ(clauses.size(), owned.size());
    for (const auto& [anchor, ordinals] : clauses) {
        unsigned expected = 1;
        for (const auto ordinal : ordinals) {
            EXPECT_EQ(ordinal, expected++) << "Anchor " << anchor.first << ':' << anchor.second;
        }
    }
}

TEST_F(Draft18CatalogPart01Test, QuotedBcp14VocabularyHasNoRuntimeObligations) {
    unsigned count = 0;
    for (const auto& occurrence : scan_normative_occurrences(source_)) {
        if (occurrence.first_line < 412 || occurrence.first_line > 414) continue;
        ++count;
        ASSERT_TRUE(occurrence.quoted_bcp14_vocabulary);
        const auto rows = at(occurrence.first_line, occurrence.occurrence_on_line);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows.front()->applicability, Applicability::Informative);
        EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
        EXPECT_TRUE(rows.front()->scenarios.empty());
        EXPECT_TRUE(rows.front()->evaluators.empty());
        EXPECT_NE(rows.front()->rationale.find("BCP 14"), std::string::npos);
    }
    EXPECT_EQ(count, 11u);
}

TEST_F(Draft18CatalogPart01Test, WrappedProhibitionsDistinguishNameParsingFromObjectImmutability) {
    const auto names = at(737);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names.front()->strength, Strength::MustNot);
    EXPECT_GE(names.front()->source.last_line, 738u);
    EXPECT_EQ(names.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(names.front()->testability, Testability::NotTestable);
    const auto objects = at(797);
    ASSERT_EQ(objects.size(), 1u);
    EXPECT_EQ(objects.front()->strength, Strength::MustNot);
    EXPECT_GE(objects.front()->source.last_line, 798u);
    EXPECT_EQ(objects.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(objects.front()->testability, Testability::Testable);
    EXPECT_EQ(objects.front()->evaluators, std::vector<std::string>{"object-payload-immutable"});
}

TEST_F(Draft18CatalogPart01Test, EndpointReceiveValidationAppliesToPublishingEndpoints) {
    const auto rows = at(632);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->actor, "endpoint");
    EXPECT_EQ(rows.front()->strength, Strength::Must);
    EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(rows.front()->testability, Testability::Testable);
    EXPECT_EQ(rows.front()->scenarios, std::vector<std::string>{"receive-key-value-type-overflow"});
    EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"session-closed-protocol-violation"});
}

TEST_F(Draft18CatalogPart01Test, FirstObjectRuleSeparatesOriginalPublisherFromRelay) {
    const auto publisher = at(901);
    ASSERT_EQ(publisher.size(), 1u);
    EXPECT_EQ(publisher.front()->actor, "original-publisher");
    EXPECT_EQ(publisher.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(publisher.front()->testability, Testability::Testable);
    EXPECT_EQ(publisher.front()->evaluators, std::vector<std::string>{"new-subgroup-first-object-bit-set"});
    const auto relay = at(905);
    ASSERT_EQ(relay.size(), 1u);
    EXPECT_EQ(relay.front()->actor, "relay");
    EXPECT_EQ(relay.front()->applicability, Applicability::NotApplicable);
    EXPECT_EQ(relay.front()->testability, Testability::NotApplicable);
}

TEST_F(Draft18CatalogPart01Test, SubscriberRangeAdviceDoesNotConstrainPublisherBehavior) {
    const auto rows = at(933);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->actor, "subscriber");
    EXPECT_EQ(rows.front()->strength, Strength::ShouldNot);
    EXPECT_EQ(rows.front()->applicability, Applicability::NotApplicable);
    EXPECT_EQ(rows.front()->testability, Testability::NotApplicable);
}

TEST_F(Draft18CatalogPart01Test, OptionalSubgroupIdReuseRemainsAnObservableCapability) {
    const auto rows = at(875);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front()->actor, "original-publisher");
    EXPECT_EQ(rows.front()->strength, Strength::May);
    EXPECT_EQ(rows.front()->applicability, Applicability::Applicable);
    EXPECT_EQ(rows.front()->testability, Testability::Testable);
    EXPECT_EQ(rows.front()->evaluators, std::vector<std::string>{"subgroup-id-reused-across-groups"});
}

TEST_F(Draft18CatalogPart01Test, UnsupportedMandatoryPropertyAdviceHasThreeIndependentPublisherActions) {
    const auto rows = at(1245);
    ASSERT_EQ(rows.size(), 3u);
    const std::vector<std::string> evaluators = {
        "subscribe-rejected-unsupported-extension", "fetch-rejected-unsupported-extension",
        "unsupported-track-not-published"};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        EXPECT_EQ(rows[index]->source.clause, index + 1);
        EXPECT_EQ(rows[index]->actor, "publisher");
        EXPECT_EQ(rows[index]->strength, Strength::Should);
        EXPECT_EQ(rows[index]->applicability, Applicability::Applicable);
        EXPECT_EQ(rows[index]->testability, Testability::Testable);
        EXPECT_EQ(rows[index]->evaluators, std::vector<std::string>{evaluators[index]});
    }
}

}  // namespace
}  // namespace moq::interop::requirements
