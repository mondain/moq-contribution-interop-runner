#pragma once

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

namespace moq::interop::requirements::test {

class CatalogPartitionTest : public ::testing::Test {
protected:
    void load_partition(unsigned draft, const std::filesystem::path& filename) {
        const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
        source_ = load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        const auto path = root / "requirements/parts" / filename;
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

    void expect_partition_coverage(std::size_t first, std::size_t last,
                                   std::size_t expected_occurrences) const {
        using Anchor = std::pair<std::size_t, unsigned>;
        std::map<Anchor, NormativeOccurrence> owned;
        for (const auto& occurrence : scan_normative_occurrences(source_)) {
            if (occurrence.first_line >= first && occurrence.first_line <= last) {
                owned.emplace(Anchor{occurrence.first_line, occurrence.occurrence_on_line}, occurrence);
            }
        }
        ASSERT_EQ(owned.size(), expected_occurrences);
        EXPECT_FALSE(catalog_.complete);
        const auto audit = audit_normative_occurrences(source_, catalog_);
        EXPECT_TRUE(audit.multiply_classified.empty());
        ASSERT_EQ(audit.errors.size(), 1u);
        EXPECT_EQ(audit.errors.front(), "Incomplete catalog cannot pass the full-corpus audit");
        for (const auto& missing : audit.missing) {
            EXPECT_TRUE(missing.first_line < first || missing.first_line > last)
                << "Missing anchor at " << missing.first_line << ':' << missing.occurrence_on_line;
        }
        std::map<Anchor, std::set<unsigned>> clauses;
        const std::regex id_pattern("D" + std::to_string(source_.number) +
            R"(-([0-9]+|[A-Z])(-[0-9]+)*-(MUST|MUST-NOT|SHOULD|SHOULD-NOT|MAY)-[0-9]{3})");
        for (const auto& row : catalog_.requirements) {
            SCOPED_TRACE(row.id);
            const Anchor anchor{row.source.first_line, row.source.occurrence};
            ASSERT_TRUE(owned.contains(anchor));
            EXPECT_LE(row.source.last_line, last);
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

    DraftSource source_{};
    RequirementCatalog catalog_{};
};

}  // namespace moq::interop::requirements::test
