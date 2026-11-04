#include "moq/interop/requirements/draft23_evaluators.h"
#include "moq/interop/requirements/draft_source.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <set>

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
    EXPECT_EQ(testable, 100);
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

session::Draft23PublishedTrack published(std::string ns, std::string name, std::uint64_t alias) { return {std::move(ns), std::move(name), alias}; }
Draft23Evidence with_tracks(std::vector<session::Draft23PublishedTrack> tracks) {
    auto context = native({});
    context.published_tracks = std::move(tracks);
    return context;
}
TEST(Draft23Evaluators, EmptyNamesAndNamespaceFieldsFail) {
    constexpr auto kName = "D23-2-4-1-MUST-028";
    constexpr auto kField = "D23-8-7-MUST-232";
    EXPECT_EQ(run(native({})).at(kName), OutcomeState::NotRun);
    EXPECT_EQ(run(with_tracks({published("media", "vide", 1)})).at(kName), OutcomeState::Pass);
    EXPECT_EQ(run(with_tracks({published("media", "vide", 1)})).at(kField), OutcomeState::Pass);
    auto empty_name = with_tracks({published("media", "vide", 1)});
    empty_name.empty_track_name = true;
    EXPECT_EQ(run(empty_name).at(kName), OutcomeState::Fail);
    auto empty_field = with_tracks({published("media", "vide", 1)});
    empty_field.empty_namespace_field = true;
    EXPECT_EQ(run(empty_field).at(kField), OutcomeState::Fail);
}
TEST(Draft23Evaluators, ReservedNamespacesAreNotPublished) {
    for (const auto id : {"D23-2-4-3-MUST-NOT-032", "D23-2-4-3-MUST-NOT-033"}) {
        EXPECT_EQ(run(native({})).at(id), OutcomeState::NotRun) << id;
        EXPECT_EQ(run(with_tracks({published("media", "vide", 1)})).at(id), OutcomeState::Pass) << id;
        EXPECT_EQ(run(with_tracks({published(".", "x", 1)})).at(id), OutcomeState::Fail) << id;
        auto announced = native({});
        announced.announced_namespaces = {"."};
        EXPECT_EQ(run(announced).at(id), OutcomeState::Fail) << id;
        announced.announced_namespaces = {"media"};
        EXPECT_EQ(run(announced).at(id), OutcomeState::Pass) << id;
        EXPECT_EQ(run(with_tracks({published(".session", "x", 1)})).at(id), OutcomeState::Pass) << id;  // a different rule
    }
}
TEST(Draft23Evaluators, SessionNamespaceIsNotPublished) {
    constexpr auto kId = "D23-6-5-MUST-NOT-158";
    EXPECT_EQ(run(native({})).at(kId), OutcomeState::NotRun);
    EXPECT_EQ(run(with_tracks({published(".session", "x", 1)})).at(kId), OutcomeState::Fail);
    auto announced = native({});
    announced.announced_namespaces = {".session"};
    EXPECT_EQ(run(announced).at(kId), OutcomeState::Fail);
    EXPECT_EQ(run(with_tracks({published("media", "vide", 1)})).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(with_tracks({published(".", "x", 1)})).at(kId), OutcomeState::Pass);
}
TEST(Draft23Evaluators, TrackAliasesAreUniquePerTrack) {
    constexpr auto kId = "D23-3-1-3-MUST-NOT-050";
    EXPECT_EQ(run(with_tracks({published("media", "vide", 1)})).at(kId), OutcomeState::NotRun);
    EXPECT_EQ(run(with_tracks({published("media", "catalog", 1), published("media", "vide", 2)})).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(with_tracks({published("media", "catalog", 1), published("media", "vide", 1)})).at(kId), OutcomeState::Fail);
    EXPECT_EQ(run(with_tracks({published("media", "vide", 1), published("media", "vide", 1)})).at(kId), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, DatagramCapacityMeansTheExtensionWasNegotiated) {
    constexpr auto kId = "D23-6-2-MUST-135";
    EXPECT_EQ(run(Draft23Evidence{}).at(kId), OutcomeState::NotRun);
    auto negotiated = native({});
    negotiated.max_datagram_payload = 1150;
    EXPECT_EQ(run(negotiated).at(kId), OutcomeState::Pass);
    auto absent = native({});
    absent.max_datagram_payload = 0;
    absent.connected = true;
    EXPECT_EQ(run(absent).at(kId), OutcomeState::Fail);
}
TEST(Draft23Evaluators, FirstBidiMessageMustBeARequestOpener) {
    constexpr auto kId = "D23-6-3-MUST-NOT-136";
    EXPECT_EQ(run(native({})).at(kId), OutcomeState::NotRun);
    for (const std::uint64_t type : {0xdu, 3u, 0x1du, 0x16u, 6u, 0x50u, 0x51u})
        EXPECT_EQ(run(with_requests({request_stream(0, false, {type})})).at(kId), OutcomeState::Pass) << type;
    for (const std::uint64_t type : {4u, 5u, 7u, 0xbu, 0x3fu})
        EXPECT_EQ(run(with_requests({request_stream(0, false, {type})})).at(kId), OutcomeState::Fail) << type;
    // The runner's own SUBSCRIBE stream starts with the publisher's SUBSCRIBE_OK; only streams the publisher opened count.
    EXPECT_EQ(run(with_requests({request_stream(1, true, {4})})).at(kId), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, ControlStreamMustStayOpen) {
    constexpr auto kId = "D23-6-3-MUST-NOT-140";
    EXPECT_EQ(run(native({})).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(Draft23Evidence{}).at(kId), OutcomeState::NotRun);
    auto closed = native({});
    closed.control_stream_closed = true;
    EXPECT_EQ(run(closed).at(kId), OutcomeState::Fail);
}
TEST(Draft23Evaluators, DeltaEncodingMustNotOverflow) {
    for (const auto id : {"D23-8-3-MUST-NOT-210", "D23-9-19-MUST-351"}) {
        EXPECT_EQ(run(Draft23Evidence{}).at(id), OutcomeState::NotRun) << id;
        EXPECT_EQ(run(native({})).at(id), OutcomeState::Pass) << id;
        auto overflow = native({});
        overflow.delta_overflow = true;
        EXPECT_EQ(run(overflow).at(id), OutcomeState::Fail) << id;
    }
}
TEST(Draft23Evaluators, DuplicateParametersFail) {
    constexpr auto kId = "D23-9-19-MUST-NOT-355";
    EXPECT_EQ(run(native({})).at(kId), OutcomeState::NotRun);
    EXPECT_EQ(run(with_tracks({published("media", "vide", 1)})).at(kId), OutcomeState::Pass);
    auto duplicate = with_tracks({published("media", "vide", 1)});
    duplicate.duplicate_parameter = true;
    EXPECT_EQ(run(duplicate).at(kId), OutcomeState::Fail);
}
TEST(Draft23Evaluators, SubscribeIsAnsweredWithSubscribeOk) {
    constexpr auto kId = "D23-9-6-MUST-332";
    EXPECT_EQ(run(with_requests({request_stream(1, true, {4})})).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(with_requests({request_stream(1, true, {5})})).at(kId), OutcomeState::Fail);
    EXPECT_EQ(run(with_requests({request_stream(1, true, {})})).at(kId), OutcomeState::NotRun);
    EXPECT_EQ(run(with_requests({request_stream(0, false, {0x1d})}, false)).at(kId), OutcomeState::NotApplicable);
}
TEST(Draft23Evaluators, ZeroStreamCountNeedsNoStreamsReceived) {
    constexpr auto kId = "D23-9-9-MUST-341";
    auto make = [&](std::uint64_t count, int streams) {
        auto done = request_stream(1, true, {4, 0x0b});
        done.established = true;
        done.publish_done = session::Draft23PublishDone{0, count};
        auto context = with_requests({done});
        context.subscription_alias = 7;
        for (int i = 0; i < streams; ++i) context.data_streams.push_back(stream(static_cast<std::uint64_t>(6 + 4 * i), 0x30, 7, static_cast<std::uint64_t>(i), 0));
        return context;
    };
    EXPECT_EQ(run(make(0, 0)).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(make(3, 0)).at(kId), OutcomeState::NotRun);
    EXPECT_EQ(run(make(2, 2)).at(kId), OutcomeState::NotApplicable);
    EXPECT_EQ(run(native({})).at(kId), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, PaddingMustBeAllZero) {
    constexpr auto kId = "D23-11-5-1-MUST-475";
    EXPECT_EQ(run(native({})).at(kId), OutcomeState::NotRun);
    auto padded = native({});
    padded.padding_streams = 2;
    EXPECT_EQ(run(padded).at(kId), OutcomeState::Pass);
    padded.padding_nonzero = true;
    EXPECT_EQ(run(padded).at(kId), OutcomeState::Fail);
}

constexpr auto kReservedScenario = "d23-probe-subscribe-reserved-namespace";
constexpr auto kSessionScenario = "d23-probe-subscribe-session-namespace";
Draft23Evidence reply_evidence(const char* scenario, bool injected, std::optional<session::Draft23ProbeReply> reply,
                               std::optional<session::Draft23ProbeClose> close = std::nullopt) {
    Draft23Evidence context;
    context.probe_scenario = scenario;
    session::Draft23ProbeResult result;
    result.injected = injected;
    result.reply = std::move(reply);
    result.close = close;
    context.probe = result;
    return context;
}
session::Draft23ProbeReply request_error(std::uint64_t code, std::vector<std::uint64_t> details = {}) {
    session::Draft23ProbeReply reply;
    reply.type = 5; reply.error_code = code; reply.detail_types = std::move(details);
    return reply;
}
TEST(Draft23Evaluators, ReplyProbesPassOnlyOnADoesNotExistRequestError) {
    for (const auto& [scenario, row] : {std::pair{kReservedScenario, "D23-2-4-3-MUST-034"}, std::pair{kSessionScenario, "D23-6-5-MUST-160"}}) {
        EXPECT_EQ(run(reply_evidence(scenario, true, request_error(0x10))).at(row), OutcomeState::Pass) << row;
        EXPECT_EQ(run(reply_evidence(scenario, true, request_error(0x11))).at(row), OutcomeState::Fail) << row;
        session::Draft23ProbeReply accepted; accepted.type = 4;
        EXPECT_EQ(run(reply_evidence(scenario, true, accepted)).at(row), OutcomeState::Fail) << row;
        EXPECT_EQ(run(reply_evidence(scenario, true, std::nullopt, session::Draft23ProbeClose{true, 3})).at(row), OutcomeState::Fail) << row;
        EXPECT_EQ(run(reply_evidence(scenario, true, std::nullopt)).at(row), OutcomeState::Fail) << row;
        EXPECT_EQ(run(reply_evidence(scenario, false, std::nullopt)).at(row), OutcomeState::NotRun) << row;
    }
    // A probe is judged only on its own run.
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x10))).at("D23-6-5-MUST-160"), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, ErrorDetailTypesAreNotRepeated) {
    constexpr auto kId = "D23-9-4-2-MUST-NOT-311";
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x10, {1, 1}))).at(kId), OutcomeState::Fail);
    EXPECT_EQ(run(reply_evidence(kSessionScenario, true, request_error(0x10, {1}))).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x10))).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, std::nullopt)).at(kId), OutcomeState::NotRun);
    session::Draft23ProbeReply accepted; accepted.type = 4;
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, accepted)).at(kId), OutcomeState::NotRun);
    EXPECT_EQ(run(probe_evidence(kProbeRows[0], true, session::Draft23ProbeClose{true, 0x303})).at(kId), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, RedirectNeedsItsDetail) {
    constexpr auto kId = "D23-9-4-2-1-MUST-313";
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x34, {1}))).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x34))).at(kId), OutcomeState::Fail);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x34, {7}))).at(kId), OutcomeState::Fail);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x10))).at(kId), OutcomeState::NotApplicable);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, std::nullopt)).at(kId), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, ReplyProbeBindingsUseTheirOwnScenarios) {
    std::map<std::string, std::set<std::string>> scenarios;
    for (const auto& binding : requirements::draft23_executable_bindings()) scenarios[binding.requirement_id].insert(binding.scenario_id);
    EXPECT_EQ(scenarios.at("D23-2-4-3-MUST-034"), std::set<std::string>{kReservedScenario});
    EXPECT_EQ(scenarios.at("D23-6-5-MUST-160"), std::set<std::string>{kSessionScenario});
    const std::set<std::string> reply_probes{kReservedScenario, kSessionScenario, "d23-probe-filter-ranges-over-limit"};
    EXPECT_EQ(scenarios.at("D23-9-4-2-MUST-NOT-311"), reply_probes);
    EXPECT_EQ(scenarios.at("D23-9-4-2-1-MUST-313"), reply_probes);
}

