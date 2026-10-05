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

}  // namespace
}  // namespace moq::interop::requirements
