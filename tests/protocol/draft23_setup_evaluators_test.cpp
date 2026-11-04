#include "moq/interop/requirements/draft23_evaluators.h"
#include "moq/interop/requirements/draft_source.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>

namespace {
using namespace moq::interop;
using requirements::Draft23SetupContext;
using requirements::OutcomeState;

const requirements::RequirementCatalog& catalog() {
    static const auto value = [] {
        const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
        const auto source = requirements::load_draft_source(23, root / "docs", root / "requirements/draft-digests.json");
        return requirements::RequirementCatalog::load(source, root / "requirements/draft23.json",
                                                      requirements::CatalogLoadMode::AllowIncomplete);
    }();
    return value;
}
std::vector<std::byte> text(std::string_view value) {
    const auto view = std::as_bytes(std::span(value));
    return {view.begin(), view.end()};
}
std::vector<std::byte> vi(std::uint64_t value) {  // one- or two-byte varint, enough for these tests
    if (value < 64) return {static_cast<std::byte>(value)};
    return {static_cast<std::byte>(0x40 | (value >> 8)), static_cast<std::byte>(value & 0xff)};
}
wire::draft21::KeyValue option(std::uint64_t type, std::vector<std::byte> value) { return {type, std::move(value)}; }

Draft23SetupContext native(std::vector<wire::draft21::KeyValue> options) {
    Draft23SetupContext context;
    context.webtransport = false;
    context.authority = "127.0.0.1:4443";
    context.path = "/moq";
    session::Draft23PeerSetup setup;
    setup.options = std::move(options);
    setup.frame_length = 20;
    context.peer_setup = std::move(setup);
    return context;
}
std::map<std::string, OutcomeState> run(const Draft23SetupContext& context) {
    std::map<std::string, OutcomeState> result;
    for (const auto& outcome : requirements::evaluate_draft23_setup(catalog(), context)) {
        EXPECT_TRUE(result.emplace(outcome.requirement_id, outcome.state).second) << outcome.requirement_id;
    }
    return result;
}
constexpr auto kMultiplicity = "D23-9-1-MUST-NOT-265";
constexpr auto kMaxSize = "D23-9-1-8-MUST-288";
constexpr auto kAuthority = "D23-9-1-1-MUST-269";
constexpr auto kPath = "D23-9-1-2-MUST-273";
constexpr auto kQuery = "D23-9-1-2-MUST-274";

TEST(Draft23SetupEvaluators, ReturnsExactlyOneOutcomeForEveryCatalogRow) {
    const auto outcomes = run(native({}));
    EXPECT_EQ(outcomes.size(), catalog().requirements.size());
}
TEST(Draft23SetupEvaluators, EveryRowIsNotRunBeforeSetupArrives) {
    Draft23SetupContext context;
    context.authority = "127.0.0.1:4443";
    context.path = "/moq";
    const auto outcomes = run(context);
    for (const auto id : {kMultiplicity, kMaxSize, kAuthority, kPath, kQuery}) EXPECT_EQ(outcomes.at(id), OutcomeState::NotRun) << id;
}
TEST(Draft23SetupEvaluators, UnreviewedRowsStayNotRunAndClassifiedRowsKeepTheirState) {
    const auto outcomes = run(native({}));
    for (const auto& row : catalog().requirements) {
        if (!row.reviewed) EXPECT_EQ(outcomes.at(row.id), OutcomeState::NotRun) << row.id;
        else if (row.testability == requirements::Testability::NotTestable) EXPECT_EQ(outcomes.at(row.id), OutcomeState::NotTestable) << row.id;
        else if (row.testability == requirements::Testability::NotApplicable) EXPECT_EQ(outcomes.at(row.id), OutcomeState::NotApplicable) << row.id;
    }
}
TEST(Draft23SetupEvaluators, MatchingAuthorityAndPathPass) {
    const auto outcomes = run(native({option(1, text("/moq")), option(5, text("127.0.0.1:4443"))}));
    EXPECT_EQ(outcomes.at(kAuthority), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kPath), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kMultiplicity), OutcomeState::Pass);
}
TEST(Draft23SetupEvaluators, MissingOrWrongAuthorityFails) {
    EXPECT_EQ(run(native({option(1, text("/moq"))})).at(kAuthority), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(5, text("example.test:1"))})).at(kAuthority), OutcomeState::Fail);
}
TEST(Draft23SetupEvaluators, MissingOrWrongPathFails) {
    EXPECT_EQ(run(native({option(5, text("127.0.0.1:4443"))})).at(kPath), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(1, text("/other"))})).at(kPath), OutcomeState::Fail);
}
TEST(Draft23SetupEvaluators, WebTransportMakesNativeOnlyRowsNotApplicable) {
    auto context = native({});
    context.webtransport = true;
    const auto outcomes = run(context);
    for (const auto id : {kAuthority, kPath, kQuery}) EXPECT_EQ(outcomes.at(id), OutcomeState::NotApplicable) << id;
}
TEST(Draft23SetupEvaluators, QueryRowNeedsAQueryInTheAdvertisedEndpoint) {
    auto context = native({option(1, text("/moq")), option(5, text("127.0.0.1:4443"))});
    EXPECT_EQ(run(context).at(kQuery), OutcomeState::NotApplicable);
    context.path = "/moq";
    context.query = "room=a";
    context.peer_setup->options = {option(1, text("/moq?room=a")), option(5, text("127.0.0.1:4443"))};
    auto outcomes = run(context);
    EXPECT_EQ(outcomes.at(kQuery), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kPath), OutcomeState::Pass);
    context.peer_setup->options = {option(1, text("/moq")), option(5, text("127.0.0.1:4443"))};
    outcomes = run(context);
    EXPECT_EQ(outcomes.at(kQuery), OutcomeState::Fail);
    EXPECT_EQ(outcomes.at(kPath), OutcomeState::Pass);  // the path portion is right; only the query is missing
}
TEST(Draft23SetupEvaluators, RepeatingASingleInstanceOptionFails) {
    const auto outcomes = run(native({option(7, text("a")), option(7, text("b"))}));
    EXPECT_EQ(outcomes.at(kMultiplicity), OutcomeState::Fail);
}
TEST(Draft23SetupEvaluators, RepeatingAuthorizationTokensAndUnknownOptionsIsNotJudged) {
    const auto outcomes = run(native({option(3, text("x")), option(3, text("y")), option(0x41, text("u")), option(0x41, text("v"))}));
    EXPECT_EQ(outcomes.at(kMultiplicity), OutcomeState::Pass);
}
TEST(Draft23SetupEvaluators, MaximumControlMessageSizeBoundaries) {
    EXPECT_EQ(run(native({option(9, vi(0))})).at(kMaxSize), OutcomeState::Fail);
    EXPECT_EQ(run(native({})).at(kMaxSize), OutcomeState::NotApplicable);
    // Draft 23 vi64: 65535 is c0 ff ff (see the wire tests).
    EXPECT_EQ(run(native({option(9, {std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}})})).at(kMaxSize), OutcomeState::Pass);
    EXPECT_EQ(run(native({option(9, {std::byte{0xc0}, std::byte{0xff}, std::byte{0xfe}})})).at(kMaxSize), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(9, {std::byte{0x40}})})).at(kMaxSize), OutcomeState::Fail);  // truncated varint
}
TEST(Draft23SetupEvaluators, BindingsNameEveryTestableRowOnBothSmokeScenarios) {
    const auto bindings = requirements::draft23_executable_bindings();
    std::map<std::string, int> per_row;
    for (const auto& binding : bindings) {
        EXPECT_EQ(binding.draft, 23u);
        EXPECT_EQ(binding.evidence_kinds, std::vector<std::string>{"peer_setup_received"});
        ++per_row[binding.requirement_id];
    }
    int testable = 0;
    for (const auto& row : catalog().requirements) {
        if (row.testability != requirements::Testability::Testable) continue;
        ++testable;
        EXPECT_EQ(per_row[row.id], 2) << row.id;
    }
    EXPECT_EQ(per_row.size(), static_cast<std::size_t>(testable));
    EXPECT_EQ(testable, 5);
}
}  // namespace