Draft23Evidence evidence_for(const session::Draft23Probe& probe, bool injected, std::optional<session::Draft23ProbeClose> close,
                             std::optional<session::Draft23ProbeReply> reply = std::nullopt) {
    Draft23Evidence context;
    context.probe_scenario = std::string(probe.scenario_id);
    session::Draft23ProbeResult result;
    result.injected = injected;
    result.close = close;
    result.reply = std::move(reply);
    context.probe = result;
    return context;
}
TEST(Draft23Evaluators, EveryCloseProbeRowPassesOnlyOnItsExpectedCode) {
    for (const auto& probe : session::draft23_probes()) {
        if (probe.expect != session::Draft23ProbeExpect::Close) continue;
        for (const auto row : session::draft23_probe_rows(probe)) {
            const std::string id(row);
            EXPECT_EQ(run(evidence_for(probe, true, session::Draft23ProbeClose{true, probe.expected_code})).at(id), OutcomeState::Pass) << id;
            EXPECT_EQ(run(evidence_for(probe, true, session::Draft23ProbeClose{true, probe.expected_code + 1})).at(id), OutcomeState::Fail) << id;
            EXPECT_EQ(run(evidence_for(probe, true, session::Draft23ProbeClose{false, probe.expected_code})).at(id), OutcomeState::Fail) << id;
            EXPECT_EQ(run(evidence_for(probe, true, std::nullopt)).at(id), OutcomeState::Fail) << id;
            EXPECT_EQ(run(evidence_for(probe, false, std::nullopt)).at(id), OutcomeState::NotRun) << id;
        }
    }
}
TEST(Draft23Evaluators, ToleranceProbesPassWhenThePublisherKeepsTheSessionOpen) {
    std::size_t seen = 0;
    for (const auto& probe : session::draft23_probes()) {
        if (probe.expect != session::Draft23ProbeExpect::Tolerate) continue;
        ++seen;
        for (const auto row : session::draft23_probe_rows(probe)) {
            const std::string id(row);
            EXPECT_EQ(run(evidence_for(probe, true, std::nullopt)).at(id), OutcomeState::Pass) << id;
            EXPECT_EQ(run(evidence_for(probe, true, session::Draft23ProbeClose{true, 3})).at(id), OutcomeState::Fail) << id;
            EXPECT_EQ(run(evidence_for(probe, true, session::Draft23ProbeClose{false, 0})).at(id), OutcomeState::Fail) << id;
            // A graceful close (NO_ERROR) cannot be tied to the unknown input, so it proves nothing either way.
            EXPECT_EQ(run(evidence_for(probe, true, session::Draft23ProbeClose{true, 0})).at(id), OutcomeState::NotRun) << id;
            EXPECT_EQ(run(evidence_for(probe, false, std::nullopt)).at(id), OutcomeState::NotRun) << id;
        }
    }
    EXPECT_EQ(seen, 6u);
}
TEST(Draft23Evaluators, AProbeRowIsNeverJudgedOnAnotherProbesRun) {
    const auto probes = session::draft23_probes();
    const auto& ran = probes[0];
    const auto outcomes = run(evidence_for(ran, true, session::Draft23ProbeClose{true, ran.expected_code}));
    for (const auto& other : probes) {
        if (other.scenario_id == ran.scenario_id) continue;
        for (const auto row : session::draft23_probe_rows(other)) {
            if (row == ran.requirement_id) continue;
            EXPECT_EQ(outcomes.at(std::string(row)), OutcomeState::NotRun) << row;
        }
    }
}
TEST(Draft23Evaluators, EveryProbeRowIsBoundToItsProbeScenarioOnly) {
    std::map<std::string, std::set<std::string>> scenarios;
    for (const auto& binding : requirements::draft23_executable_bindings()) scenarios[binding.requirement_id].insert(binding.scenario_id);
    for (const auto& probe : session::draft23_probes())
        for (const auto row : session::draft23_probe_rows(probe))
            EXPECT_TRUE(scenarios.at(std::string(row)).contains(std::string(probe.scenario_id))) << row;
}

