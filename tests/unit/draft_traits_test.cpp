#include "moq/interop/app/draft_traits.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace moq::interop::app {
namespace {

constexpr DraftVersion kAllDrafts[] = {DraftVersion::Draft18, DraftVersion::Draft21,
                                       DraftVersion::Draft22};

static_assert(draft_number(DraftVersion::Draft18) == 18);
static_assert(draft_number(DraftVersion::Draft21) == 21);
static_assert(draft_number(DraftVersion::Draft22) == 22);
static_assert(alpn(DraftVersion::Draft22) == "moqt-22");
static_assert(runnable(DraftVersion::Draft18) && runnable(DraftVersion::Draft21));
static_assert(!runnable(DraftVersion::Draft22));

TEST(DraftTraits, EveryFunctionHandlesEveryDraft) {
    for (const auto draft : kAllDrafts) {
        const auto number = draft_number(draft);
        EXPECT_EQ(static_cast<unsigned>(draft), number);
        EXPECT_EQ(std::string(alpn(draft)), "moqt-" + std::to_string(number));
        ASSERT_TRUE(parse_draft(number).has_value());
        EXPECT_EQ(*parse_draft(number), draft);
        EXPECT_TRUE(known_alpn(alpn(draft)));
    }
}

TEST(DraftTraits, ParseDraftRejectsEverythingElse) {
    for (const unsigned value : {0u, 1u, 17u, 19u, 20u, 23u, 100u, 4294967295u}) {
        EXPECT_FALSE(parse_draft(value).has_value()) << value;
    }
}

TEST(DraftTraits, KnownAlpnIsExactlyTheThreeProtocols) {
    EXPECT_TRUE(known_alpn("moqt-18"));
    EXPECT_TRUE(known_alpn("moqt-21"));
    EXPECT_TRUE(known_alpn("moqt-22"));
    for (const char* other : {"", "moqt-19", "moqt-20", "moqt-23", "moqt-2", "MOQT-22", "moqt-22 ",
                              "h3", "moq-00"}) {
        EXPECT_FALSE(known_alpn(other)) << other;
    }
}

TEST(DraftTraits, ByDraftRunsOnlyTheMatchingCallable) {
    int ran_18 = 0;
    int ran_21 = 0;
    const auto pick = [&](DraftVersion draft) {
        return by_draft(draft, [&] { ++ran_18; return 18; }, [&] { ++ran_21; return 21; });
    };
    EXPECT_EQ(pick(DraftVersion::Draft18), 18);
    EXPECT_EQ(ran_18, 1);
    EXPECT_EQ(ran_21, 0);
    EXPECT_EQ(pick(DraftVersion::Draft21), 21);
    EXPECT_EQ(ran_18, 1);
    EXPECT_EQ(ran_21, 1);
}

TEST(DraftTraits, ByDraftRefusesDraft22WithoutRunningEitherCallable) {
    int ran = 0;
    EXPECT_THROW(by_draft(DraftVersion::Draft22, [&] { ++ran; return 0; }, [&] { ++ran; return 0; }),
                 std::logic_error);
    EXPECT_EQ(ran, 0);
}

TEST(DraftTraits, ByDraftSupportsReferenceReturns) {
    const std::string a = "eighteen";
    const std::string b = "twenty-one";
    const auto& chosen = by_draft(
        DraftVersion::Draft21, [&]() -> const std::string& { return a; },
        [&]() -> const std::string& { return b; });
    EXPECT_EQ(&chosen, &b);
}

TEST(DraftTraits, FamilyDraftMapsDraft22ToDraft21AndLeavesTheOthers) {
    EXPECT_EQ(family_draft(DraftVersion::Draft18), DraftVersion::Draft18);
    EXPECT_EQ(family_draft(DraftVersion::Draft21), DraftVersion::Draft21);
    EXPECT_EQ(family_draft(DraftVersion::Draft22), DraftVersion::Draft21);
}

}  // namespace
}  // namespace moq::interop::app
