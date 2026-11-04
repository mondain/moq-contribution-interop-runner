#include "moq/interop/requirements/draft23_evaluators.h"
#include "moq/interop/requirements/draft_source.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>

namespace {
using namespace moq::interop;
using requirements::Draft23Evidence;
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

Draft23Evidence native(std::vector<wire::draft21::KeyValue> options) {
    Draft23Evidence context;
    context.webtransport = false;
    context.authority = "127.0.0.1:4443";
    context.path = "/moq";
    session::Draft23PeerSetup setup;
    setup.options = std::move(options);
    setup.frame_length = 20;
    context.peer_setup = std::move(setup);
    return context;
}
std::map<std::string, OutcomeState> run(const Draft23Evidence& context) {
    std::map<std::string, OutcomeState> result;
    for (const auto& outcome : requirements::evaluate_draft23(catalog(), context)) {
        EXPECT_TRUE(result.emplace(outcome.requirement_id, outcome.state).second) << outcome.requirement_id;
    }
    return result;
}
constexpr auto kMultiplicity = "D23-9-1-MUST-NOT-265";
constexpr auto kMaxSize = "D23-9-1-8-MUST-288";
constexpr auto kAuthority = "D23-9-1-1-MUST-269";
constexpr auto kPath = "D23-9-1-2-MUST-273";
constexpr auto kQuery = "D23-9-1-2-MUST-274";

TEST(Draft23Evaluators, ReturnsExactlyOneOutcomeForEveryCatalogRow) {
    const auto outcomes = run(native({}));
    EXPECT_EQ(outcomes.size(), catalog().requirements.size());
}
TEST(Draft23Evaluators, EveryRowIsNotRunBeforeSetupArrives) {
    Draft23Evidence context;
    context.authority = "127.0.0.1:4443";
    context.path = "/moq";
    const auto outcomes = run(context);
    for (const auto id : {kMultiplicity, kMaxSize, kAuthority, kPath, kQuery}) EXPECT_EQ(outcomes.at(id), OutcomeState::NotRun) << id;
}
TEST(Draft23Evaluators, UnreviewedRowsStayNotRunAndClassifiedRowsKeepTheirState) {
    const auto outcomes = run(native({}));
    for (const auto& row : catalog().requirements) {
        if (!row.reviewed) EXPECT_EQ(outcomes.at(row.id), OutcomeState::NotRun) << row.id;
        else if (row.testability == requirements::Testability::NotTestable) EXPECT_EQ(outcomes.at(row.id), OutcomeState::NotTestable) << row.id;
        else if (row.testability == requirements::Testability::NotApplicable) EXPECT_EQ(outcomes.at(row.id), OutcomeState::NotApplicable) << row.id;
    }
}
TEST(Draft23Evaluators, MatchingAuthorityAndPathPass) {
    const auto outcomes = run(native({option(1, text("/moq")), option(5, text("127.0.0.1:4443"))}));
    EXPECT_EQ(outcomes.at(kAuthority), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kPath), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kMultiplicity), OutcomeState::Pass);
}
TEST(Draft23Evaluators, MissingOrWrongAuthorityFails) {
    EXPECT_EQ(run(native({option(1, text("/moq"))})).at(kAuthority), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(5, text("example.test:1"))})).at(kAuthority), OutcomeState::Fail);
}
TEST(Draft23Evaluators, MissingOrWrongPathFails) {
    EXPECT_EQ(run(native({option(5, text("127.0.0.1:4443"))})).at(kPath), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(1, text("/other"))})).at(kPath), OutcomeState::Fail);
}
TEST(Draft23Evaluators, WebTransportMakesNativeOnlyRowsNotApplicable) {
    auto context = native({});
    context.webtransport = true;
    const auto outcomes = run(context);
    for (const auto id : {kAuthority, kPath, kQuery}) EXPECT_EQ(outcomes.at(id), OutcomeState::NotApplicable) << id;
}
TEST(Draft23Evaluators, QueryRowNeedsAQueryInTheAdvertisedEndpoint) {
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
TEST(Draft23Evaluators, RepeatingASingleInstanceOptionFails) {
    const auto outcomes = run(native({option(7, text("a")), option(7, text("b"))}));
    EXPECT_EQ(outcomes.at(kMultiplicity), OutcomeState::Fail);
}
TEST(Draft23Evaluators, RepeatingAuthorizationTokensAndUnknownOptionsIsNotJudged) {
    const auto outcomes = run(native({option(3, text("x")), option(3, text("y")), option(0x41, text("u")), option(0x41, text("v"))}));
    EXPECT_EQ(outcomes.at(kMultiplicity), OutcomeState::Pass);
}
TEST(Draft23Evaluators, MaximumControlMessageSizeBoundaries) {
    EXPECT_EQ(run(native({option(9, vi(0))})).at(kMaxSize), OutcomeState::Fail);
    EXPECT_EQ(run(native({})).at(kMaxSize), OutcomeState::NotApplicable);
    // Draft 23 vi64: 65535 is c0 ff ff (see the wire tests).
    EXPECT_EQ(run(native({option(9, {std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}})})).at(kMaxSize), OutcomeState::Pass);
    EXPECT_EQ(run(native({option(9, {std::byte{0xc0}, std::byte{0xff}, std::byte{0xfe}})})).at(kMaxSize), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(9, {std::byte{0x40}})})).at(kMaxSize), OutcomeState::Fail);  // truncated varint
}
TEST(Draft23Evaluators, BindingsNameEveryTestableRowOnBothSmokeScenarios) {
    const auto bindings = requirements::draft23_executable_bindings();
    std::map<std::string, int> per_row;
    for (const auto& binding : bindings) {
        EXPECT_EQ(binding.draft, 23u);
        EXPECT_FALSE(binding.evidence_kinds.empty());
        ++per_row[binding.requirement_id];
    }
    int testable = 0;
    for (const auto& row : catalog().requirements) {
        if (row.testability != requirements::Testability::Testable) continue;
        ++testable;
        EXPECT_EQ(per_row[row.id], 2) << row.id;
    }
    EXPECT_EQ(per_row.size(), static_cast<std::size_t>(testable));
    EXPECT_EQ(testable, 7);
}