TEST(Draft23Evaluators, PublishDoneShouldBeFollowedByAFin) {
    constexpr auto kId = "D23-6-4-2-2-SHOULD-154";
    auto done = request_stream(1, true, {4, 0x0b});
    done.established = true;
    done.publish_done = session::Draft23PublishDone{2, 2};
    EXPECT_EQ(run(with_requests({done})).at(kId), OutcomeState::Fail);  // PUBLISH_DONE, then the stream stayed open
    done.fin = true; done.fin_after_publish_done = true;
    EXPECT_EQ(run(with_requests({done})).at(kId), OutcomeState::Pass);
    auto open = request_stream(1, true, {4});
    open.established = true;
    EXPECT_EQ(run(with_requests({open})).at(kId), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, RejectionsShouldCarryAFin) {
    constexpr auto kId = "D23-6-4-2-3-SHOULD-157";
    auto with_fin = reply_evidence(kReservedScenario, true, request_error(0x10));
    with_fin.probe->reply_fin = true;
    EXPECT_EQ(run(with_fin).at(kId), OutcomeState::Pass);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, request_error(0x10))).at(kId), OutcomeState::Fail);
    EXPECT_EQ(run(reply_evidence(kReservedScenario, true, std::nullopt)).at(kId), OutcomeState::NotRun);
}
TEST(Draft23Evaluators, ImplementationOptionShouldBeSentAndBeText) {
    constexpr auto kSent = "D23-9-1-5-SHOULD-281";
    constexpr auto kText = "D23-9-1-5-SHOULD-280";
    EXPECT_EQ(run(Draft23Evidence{}).at(kSent), OutcomeState::NotRun);
    EXPECT_EQ(run(native({option(1, text("/moq"))})).at(kSent), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(7, text("moqxr/1.0"))})).at(kSent), OutcomeState::Pass);
    EXPECT_EQ(run(native({option(7, text("moqxr/1.0"))})).at(kText), OutcomeState::Pass);
    EXPECT_EQ(run(native({option(7, {std::byte{0xff}, std::byte{0xfe}})})).at(kText), OutcomeState::Fail);
    EXPECT_EQ(run(native({option(1, text("/moq"))})).at(kText), OutcomeState::NotApplicable);
}
TEST(Draft23Evaluators, SharedEvaluatorsJudgeTheirSecondRowToo) {
    EXPECT_EQ(run(with_tracks({published("media", "a", 1), published("media", "b", 1)})).at("D23-3-1-3-SHOULD-NOT-052"), OutcomeState::Fail);
    EXPECT_EQ(run(with_tracks({published("media", "a", 1), published("media", "b", 2)})).at("D23-3-1-3-SHOULD-NOT-052"), OutcomeState::Pass);
    EXPECT_EQ(run(with_streams({stream(6, 0x30, 7, 0, 0), stream(10, 0x30, 7, 0, 0)})).at("D23-11-3-SHOULD-NOT-443"), OutcomeState::Fail);
    EXPECT_EQ(run(with_streams({stream(6, 0x30, 7, 0, 0), stream(10, 0x30, 7, 1, 0)})).at("D23-11-3-SHOULD-NOT-443"), OutcomeState::Pass);
}

