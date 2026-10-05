#include "moq/interop/requirements/carry_forward.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace moq::interop::requirements {
namespace {

DraftSource source_from_text(unsigned draft, std::string text) {
    DraftSource source{draft, {}, "synthetic", std::move(text), {0}};
    for (std::size_t i = 0; i + 1 < source.text.size(); ++i) {
        if (source.text[i] == '\n') {
            source.line_offsets.push_back(i + 1);
        }
    }
    return source;
}

TEST(CarryForwardNormalizeTest, CollapsesWhitespaceAndTrims) {
    EXPECT_EQ(normalize_text("  a\n   b\t c \n"), "a b c");
    EXPECT_EQ(normalize_text(""), "");
}

TEST(CarryForwardContextTest, TracksSectionAndIsolatesTheSentence) {
    const auto source = source_from_text(22,
        "1.  Introduction\n"
        "\n"
        "   An endpoint MUST send HELLO.  A peer MAY ignore it.\n"
        "\n"
        "2.  Other Things\n"
        "\n"
        "   A relay SHOULD NOT forward.\n");
    const auto contexts = extract_contexts(source);
    ASSERT_EQ(contexts.size(), 3u);
    EXPECT_EQ(contexts[0].section, "1");
    EXPECT_EQ(contexts[0].section_title, "Introduction");
    EXPECT_EQ(contexts[0].sentence, "An endpoint MUST send HELLO.");
    EXPECT_EQ(contexts[0].strength, Strength::Must);
    EXPECT_EQ(contexts[1].sentence, "A peer MAY ignore it.");
    EXPECT_EQ(contexts[2].section, "2");
    EXPECT_EQ(contexts[2].section_title, "Other Things");
    EXPECT_EQ(contexts[2].sentence, "A relay SHOULD NOT forward.");
    EXPECT_EQ(contexts[2].first_line, 7u);
}

TEST(CarryForwardContextTest, JoinsASentenceAcrossAPageBreak) {
    const auto source = source_from_text(22,
        "1.  Introduction\n"
        "\n"
        "   An endpoint MUST send\n"
        "\n"
        "\n"
        "\n"
        "Nandakumar, et al.        Expires 4 April 2027                [Page 1]\n"
        "\f\n"
        "Internet-Draft                moq-transport                 October 2026\n"
        "\n"
        "   HELLO first.\n");
    const auto contexts = extract_contexts(source);
    ASSERT_EQ(contexts.size(), 1u);
    EXPECT_EQ(contexts[0].sentence, "An endpoint MUST send HELLO first.");
    EXPECT_EQ(contexts[0].first_line, 3u);
    EXPECT_EQ(contexts[0].sentence_last_line, 11u);
}

TEST(CarryForwardContextTest, SentenceEndingAtALineEndKeepsItsOwnLastLine) {
    const auto source = source_from_text(22,
        "1.  Introduction\n"
        "\n"
        "   An endpoint MUST send HELLO.\n"
        "   A peer MAY ignore it\n"
        "   completely.\n");
    const auto contexts = extract_contexts(source);
    ASSERT_EQ(contexts.size(), 2u);
    EXPECT_EQ(contexts[0].sentence, "An endpoint MUST send HELLO.");
    EXPECT_EQ(contexts[0].sentence_last_line, 3u);
    EXPECT_EQ(contexts[1].sentence, "A peer MAY ignore it completely.");
    EXPECT_EQ(contexts[1].sentence_last_line, 5u);
}

TEST(CarryForwardContextTest, GivesEachKeywordInOneSentenceItsOwnOrdinal) {
    const auto source = source_from_text(22,
        "1.  Introduction\n"
        "\n"
        "   The words \"MUST\", \"MUST NOT\" and \"MAY\" are defined.\n");
    const auto contexts = extract_contexts(source);
    ASSERT_EQ(contexts.size(), 3u);
    EXPECT_EQ(contexts[0].sentence, contexts[1].sentence);
    EXPECT_EQ(contexts[0].ordinal_in_sentence, 1u);
    EXPECT_EQ(contexts[1].ordinal_in_sentence, 2u);
    EXPECT_EQ(contexts[2].ordinal_in_sentence, 3u);
}

TEST(CarryForwardContextTest, BulletsAreSeparateSentenceUnits) {
    const auto source = source_from_text(22,
        "3.  Rules\n"
        "\n"
        "   A sender follows these:\n"
        "\n"
        "   *  It MUST be first.\n"
        "   *  It MAY be last.\n");
    const auto contexts = extract_contexts(source);
    ASSERT_EQ(contexts.size(), 2u);
    EXPECT_EQ(contexts[0].sentence, "* It MUST be first.");
    EXPECT_EQ(contexts[1].sentence, "* It MAY be last.");
}

TEST(CarryForwardContextTest, SectionsMatchTheReviewedDraft21Catalog) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft21.json");
    const auto contexts = extract_contexts(source);
    std::map<std::pair<std::size_t, unsigned>, const OccurrenceContext*> by_anchor;
    std::size_t unparsed = 0;
    for (const auto& context : contexts) {
        by_anchor[{context.first_line, context.occurrence_on_line}] = &context;
        unparsed += context.sentence.empty() ? 1 : 0;
    }
    std::size_t mismatched = 0;
    for (const auto& row : catalog.requirements) {
        const auto it = by_anchor.find({row.source.first_line, row.source.occurrence});
        ASSERT_NE(it, by_anchor.end()) << row.id;
        if (it->second->section != row.source.section) {
            ++mismatched;
            ADD_FAILURE() << row.id << " catalog=" << row.source.section
                          << " extracted=" << it->second->section;
        }
    }
    EXPECT_EQ(mismatched, 0u);
    // Unparsed sentences become reviewed `new` rows, so they must stay rare.
    EXPECT_LE(unparsed * 100, contexts.size() * 3) << unparsed << " of " << contexts.size();
}

