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
    std::map<std::string, std::size_t> per_row;
    for (const auto& binding : bindings) {
        EXPECT_EQ(binding.draft, 23u);
        EXPECT_FALSE(binding.evidence_kinds.empty());
        for (const auto& kind : binding.evidence_kinds) EXPECT_FALSE(kind.empty());
        ++per_row[binding.requirement_id];
    }
    int testable = 0;
    for (const auto& row : catalog().requirements) {
        if (row.testability != requirements::Testability::Testable) continue;
        ++testable;
        EXPECT_EQ(per_row[row.id], row.scenarios.size()) << row.id;
    }
    EXPECT_EQ(per_row.size(), static_cast<std::size_t>(testable));
    EXPECT_EQ(testable, 21);
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

constexpr auto kLength = "D23-9-MUST-NOT-259";
constexpr auto kRequestId = "D23-6-4-2-1-MUST-NOT-147";
constexpr auto kSingleResponse = "D23-3-1-MUST-036";
constexpr auto kFinRequired = "D23-6-4-2-2-MUST-NOT-150";
constexpr auto kResponseBeforeFin = "D23-6-4-2-2-MUST-151";
constexpr auto kDoneBeforeFin = "D23-6-4-2-2-MUST-152";
constexpr auto kStreamCount = "D23-9-9-MUST-342";