TEST(Draft23Evaluators, PublishRowsNeedThePublishScenarioBecauseTheSmokesNeverSeeAPublish) {
    const auto judged = [](std::string scenario, std::vector<session::Draft23PublishedTrack> tracks) {
        auto context = with_tracks(std::move(tracks));
        context.scenario = std::move(scenario);
        return run(context);
    };
    const auto tracks = std::vector{published("media", "catalog", 0), published("media", "soun_2", 2), published("media", "vide_1", 1)};
    for (const auto* row : {"D23-2-4-1-MUST-028", "D23-3-1-3-MUST-NOT-050", "D23-3-1-3-SHOULD-NOT-052"}) {
        EXPECT_EQ(judged("d23-publish-smoke", tracks).at(row), OutcomeState::Pass) << row;
        EXPECT_EQ(judged("d23-publish-smoke", {}).at(row), OutcomeState::NotRun) << row;
    }
    EXPECT_EQ(judged("d23-publish-smoke", {published("media", "a", 1), published("media", "b", 1)}).at("D23-3-1-3-MUST-NOT-050"),
              OutcomeState::Fail);
    // A row that is not bound to the PUBLISH scenario stays unjudged there.
    EXPECT_NE(judged("d23-publish-smoke", tracks).at("D23-9-1-MUST-NOT-265"), OutcomeState::Pass);
    std::size_t bound = 0;
    for (const auto& binding : requirements::draft23_executable_bindings()) if (binding.scenario_id == "d23-publish-smoke") ++bound;
    EXPECT_EQ(bound, 3u);
}