constexpr auto kBitFour = "D23-11-3-1-MUST-445";
constexpr auto kOneStream = "D23-2-2-MUST-NOT-020";
session::Draft23DataStream stream(std::uint64_t id, std::uint64_t flags, std::uint64_t alias, std::uint64_t group,
                                  std::optional<std::uint64_t> subgroup, bool reset = false) {
    session::Draft23DataStream result;
    result.stream_id = id; result.type_flags = flags; result.valid_header = (flags & 0x10) != 0 && flags < 128;
    result.track_alias = alias; result.group_id = group; result.subgroup_id = subgroup; result.reset = reset;
    return result;
}
Draft23Evidence with_streams(std::vector<session::Draft23DataStream> streams) {
    auto context = native({});
    context.data_streams = std::move(streams);
    return context;
}
TEST(Draft23Evaluators, StreamRowsAreNotRunWhenNoDataStreamWasObserved) {
    const auto outcomes = run(native({}));
    EXPECT_EQ(outcomes.at(kBitFour), OutcomeState::NotRun);
    EXPECT_EQ(outcomes.at(kOneStream), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, ValidSubgroupHeadersPassBitFour) {
    EXPECT_EQ(run(with_streams({stream(6, 0x30, 7, 0, 0), stream(10, 0x32, 7, 1, 4)})).at(kBitFour), OutcomeState::Pass);
}
TEST(Draft23Evaluators, AHeaderWithBitFourClearFails) {
    EXPECT_EQ(run(with_streams({stream(6, 0x30, 7, 0, 0), stream(10, 0x20, 0, 0, std::nullopt)})).at(kBitFour), OutcomeState::Fail);
}
TEST(Draft23Evaluators, FetchHeaderAndOversizedTypesAreNotABitFourViolation) {
    EXPECT_EQ(run(with_streams({stream(6, 0x30, 7, 0, 0), stream(10, 5, 0, 0, std::nullopt),
                                stream(14, 0x1000, 0, 0, std::nullopt)})).at(kBitFour), OutcomeState::Pass);
}
TEST(Draft23Evaluators, DistinctSubgroupsOnDistinctStreamsPass) {
    const auto outcomes = run(with_streams({stream(6, 0x30, 7, 0, 0), stream(10, 0x30, 7, 0, 1), stream(14, 0x30, 7, 1, 0),
                                            stream(18, 0x30, 8, 0, 0)}));
    EXPECT_EQ(outcomes.at(kOneStream), OutcomeState::Pass);
}
TEST(Draft23Evaluators, OneSubgroupOnTwoStreamsFails) {
    EXPECT_EQ(run(with_streams({stream(6, 0x30, 7, 0, 0), stream(10, 0x30, 7, 0, 0)})).at(kOneStream), OutcomeState::Fail);
}
TEST(Draft23Evaluators, ResetStreamExcusesASecondStreamForTheSameSubgroup) {
    EXPECT_EQ(run(with_streams({stream(6, 0x30, 7, 0, 0, true), stream(10, 0x30, 7, 0, 0)})).at(kOneStream), OutcomeState::Pass);
}
TEST(Draft23Evaluators, StreamsWithoutAKnownSubgroupAreNotJudged) {
    EXPECT_EQ(run(with_streams({stream(6, 0x32, 7, 0, std::nullopt), stream(10, 0x32, 7, 0, std::nullopt)})).at(kOneStream),
              OutcomeState::NotRun);
}
}  // namespace