session::Draft23RequestStream request_stream(std::uint64_t id, bool local, std::vector<std::uint64_t> types) {
    session::Draft23RequestStream result;
    result.stream_id = id; result.local = local;
    for (const auto type : types) result.messages.push_back({type, 4});
    return result;
}
Draft23Evidence with_requests(std::vector<session::Draft23RequestStream> requests, bool direct = true) {
    auto context = native({});
    context.direct_subscribe = direct;
    context.requests = std::move(requests);
    return context;
}
TEST(Draft23Evaluators, LifecycleRowsAreNotRunWithoutRequestEvidence) {
    const auto outcomes = run(native({}));
    for (const auto id : {kRequestId, kSingleResponse, kFinRequired, kResponseBeforeFin, kDoneBeforeFin, kStreamCount})
        EXPECT_NE(outcomes.at(id), OutcomeState::Pass) << id;
    EXPECT_EQ(outcomes.at(kRequestId), OutcomeState::NotRun);
    EXPECT_EQ(outcomes.at(kDoneBeforeFin), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, ControlMessageLengthPassesWithinTheDefaultLimitAndFailsOverIt) {
    EXPECT_EQ(run(native({})).at(kLength), OutcomeState::Pass);  // SETUP frame_length 20
    auto over = native({});
    over.oversize_control_message = true;
    EXPECT_EQ(run(over).at(kLength), OutcomeState::Fail);
    auto long_message = with_requests({request_stream(0, false, {6})});
    long_message.requests[0].messages[0].body_length = 65536;
    EXPECT_EQ(run(long_message).at(kLength), OutcomeState::Fail);
    EXPECT_EQ(run(Draft23Evidence{}).at(kLength), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, RequestIdReuseFails) {
    auto streams = with_requests({request_stream(0, false, {6})});
    streams.requests[0].request_id = 0;
    EXPECT_EQ(run(streams).at(kRequestId), OutcomeState::Pass);
    streams.duplicate_request_id = true;
    EXPECT_EQ(run(streams).at(kRequestId), OutcomeState::Fail);
}
TEST(Draft23Evaluators, SubscribeNeedsExactlyOneResponse) {
    EXPECT_EQ(run(with_requests({request_stream(1, true, {4})})).at(kSingleResponse), OutcomeState::Pass);
    EXPECT_EQ(run(with_requests({request_stream(1, true, {4, 5})})).at(kSingleResponse), OutcomeState::Fail);
    EXPECT_EQ(run(with_requests({request_stream(1, true, {4, 4})})).at(kSingleResponse), OutcomeState::Fail);
    EXPECT_EQ(run(with_requests({request_stream(1, true, {})})).at(kSingleResponse), OutcomeState::NotRun);
    EXPECT_EQ(run(with_requests({request_stream(0, false, {0x1d})}, false)).at(kSingleResponse), OutcomeState::NotApplicable);
}
TEST(Draft23Evaluators, FinBeforeTheSubscribeResponseFails) {
    auto early = request_stream(1, true, {});
    early.fin = true;
    EXPECT_EQ(run(with_requests({early})).at(kResponseBeforeFin), OutcomeState::Fail);
    EXPECT_EQ(run(with_requests({early})).at(kFinRequired), OutcomeState::Fail);
    auto answered = request_stream(1, true, {4});
    answered.fin = true;
    EXPECT_EQ(run(with_requests({answered})).at(kResponseBeforeFin), OutcomeState::Pass);
    EXPECT_EQ(run(with_requests({request_stream(0, false, {0x1d})}, false)).at(kResponseBeforeFin), OutcomeState::NotApplicable);
}
TEST(Draft23Evaluators, PublishDoneMustPrecedeTheFinOfAnEstablishedSubscription) {
    auto bad = request_stream(1, true, {4});
    bad.established = true; bad.fin = true; bad.fin_after_publish_done = false;
    EXPECT_EQ(run(with_requests({bad})).at(kDoneBeforeFin), OutcomeState::Fail);
    EXPECT_EQ(run(with_requests({bad})).at(kFinRequired), OutcomeState::Fail);
    auto good = request_stream(1, true, {4, 0x0b});
    good.established = true; good.fin = true; good.fin_after_publish_done = true;
    good.publish_done = session::Draft23PublishDone{0, 1};
    EXPECT_EQ(run(with_requests({good})).at(kDoneBeforeFin), OutcomeState::Pass);
    EXPECT_EQ(run(with_requests({good})).at(kFinRequired), OutcomeState::Pass);
    auto open = request_stream(1, true, {4});
    open.established = true;
    EXPECT_EQ(run(with_requests({open})).at(kDoneBeforeFin), OutcomeState::NotRun);
    EXPECT_EQ(run(with_requests({open})).at(kFinRequired), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, PublishFirstSubscriptionsAreCheckedForPublishDoneToo) {
    auto good = request_stream(0, false, {0x1d, 0x0b});
    good.established = true; good.fin = true; good.fin_after_publish_done = true;
    good.publish_done = session::Draft23PublishDone{0, 0};
    const auto outcomes = run(with_requests({good}, false));
    EXPECT_EQ(outcomes.at(kDoneBeforeFin), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kFinRequired), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kResponseBeforeFin), OutcomeState::NotApplicable);
}
TEST(Draft23Evaluators, StreamCountComparesWithStreamsReceivedForTheAlias) {
    auto done = request_stream(1, true, {4, 0x0b});
    done.established = true;
    auto make = [&](std::uint64_t count, int streams, std::uint64_t other_alias_streams = 0) {
        auto context = with_requests({done});
        context.requests[0].publish_done = session::Draft23PublishDone{0, count};
        context.subscription_alias = 7;
        for (int i = 0; i < streams; ++i) context.data_streams.push_back(stream(static_cast<std::uint64_t>(6 + 4 * i), 0x30, 7, static_cast<std::uint64_t>(i), 0));
        for (std::uint64_t i = 0; i < other_alias_streams; ++i) context.data_streams.push_back(stream(100 + 4 * i, 0x30, 9, i, 0));
        return context;
    };
    EXPECT_EQ(run(make(2, 2)).at(kStreamCount), OutcomeState::Pass);
    EXPECT_EQ(run(make(1, 2)).at(kStreamCount), OutcomeState::Fail);
    EXPECT_EQ(run(make(3, 2)).at(kStreamCount), OutcomeState::NotRun);  // a late stream may still be in flight
    EXPECT_EQ(run(make(~std::uint64_t{0}, 2)).at(kStreamCount), OutcomeState::Pass);
    EXPECT_EQ(run(make(2, 2, 5)).at(kStreamCount), OutcomeState::Pass);  // other tracks' streams are not counted
    EXPECT_EQ(run(with_requests({request_stream(1, true, {4})})).at(kStreamCount), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, PublishDoneWithoutAnyFinStillSatisfiesTheOrdering) {
    // The rule only forbids a FIN before PUBLISH_DONE; once PUBLISH_DONE has arrived no earlier FIN can exist.
    auto done = request_stream(1, true, {4, 0x0b});
    done.established = true;
    done.publish_done = session::Draft23PublishDone{2, 2};
    const auto outcomes = run(with_requests({done}));
    EXPECT_EQ(outcomes.at(kDoneBeforeFin), OutcomeState::Pass);
    EXPECT_EQ(outcomes.at(kFinRequired), OutcomeState::Pass);
}

struct ProbeRow { const char* scenario; const char* row; std::uint64_t code; };
constexpr ProbeRow kProbeRows[] = {
    {"d23-probe-unknown-control-message", "D23-9-MUST-257", 0x303},
    {"d23-probe-bad-first-bidi-message", "D23-6-3-MUST-137", 0x303},
    {"d23-probe-length-mismatch", "D23-9-MUST-258", 0x300},
    {"d23-probe-oversize-control-message", "D23-9-MUST-260", 0x401},
    {"d23-probe-server-authority", "D23-9-1-1-MUST-268", 0x19},
    {"d23-probe-server-path", "D23-9-1-2-MUST-272", 0x8},
    {"d23-probe-unknown-uni-stream", "D23-6-4-1-MUST-146", 0x200},
};
Draft23Evidence probe_evidence(const ProbeRow& probe, bool injected, std::optional<session::Draft23ProbeClose> close) {
    Draft23Evidence context;
    context.probe_scenario = probe.scenario;
    session::Draft23ProbeResult result;
    result.injected = injected;
    result.close = close;
    context.probe = result;
    return context;
}
TEST(Draft23Evaluators, ProbePassesOnlyWithTheExpectedApplicationCloseCode) {
    for (const auto& probe : kProbeRows) {
        EXPECT_EQ(run(probe_evidence(probe, true, session::Draft23ProbeClose{true, probe.code})).at(probe.row), OutcomeState::Pass) << probe.row;
        EXPECT_EQ(run(probe_evidence(probe, true, session::Draft23ProbeClose{true, probe.code + 1})).at(probe.row), OutcomeState::Fail) << probe.row;
        EXPECT_EQ(run(probe_evidence(probe, true, session::Draft23ProbeClose{false, probe.code})).at(probe.row), OutcomeState::Fail) << probe.row;
        EXPECT_EQ(run(probe_evidence(probe, true, std::nullopt)).at(probe.row), OutcomeState::Fail) << probe.row;
        EXPECT_EQ(run(probe_evidence(probe, false, std::nullopt)).at(probe.row), OutcomeState::NotRun) << probe.row;
    }
}
TEST(Draft23Evaluators, OnlyTheProbeThatRanIsJudged) {
    const auto outcomes = run(probe_evidence(kProbeRows[0], true, session::Draft23ProbeClose{true, 0x303}));
    EXPECT_EQ(outcomes.at(kProbeRows[0].row), OutcomeState::Pass);
    for (std::size_t i = 1; i < std::size(kProbeRows); ++i) EXPECT_EQ(outcomes.at(kProbeRows[i].row), OutcomeState::NotRun) << kProbeRows[i].row;
}
TEST(Draft23Evaluators, SmokeRunsLeaveEveryProbeRowNotRun) {
    const auto outcomes = run(native({option(1, text("/moq")), option(5, text("127.0.0.1:4443"))}));
    for (const auto& probe : kProbeRows) EXPECT_EQ(outcomes.at(probe.row), OutcomeState::NotRun) << probe.row;
}
TEST(Draft23Evaluators, ProbeBindingsUseTheProbeScenarioAndPeerClosedEvidence) {
    std::size_t found = 0;
    for (const auto& binding : requirements::draft23_executable_bindings()) {
        for (const auto& probe : kProbeRows) {
            if (binding.requirement_id != probe.row) continue;
            ++found;
            EXPECT_EQ(binding.scenario_id, probe.scenario);
            EXPECT_EQ(binding.evidence_kinds, std::vector<std::string>{"peer_closed"});
        }
    }
    EXPECT_EQ(found, std::size(kProbeRows));
}
}  // namespace