TEST(Draft23Evaluators, ARowIsOnlyJudgedInTheScenariosItIsBoundTo) {
    constexpr auto kSmokeRow = "D23-9-1-MUST-NOT-265";  // bound to the two smoke scenarios
    auto context = native({option(7, text("a")), option(7, text("b"))});
    context.scenario = "d23-publisher-smoke";
    EXPECT_EQ(run(context).at(kSmokeRow), OutcomeState::Fail);
    context.scenario = "d23-direct-subscribe-smoke";
    EXPECT_EQ(run(context).at(kSmokeRow), OutcomeState::Fail);
    context.scenario = "d23-probe-unknown-control-message";
    EXPECT_EQ(run(context).at(kSmokeRow), OutcomeState::NotRun);
    context.scenario.clear();  // unspecified: judge on the evidence, as the unit tests above do
    EXPECT_EQ(run(context).at(kSmokeRow), OutcomeState::Fail);
}
TEST(Draft23Evaluators, AProbeRowIsJudgedInItsOwnScenarioAndNoOther) {
    const auto& probe = *session::find_draft23_probe("d23-probe-length-mismatch");
    auto context = evidence_for(probe, true, session::Draft23ProbeClose{true, probe.expected_code});
    context.scenario = std::string(probe.scenario_id);
    EXPECT_EQ(run(context).at("D23-9-MUST-258"), OutcomeState::Pass);
    context.scenario = "d23-publisher-smoke";
    EXPECT_EQ(run(context).at("D23-9-MUST-258"), OutcomeState::NotRun);
}
}  // namespace