RequirementCatalog catalog_for(const DraftSource& source) {
    RequirementCatalog catalog{source.number, source.sha256, true, {}};
    unsigned counter = 0;
    for (const auto& occurrence : scan_normative_occurrences(source)) {
        const char* token = "MAY";
        switch (occurrence.normalized_strength) {
            case Strength::Must: token = "MUST"; break;
            case Strength::MustNot: token = "MUST-NOT"; break;
            case Strength::Should: token = "SHOULD"; break;
            case Strength::ShouldNot: token = "SHOULD-NOT"; break;
            case Strength::May: break;
        }
        catalog.requirements.push_back(
            {"D21-1-" + std::string(token) + "-" + std::to_string(++counter),
             occurrence.normalized_strength,
             {"1", occurrence.first_line, occurrence.last_line, occurrence.occurrence_on_line, 1},
             "endpoint", "summary", Applicability::Applicable, Testability::NotTestable, {}, {},
             "rationale"});
    }
    return catalog;
}

constexpr const char* kOld =
    "1.  Introduction\n"
    "\n"
    "   An endpoint MUST send HELLO.  A peer MAY ignore it.\n"
    "\n"
    "2.  Other Things\n"
    "\n"
    "   A relay SHOULD NOT forward.\n"
    "\n"
    "   A sender MUST retry.\n";

constexpr const char* kNew =
    "1.  Introduction\n"
    "\n"
    "   An endpoint MUST send HELLO immediately.  A peer MAY ignore it.\n"
    "\n"
    "2.  Other Things\n"
    "\n"
    "   A sender MUST retry.\n"
    "\n"
    "3.  Relays\n"
    "\n"
    "   A relay SHOULD NOT forward.\n"
    "\n"
    "   An endpoint MUST close the session.\n";

TEST(CarryForwardMatchTest, ClassifiesIdenticalMovedRewordedNewAndRemoved) {
    const auto old_source = source_from_text(21, kOld);
    const auto old_catalog = catalog_for(old_source);
    const auto new_source = source_from_text(22, kNew);
    const auto result = carry_forward(old_source, old_catalog, new_source);
    ASSERT_EQ(result.matches.size(), 5u);
    EXPECT_EQ(result.matches[0].change, DeltaClass::Reworded);
    EXPECT_GE(result.matches[0].similarity, 0.6);
    ASSERT_EQ(result.matches[0].sources.size(), 1u);
    EXPECT_EQ(result.matches[0].sources[0]->id, "D21-1-MUST-1");
    EXPECT_EQ(result.matches[1].change, DeltaClass::Identical);
    EXPECT_EQ(result.matches[1].sources[0]->id, "D21-1-MAY-2");
    EXPECT_EQ(result.matches[2].change, DeltaClass::Identical);
    EXPECT_EQ(result.matches[2].sources[0]->id, "D21-1-MUST-4");
    EXPECT_EQ(result.matches[3].change, DeltaClass::Moved);
    EXPECT_EQ(result.matches[3].sources[0]->id, "D21-1-SHOULD-NOT-3");
    EXPECT_EQ(result.matches[4].change, DeltaClass::New);
    EXPECT_TRUE(result.matches[4].sources.empty());
    EXPECT_TRUE(result.removed.empty());
}

TEST(CarryForwardMatchTest, ReportsUnmatchedDraft21RowsAsRemoved) {
    const auto old_source = source_from_text(21, kOld);
    const auto old_catalog = catalog_for(old_source);
    const auto new_source = source_from_text(22,
        "1.  Introduction\n\n   A peer MAY ignore it.\n");
    const auto result = carry_forward(old_source, old_catalog, new_source);
    ASSERT_EQ(result.matches.size(), 1u);
    EXPECT_EQ(result.matches[0].change, DeltaClass::Identical);
    ASSERT_EQ(result.removed.size(), 3u);
}

TEST(CarryForwardMatchTest, DuplicateSentencesInTwoSectionsMatchOneToOne) {
    const auto old_source = source_from_text(21,
        "1.  A\n\n   *  It MUST be first.\n\n2.  B\n\n   *  It MUST be first.\n");
    const auto old_catalog = catalog_for(old_source);
    const auto new_source = source_from_text(22,
        "1.  A\n\n   *  It MUST be first.\n\n2.  B\n\n   *  It MUST be first.\n");
    const auto result = carry_forward(old_source, old_catalog, new_source);
    ASSERT_EQ(result.matches.size(), 2u);
    EXPECT_EQ(result.matches[0].sources[0]->id, "D21-1-MUST-1");
    EXPECT_EQ(result.matches[1].sources[0]->id, "D21-1-MUST-2");
    EXPECT_EQ(result.matches[0].change, DeltaClass::Identical);
    EXPECT_EQ(result.matches[1].change, DeltaClass::Identical);
}

TEST(CarryForwardMatchTest, KeywordsInOneSentenceMatchByOrdinal) {
    const char* text = "1.  A\n\n   The words \"MUST\", \"MUST NOT\" and \"MAY\" are defined.\n";
    const auto old_source = source_from_text(21, text);
    const auto old_catalog = catalog_for(old_source);
    const auto result = carry_forward(old_source, old_catalog, source_from_text(22, text));
    ASSERT_EQ(result.matches.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(result.matches[i].change, DeltaClass::Identical);
        EXPECT_EQ(result.matches[i].sources[0]->source.occurrence, i + 1);
    }
}

TEST(CarryForwardWireTest, ExtractsNamedBlocksAcrossPageBreaks) {
    const auto source = source_from_text(22,
        "   LOCATION_FILTER Parameter {\n"
        "     Parameter Type (vi64) = 0x21,\n"
        "     Location Filter Type (vi64),\n"
        "   }\n"
        "\n"
        "   GOAWAY Message {\n"
        "     Type (vi64) = 0x10,\n"
        "   }\n");
    const auto blocks = extract_wire_blocks(source);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks.at("LOCATION_FILTER Parameter"),
              "Parameter Type (vi64) = 0x21, Location Filter Type (vi64),");
    EXPECT_EQ(blocks.at("GOAWAY Message"), "Type (vi64) = 0x10,");
}

TEST(CarryForwardWireTest, ReportsAddedRemovedAndChangedBlocks) {
    const auto old_source = source_from_text(21,
        "   A Message {\n     X (i),\n   }\n\n   B Message {\n     Y (i),\n   }\n\n"
        "   C Message {\n     Z (i),\n   }\n");
    const auto new_source = source_from_text(22,
        "   A Message {\n     X (i),\n   }\n\n   B Message {\n     Y (i),\n     Q (i),\n   }\n\n"
        "   D Message {\n     W (i),\n   }\n");
    const auto delta = diff_wire_blocks(old_source, new_source);
    EXPECT_EQ(delta.added, std::vector<std::string>{"D Message"});
    EXPECT_EQ(delta.removed, std::vector<std::string>{"C Message"});
    EXPECT_EQ(delta.changed, std::vector<std::string>{"B Message"});
}

}  // namespace
}  // namespace moq::interop::requirements
