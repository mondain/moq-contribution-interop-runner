// The moq-lite-06 announce and session scenarios (L1d Task 5): every evaluator against the conforming scripted
// publisher and against publishers that violate exactly one rule, plus the NotRun conditions of the catalog rows
// (L06-7-4-MUST-139, L06-7-5-SHOULD-143, L06-7-5-MUST-NOT-141, L06-7-7-MUST-NOT-152, L06-4-3-MUST-025).

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/varint.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_announce_hop_list_excludes_own;
using moq::interop::scenarios::evaluate_l06_announce_ok_hop_assigned;
using moq::interop::scenarios::evaluate_l06_announce_ok_then_starts;
using moq::interop::scenarios::evaluate_l06_announce_retired_id_unused;
using moq::interop::scenarios::evaluate_l06_session_peer_closes_send;
using moq::interop::scenarios::judgeable;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteStep;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
namespace scen = moq::interop::scenarios;
namespace lite06 = moq::interop::scenarios::lite06;
namespace session = moq::interop::session;
namespace transport = moq::interop::transport;
namespace wire = moq::interop::wire;

using Verdict = std::optional<bool>;
using Evaluator = Verdict (*)(const LiteTranscript&);
// (139 ok-then-starts, 143 ok-hop-assigned, 141 hop-list-excludes-own)
using PrefixVerdicts = std::tuple<Verdict, Verdict, Verdict>;

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 15000ms;
const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";
constexpr std::uint64_t kHop = 7;

const Verdict kPass{true};
const Verdict kFail{false};
const Verdict kNotRun{};

// --- encoding helpers ---------------------------------------------------------------------------------------------

Bytes varint(std::uint64_t value) {
    return encode_with([&](wire::ByteWriter& out) { return l06::write_varint(value, out); });
}
Bytes framed(const Bytes& body) {
    return encode_with([&](wire::ByteWriter& out) { return l06::write_framed_message(body, out); });
}
Bytes ok(std::uint64_t hop, std::uint64_t active) { return announce_ok({hop, active}); }
Bytes start(const std::string& suffix, std::vector<std::uint64_t> hops = {}) {
    l06::AnnounceStart message;
    message.suffix = suffix;
    message.route.hop_ids = std::move(hops);
    return announce_message(message);
}
Bytes end(std::uint64_t id) { return announce_message(l06::AnnounceEnd{id}); }
Bytes update(std::uint64_t id, std::vector<std::uint64_t> hops = {}) {
    l06::AnnounceUpdate message;
    message.announce_id = id;
    message.route.hop_ids = std::move(hops);
    return announce_message(message);
}
// An ANNOUNCE_START built raw: the Hop Count is written as given, so it can disagree with the entries, and
// non-zero entries may repeat (the encoder refuses both).
Bytes raw_start(const std::string& suffix, std::uint64_t hop_count, const std::vector<std::uint64_t>& hops) {
    Bytes body = join({varint(suffix.size()), bytes_of(suffix), varint(hop_count)});
    for (const auto hop : hops) body = join({body, varint(hop)});
    body = join({body, varint(0), varint(0)});
    return join({varint(l06::kAnnounceTypeStart), framed(body)});
}
// An announce message with an undefined Type (decision (a): skipped by its Message Length, inconclusive).
Bytes unknown_type(std::uint64_t type = 4) { return join({varint(type), framed(bytes_of("xy"))}); }

bool covered(const std::string& prefix) { return kBroadcast.starts_with(prefix); }
std::string suffix_for(const std::string& prefix) { return kBroadcast.substr(prefix.size()); }

// --- publishers ---------------------------------------------------------------------------------------------------

ConformingLitePublisherConfig base_config() {
    ConformingLitePublisherConfig config;
    config.broadcast = kBroadcast;
    config.track = kTrack;
    config.hop_id = kHop;
    return config;
}

// Replaces the publisher's announce answer: `answer(peer, stream, prefix)`.
using Answer = std::function<void(ScriptedLitePeer&, transport::StreamId, const std::string&)>;
ConformingLitePublisherConfig answering(Answer answer, ConformingLitePublisherConfig config = base_config()) {
    config.hooks.on_request = [answer = std::move(answer)](ConformingLitePublisher&, ScriptedLitePeer& peer,
                                                           const LiteRunnerRequest& request) {
        const auto* announce = std::get_if<l06::AnnounceRequest>(&request.message);
        if (!announce) return false;
        answer(peer, request.stream, announce->prefix);
        return true;
    };
    return config;
}

// The conforming answer with the covered streams' START replaced by `covered_tail` (bytes after ANNOUNCE_OK).
ConformingLitePublisherConfig covered_answer(std::uint64_t active, std::function<Bytes(const std::string&)> tail,
                                             std::uint64_t hop = kHop) {
    return answering([=](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        if (!covered(prefix)) {
            peer.data(stream, ok(hop, 0));
            return;
        }
        peer.data(stream, join({ok(hop, active), tail(suffix_for(prefix))}));
    });
}

LiteTranscript run(LiteProbeDefinition definition, ConformingLitePublisherConfig config = base_config(),
                   const std::function<void(ScriptedLitePeer&)>& tweak = {}) {
    ConformingLitePublisher publisher(std::move(config));
    ScriptedLitePeer peer(publisher.reaction());
    if (tweak) tweak(peer);
    ManualLiteClock clock;
    return run_lite_probe(peer, std::move(definition), clock, kTick);
}

LiteProbeDefinition prefix_probe() { return scen::l06_announce_prefix_probe(kDeadline, kBroadcast); }
LiteProbeDefinition lifecycle_probe() { return scen::l06_announce_lifecycle_probe(kDeadline, kBroadcast); }
LiteProbeDefinition stream_close_probe() {
    return scen::l06_session_stream_close_probe(kDeadline, kBroadcast, kTrack);
}

// The l06-announce-prefix probe against `config`.
LiteTranscript run(ConformingLitePublisherConfig config) { return run(prefix_probe(), std::move(config)); }

PrefixVerdicts judge_prefix(const LiteTranscript& t) {
    return {evaluate_l06_announce_ok_then_starts(t), evaluate_l06_announce_ok_hop_assigned(t),
            evaluate_l06_announce_hop_list_excludes_own(t)};
}

std::vector<Evaluator> all_evaluators() {
    return {evaluate_l06_announce_ok_then_starts, evaluate_l06_announce_ok_hop_assigned,
            evaluate_l06_announce_hop_list_excludes_own, evaluate_l06_announce_retired_id_unused,
            evaluate_l06_session_peer_closes_send};
}

std::vector<std::function<LiteProbeDefinition()>> every_probe() {
    return {prefix_probe, lifecycle_probe, stream_close_probe};
}

// A lifecycle script: the announce answer (ANNOUNCE_OK{7,1} + the covering START) then `later[n]` written on the
// announce stream n polls after the request was answered (each entry is its own StreamDataEvent).
ConformingLitePublisherConfig lifecycle(std::map<std::size_t, Bytes> later) {
    struct State {
        std::optional<transport::StreamId> stream;
        std::size_t polls{0};
    };
    auto state = std::make_shared<State>();
    auto config = answering([state](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        state->stream = stream;
        peer.data(stream, join({ok(kHop, 1), start(suffix_for(prefix))}));
    });
    config.hooks.on_poll = [state, later = std::move(later)](ConformingLitePublisher&, ScriptedLitePeer& peer) {
        if (!state->stream) return;
        const auto found = later.find(++state->polls);
        if (found != later.end()) peer.data(*state->stream, found->second);
    };
    return config;
}

// How the publisher reacts when the runner closes the send direction of a bidirectional stream (row 025).
enum class OnRunnerFin { Fin, Reset, CloseSession, Nothing, FinAnnounceOnly };
ConformingLitePublisherConfig on_runner_fin(OnRunnerFin how, ConformingLitePublisherConfig config = base_config()) {
    auto handled = std::make_shared<std::set<transport::StreamId>>();
    config.hooks.on_poll = [how, handled](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        for (const auto& [id, stream] : peer.runner_streams()) {
            if ((id & 2u) != 0u || !stream.fin || !handled->insert(id).second) continue;
            const bool announce = !stream.bytes.empty() && stream.bytes.front() == std::byte{0x1};
            switch (how) {
                case OnRunnerFin::Fin: peer.fin(id); break;
                case OnRunnerFin::Reset: peer.peer_reset(id, 0x0); break;
                case OnRunnerFin::CloseSession: publisher.close(peer, 0x0); return;
                case OnRunnerFin::Nothing: break;
                case OnRunnerFin::FinAnnounceOnly:
                    if (announce) peer.fin(id);
                    break;
            }
        }
    };
    return config;
}

std::vector<const session::LiteDecodeIssue*> issues_on(const LiteTranscript& t, std::string_view label) {
    const auto* step = lite06::step_labelled(t, label);
    if (!step || !step->stream_id) return {};
    const auto* record = lite06::find_stream(t, *step->stream_id);
    return record ? session::peer_protocol_issues(*record) : std::vector<const session::LiteDecodeIssue*>{};
}

// --- helpers and probe structure ----------------------------------------------------------------------------------

TEST(Lite06AnnounceCommon, PathHelpers) {
    EXPECT_EQ(scen::l06_path_segments("demo/live"), (std::vector<std::string>{"demo", "live"}));
    EXPECT_EQ(scen::l06_path_segments("/demo//live/"), (std::vector<std::string>{"demo", "live"}));
    EXPECT_TRUE(scen::l06_path_segments("").empty());
    EXPECT_EQ(scen::l06_broadcast_prefix("demo/live"), "demo");
    EXPECT_EQ(scen::l06_broadcast_prefix("live"), "live");
    EXPECT_EQ(scen::l06_disjoint_prefix("demo/live"), "l1d-disjoint");
    EXPECT_EQ(scen::l06_disjoint_prefix("l1d-disjoint/x"), "l1d-disjoint-x");
    // Coverage is per segment: the suffix is joined to the prefix with or without a leading '/'.
    EXPECT_TRUE(scen::l06_route_covers("", "demo/live", "demo/live"));
    EXPECT_TRUE(scen::l06_route_covers("demo", "/live", "demo/live"));
    EXPECT_TRUE(scen::l06_route_covers("demo", "live", "demo/live"));
    EXPECT_TRUE(scen::l06_route_covers("demo", "", "demo/live"));  // a route above the request prefix
    EXPECT_TRUE(scen::l06_route_covers("", "demo", "demo/live"));
    EXPECT_FALSE(scen::l06_route_covers("demo", "/liv", "demo/live"));  // never half a segment
    EXPECT_FALSE(scen::l06_route_covers("demo", "/live/hd", "demo/live"));
    EXPECT_FALSE(scen::l06_route_covers("l1d-disjoint", "", "demo/live"));
}

TEST(Lite06AnnounceCommon, BuildersRejectAShortDeadlineAndAMissingFixture) {
    EXPECT_THROW(scen::l06_announce_prefix_probe(3000ms, kBroadcast), std::invalid_argument);
    EXPECT_THROW(scen::l06_announce_prefix_probe(kDeadline, ""), std::invalid_argument);
    EXPECT_THROW(scen::l06_announce_prefix_probe(kDeadline, "/"), std::invalid_argument);
    EXPECT_THROW(scen::l06_announce_lifecycle_probe(6000ms, kBroadcast), std::invalid_argument);
    EXPECT_THROW(scen::l06_announce_lifecycle_probe(kDeadline, ""), std::invalid_argument);
    // The stream-close probe needs the answer wait and the close allowance within its deadline.
    EXPECT_THROW(scen::l06_session_stream_close_probe(6000ms, kBroadcast, kTrack), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_session_stream_close_probe(6001ms, kBroadcast, kTrack));
    EXPECT_THROW(scen::l06_session_stream_close_probe(kDeadline, kBroadcast, ""), std::invalid_argument);
    EXPECT_THROW(scen::l06_session_stream_close_probe(kDeadline, "", kTrack), std::invalid_argument);
    EXPECT_THROW(scen::l06_session_stream_close_probe(kDeadline, kBroadcast, kTrack, 3000ms, 0ms),
                 std::invalid_argument);
}

TEST(Lite06AnnounceCommon, ProbeStimuli) {
    using Kind = LiteStep::Kind;
    const auto prefix = prefix_probe();
    EXPECT_EQ(prefix.id, scen::kL06AnnouncePrefix);
    EXPECT_TRUE(prefix.requires_track);
    EXPECT_EQ(prefix.broadcast_path, kBroadcast);
    EXPECT_EQ(prefix.observation_window, std::optional<std::chrono::milliseconds>{0ms});
    ASSERT_EQ(prefix.steps.size(), 4u);
    const std::vector<std::pair<std::string_view, std::string>> requests{
        {scen::kL06AnnounceEmptyLabel, ""},
        {scen::kL06AnnounceBroadcastLabel, "demo"},
        {scen::kL06AnnounceDisjointLabel, "l1d-disjoint"}};
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& step = prefix.steps[i];
        EXPECT_EQ(step.kind, Kind::OpenBidiAndSend);
        EXPECT_EQ(step.label, requests[i].first);
        EXPECT_EQ(step.bytes, scen::l06_announce_request_bytes(requests[i].second));
        EXPECT_FALSE(step.fin);
        EXPECT_FALSE(step.gate);  // all open at once
        EXPECT_EQ(step.delay, 0ms);
    }
    EXPECT_EQ(prefix.steps[3].kind, Kind::Wait);
    EXPECT_EQ(prefix.steps[3].label, lite06::kAllowanceLabel);
    EXPECT_FALSE(prefix.steps[3].gate);
    EXPECT_EQ(prefix.steps[3].delay, scen::kLiteResponseAllowance);

    const auto life = lifecycle_probe();
    EXPECT_EQ(life.id, scen::kL06AnnounceLifecycle);
    EXPECT_TRUE(life.requires_track);
    ASSERT_EQ(life.steps.size(), 2u);
    EXPECT_EQ(life.steps[0].label, scen::kL06AnnounceRequestLabel);
    EXPECT_EQ(life.steps[0].bytes, scen::l06_announce_request_bytes("demo"));
    EXPECT_FALSE(life.steps[1].gate);
    EXPECT_EQ(life.steps[1].delay, scen::kLiteObservationWindow);

    const auto close = stream_close_probe();
    EXPECT_EQ(close.id, scen::kL06SessionStreamClose);
    EXPECT_TRUE(close.requires_track);
    EXPECT_EQ(close.track_name, kTrack);
    ASSERT_EQ(close.steps.size(), 6u);
    EXPECT_EQ(close.steps[0].bytes, scen::l06_announce_request_bytes(""));
    EXPECT_EQ(close.steps[1].bytes, scen::l06_stream_close_subscribe_bytes(kBroadcast, kTrack));
    EXPECT_EQ(close.steps[2].label, scen::kL06AnswersLabel);
    EXPECT_TRUE(close.steps[2].gate);
    EXPECT_EQ(close.steps[2].gate_deadline, scen::kLiteResponseAllowance);
    EXPECT_EQ(close.steps[3].kind, Kind::FinStream);
    EXPECT_EQ(close.steps[3].stream_ref, std::optional<std::size_t>{0});
    EXPECT_EQ(close.steps[4].kind, Kind::FinStream);
    EXPECT_EQ(close.steps[4].stream_ref, std::optional<std::size_t>{1});
    EXPECT_FALSE(close.steps[5].gate);
    EXPECT_EQ(close.steps[5].label, lite06::kAllowanceLabel);
    EXPECT_EQ(close.steps[5].delay, scen::kLiteCloseAllowance);
}

TEST(Lite06AnnounceCommon, TrackFixtureIsCopiedToTheTranscript) {
    const auto t = run(stream_close_probe(), on_runner_fin(OnRunnerFin::Fin));
    EXPECT_EQ(t.broadcast_path, kBroadcast);
    EXPECT_EQ(t.track_name, kTrack);
    EXPECT_TRUE(LiteTranscript{}.broadcast_path.empty());
}

// --- l06-announce-prefix: rows 139, 143, 141 ----------------------------------------------------------------------

TEST(Lite06AnnouncePrefix, ConformingPublisherPassesAllThree) {
    const auto t = run(prefix_probe());
    EXPECT_TRUE(judgeable(t));
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    EXPECT_FALSE(t.timed_out);
    // The probe lasts its whole allowance.
    EXPECT_GE(t.ended_ns - t.established_ns, std::uint64_t{3'000'000'000});
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, OkTwiceFailsOnlyOkThenStarts) {
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        if (!covered(prefix)) {
            peer.data(stream, join({ok(kHop, 0), ok(kHop, 0)}));
            return;
        }
        peer.data(stream, join({ok(kHop, 1), ok(kHop, 1), start(suffix_for(prefix))}));
    }));
    EXPECT_FALSE(issues_on(t, scen::kL06AnnounceEmptyLabel).empty());  // the second OK did not decode
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kNotRun));
}

TEST(Lite06AnnouncePrefix, StartBeforeOkFailsOnlyOkThenStarts) {
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        if (!covered(prefix)) {
            peer.data(stream, ok(kHop, 0));
            return;
        }
        peer.data(stream, join({start(suffix_for(prefix)), ok(kHop, 1)}));
    }));
    EXPECT_FALSE(issues_on(t, scen::kL06AnnounceBroadcastLabel).empty());
    // 143 is judged on the disjoint stream's ANNOUNCE_OK; no START decodes, so 141 has nothing to judge.
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kNotRun));
}

TEST(Lite06AnnouncePrefix, FewerStartsThanTheActiveCountFailsOnlyOkThenStarts) {
    const auto t = run(covered_answer(2, [](const std::string& suffix) { return start(suffix); }));
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, AnUpdateInsideTheInitialSetFails) {
    const auto t = run(covered_answer(2, [](const std::string& suffix) {
        return join({start(suffix), update(0), start(suffix + "/x")});
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, AnEndOfAnInitialRouteMakesTheActiveCountInconclusive) {
    const auto t = run(covered_answer(2, [](const std::string& suffix) {
        return join({start(suffix), end(0), start(suffix)});
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kNotRun, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, NoCoveringStartFailsOnlyOkThenStarts) {
    // Active Count 0 is valid, but the covered requests must see a covering route within the allowance.
    const auto t = run(covered_answer(0, [](const std::string&) { return Bytes{}; }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kNotRun));
}

TEST(Lite06AnnouncePrefix, ANonCoveringStartDoesNotCount) {
    const auto t = run(covered_answer(1, [](const std::string& suffix) { return start(suffix + "/other"); }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, ALiveStartBeyondTheInitialSetCovers) {
    const auto t = run(covered_answer(0, [](const std::string& suffix) { return start(suffix); }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, ARouteAboveTheRequestCoversWithAnEmptySuffix) {
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        if (prefix.empty()) {
            peer.data(stream, join({ok(kHop, 1), start("demo")}));
        } else if (covered(prefix)) {
            peer.data(stream, join({ok(kHop, 1), start("")}));
        } else {
            peer.data(stream, ok(kHop, 0));
        }
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, HopCountMismatchFailsOnlyOkThenStarts) {
    const auto t = run(covered_answer(1, [](const std::string& suffix) { return raw_start(suffix, 2, {5}); }));
    const auto issues = issues_on(t, scen::kL06AnnounceEmptyLabel);
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues.front()->code, session::kIssueProtocolViolation);  // the decode error is recorded
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kNotRun));
}

TEST(Lite06AnnouncePrefix, DuplicateNonZeroHopIdsFailOnlyOkThenStarts) {
    const auto t = run(covered_answer(1, [](const std::string& suffix) { return raw_start(suffix, 2, {5, 5}); }));
    EXPECT_FALSE(issues_on(t, scen::kL06AnnounceBroadcastLabel).empty());
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kNotRun));
}

TEST(Lite06AnnouncePrefix, DuplicateZeroHopIdsAreLegal) {
    const auto t = run(covered_answer(1, [](const std::string& suffix) { return raw_start(suffix, 2, {0, 0}); }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, OwnHopIdAsTheLastEntryFailsOnlyHopListExcludesOwn) {
    const auto t = run(covered_answer(1, [](const std::string& suffix) { return start(suffix, {kHop}); }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kFail));
}

TEST(Lite06AnnouncePrefix, OwnHopIdAsTheLastEntryOfAnUpdateFails) {
    const auto t = run(covered_answer(1, [](const std::string& suffix) {
        return join({start(suffix), update(0, {3, kHop})});
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kFail));
}

TEST(Lite06AnnouncePrefix, OwnHopIdElsewhereInTheListIsNotThisRow) {
    // Draft 7.5 forbids only the LAST entry; a changed hop list in an UPDATE is allowed (L06-7-8-MAY-153).
    const auto t = run(covered_answer(1, [](const std::string& suffix) {
        return join({start(suffix, {kHop, 3}), update(0, {4})});
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, HopIdZeroFailsOnlyHopAssignedAndLeavesTheHopListNotRun) {
    auto config = base_config();
    config.hop_id = 0;
    const auto t = run(prefix_probe(), config);
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kFail, kNotRun));
    // A zero OK Hop ID with a zero last entry is still not judged (repeated 0 entries are legal).
    const auto zeros =
        run(covered_answer(1, [](const std::string& suffix) { return start(suffix, {0}); }, 0));
    EXPECT_EQ(judge_prefix(zeros), PrefixVerdicts(kPass, kFail, kNotRun));
}

TEST(Lite06AnnouncePrefix, UnknownAnnounceTypeIsInconclusive) {
    // In the same event as the START: everything after the OK is inconclusive for 139 and 141.
    const auto t = run(covered_answer(1, [](const std::string& suffix) {
        return join({start(suffix), unknown_type()});
    }));
    ASSERT_TRUE(judgeable(t));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kNotRun, kPass, kNotRun));
    // Before the covering START: still inconclusive, never a Fail for the missing route.
    const auto first = run(covered_answer(1, [](const std::string& suffix) {
        return join({unknown_type(5), start(suffix)});
    }));
    EXPECT_EQ(judge_prefix(first), PrefixVerdicts(kNotRun, kPass, kNotRun));
}

TEST(Lite06AnnouncePrefix, AFailBeforeAnUnknownTypeStillCounts) {
    // The defects arrive in earlier events than the unknown Type: they are judged.
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        if (!covered(prefix)) {
            peer.data(stream, ok(kHop, 0));
            return;
        }
        peer.data(stream, join({ok(kHop, 2), start(suffix_for(prefix), {kHop}), update(0)}));
        peer.data(stream, unknown_type());
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kFail));
}

TEST(Lite06AnnouncePrefix, PublisherThatNeverAnswersFailsOnlyOkThenStarts) {
    auto config = base_config();
    config.defect = LiteDefect::SilentOnAnnounce;
    const auto t = run(prefix_probe(), config);
    EXPECT_FALSE(t.timed_out);  // ended by the allowance: the Fail is time-bounded
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kNotRun, kNotRun));
}

TEST(Lite06AnnouncePrefix, ASessionCloseInsteadOfTheAnswerFails) {
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId, const std::string&) {
        peer.close_session(0x0);
    }));
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kNotRun, kNotRun));
}

TEST(Lite06AnnouncePrefix, AResetInsteadOfTheAnswerFails) {
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId stream, const std::string&) {
        peer.peer_reset(stream, 0x0);
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kNotRun, kNotRun));
}

TEST(Lite06AnnouncePrefix, ARequestRefusedByTheTransportFails) {
    // The publisher reset the request stream before the runner's write went through.
    const auto t = run(prefix_probe(), base_config(), [](ScriptedLitePeer& peer) {
        peer.forced_status[1] = transport::TransportStatus::PeerReset;
    });
    const auto* step = lite06::step_labelled(t, scen::kL06AnnounceEmptyLabel);
    ASSERT_NE(step, nullptr);
    ASSERT_TRUE(step->refused.has_value());
    EXPECT_EQ(std::get<0>(judge_prefix(t)), kFail);
}

TEST(Lite06AnnouncePrefix, EndingTheStreamAfterACompleteAnswerPasses) {
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        if (covered(prefix)) {
            peer.data(stream, join({ok(kHop, 1), start(suffix_for(prefix))}), true);
        } else {
            peer.data(stream, ok(kHop, 0), true);
        }
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kPass, kPass, kPass));
}

TEST(Lite06AnnouncePrefix, EndingTheStreamWithoutAnAnswerFails) {
    const auto t = run(answering([](ScriptedLitePeer& peer, transport::StreamId stream, const std::string& prefix) {
        if (covered(prefix)) {
            peer.fin(stream);
        } else {
            peer.data(stream, ok(kHop, 0));
        }
    }));
    EXPECT_EQ(judge_prefix(t), PrefixVerdicts(kFail, kPass, kNotRun));
}

// --- l06-announce-lifecycle: row 152 -------------------------------------------------------------------------------

TEST(Lite06AnnounceLifecycle, NoRetractionIsNotRun) {
    const auto t = run(lifecycle_probe());
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    EXPECT_GE(t.ended_ns - t.established_ns, std::uint64_t{6'000'000'000});
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(t), kNotRun);
}

TEST(Lite06AnnounceLifecycle, ARetractionAndAFreshIdPass) {
    const auto t = run(lifecycle_probe(), lifecycle({{5, end(0)}, {10, start("/live")}, {15, update(1, {3})}}));
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(t), kPass);
}

TEST(Lite06AnnounceLifecycle, UpdatingARetiredIdFails) {
    const auto t = run(lifecycle_probe(), lifecycle({{5, end(0)}, {10, start("/live")}, {15, update(0)}}));
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(t), kFail);
}

TEST(Lite06AnnounceLifecycle, EndingARetiredIdAgainFails) {
    const auto t = run(lifecycle_probe(), lifecycle({{5, end(0)}, {10, end(0)}}));
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(t), kFail);
}

TEST(Lite06AnnounceLifecycle, ANeverAssignedIdIsInformationOnly) {
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(run(lifecycle_probe(), lifecycle({{5, end(5)}}))), kNotRun);
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(
                  run(lifecycle_probe(), lifecycle({{5, end(5)}, {6, end(0)}, {7, update(9)}}))),
              kPass);
}

TEST(Lite06AnnounceLifecycle, UnknownAnnounceTypeIsInconclusiveFromThatPointOn) {
    // A reuse after the unknown Type is not judged.
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(
                  run(lifecycle_probe(), lifecycle({{5, end(0)}, {6, unknown_type()}, {7, update(0)}}))),
              kNotRun);
    // A retraction followed only by the unknown Type cannot pass either.
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(
                  run(lifecycle_probe(), lifecycle({{5, end(0)}, {6, unknown_type()}}))),
              kNotRun);
    // A reuse before it is a Fail.
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(
                  run(lifecycle_probe(), lifecycle({{5, end(0)}, {6, update(0)}, {7, unknown_type()}}))),
              kFail);
}

TEST(Lite06AnnounceLifecycle, PublisherThatNeverAnswersIsNotRun) {
    auto config = base_config();
    config.defect = LiteDefect::SilentOnAnnounce;
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(run(lifecycle_probe(), config)), kNotRun);
}

TEST(Lite06AnnounceLifecycle, ASessionEndingWithTheBroadcastIsNotRun) {
    // The moq CLI ends its session with the broadcast: no verdict, even after a retraction.
    auto config = lifecycle({{5, end(0)}});
    config.hooks.on_poll = [inner = config.hooks.on_poll, polls = std::make_shared<std::size_t>(0)](
                               ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        inner(publisher, peer);
        if (++*polls == 20) publisher.close(peer, 0x0);
    };
    const auto t = run(lifecycle_probe(), config);
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(t), kNotRun);
}

TEST(Lite06AnnounceLifecycle, ADecodeFailureStopsTheJudgement) {
    // Decoding stops at the malformed START: later references cannot be seen, so no Pass.
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(
                  run(lifecycle_probe(), lifecycle({{5, end(0)}, {6, raw_start("/live", 2, {5})}}))),
              kNotRun);
}

// --- l06-session-stream-close: row 025 ------------------------------------------------------------------------------

TEST(Lite06SessionStreamClose, PublisherClosingItsSendDirectionPasses) {
    const auto t = run(stream_close_probe(), on_runner_fin(OnRunnerFin::Fin));
    EXPECT_TRUE(judgeable(t));
    const auto* answers = lite06::step_labelled(t, scen::kL06AnswersLabel);
    ASSERT_NE(answers, nullptr);
    EXPECT_FALSE(answers->gate_expired);
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(t), kPass);
}

TEST(Lite06SessionStreamClose, AResetAlsoClosesTheSendDirection) {
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(run(stream_close_probe(), on_runner_fin(OnRunnerFin::Reset))),
              kPass);
}

TEST(Lite06SessionStreamClose, NotClosingWithinTheAllowanceFails) {
    const auto t = run(stream_close_probe(), on_runner_fin(OnRunnerFin::Nothing));
    EXPECT_FALSE(t.timed_out);  // ended by the allowance: the Fail is time-bounded
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(t), kFail);
}

TEST(Lite06SessionStreamClose, ClosingOnlyOneOfTheStreamsFails) {
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(
                  run(stream_close_probe(), on_runner_fin(OnRunnerFin::FinAnnounceOnly))),
              kFail);
}

TEST(Lite06SessionStreamClose, ASessionCloseIsNoSubstitute) {
    const auto t = run(stream_close_probe(), on_runner_fin(OnRunnerFin::CloseSession));
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(t), kFail);
}

TEST(Lite06SessionStreamClose, PublisherThatNeverAnswersIsNotRun) {
    auto config = on_runner_fin(OnRunnerFin::Fin);
    config.defect = LiteDefect::SilentOnAnnounce;
    const auto t = run(stream_close_probe(), config);
    const auto* answers = lite06::step_labelled(t, scen::kL06AnswersLabel);
    ASSERT_NE(answers, nullptr);
    EXPECT_TRUE(answers->gate_expired);
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(t), kNotRun);
}

TEST(Lite06SessionStreamClose, AStreamThePublisherEndedFirstIsNotJudged) {
    // The publisher FINs its announce side with the answer, before the runner's FIN: that stream cannot show the
    // rule; the subscribe stream still can.
    auto config = on_runner_fin(OnRunnerFin::Fin, answering([](ScriptedLitePeer& peer, transport::StreamId stream,
                                                                const std::string& prefix) {
        peer.data(stream, join({ok(kHop, 1), start(suffix_for(prefix))}), true);
    }));
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(run(stream_close_probe(), config)), kPass);
    // ...and with the subscribe refused up front too, nothing is left to judge.
    config.track = "other";
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(run(stream_close_probe(), config)), kNotRun);
}

// --- attribution and scope ------------------------------------------------------------------------------------------

TEST(Lite06AnnounceAttribution, EachEvaluatorJudgesOnlyItsOwnScenario) {
    const auto prefix = run(prefix_probe());
    const auto life = run(lifecycle_probe(), lifecycle({{5, end(0)}}));
    const auto close = run(stream_close_probe(), on_runner_fin(OnRunnerFin::Fin));
    EXPECT_EQ(judge_prefix(prefix), PrefixVerdicts(kPass, kPass, kPass));
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(prefix), kNotRun);
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(prefix), kNotRun);
    EXPECT_EQ(judge_prefix(life), PrefixVerdicts(kNotRun, kNotRun, kNotRun));
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(life), kPass);
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(life), kNotRun);
    EXPECT_EQ(judge_prefix(close), PrefixVerdicts(kNotRun, kNotRun, kNotRun));
    EXPECT_EQ(evaluate_l06_announce_retired_id_unused(close), kNotRun);
    EXPECT_EQ(evaluate_l06_session_peer_closes_send(close), kPass);
    // The setup evaluators (and the shared code-space evaluator) judge none of these transcripts.
    const std::vector<Evaluator> setup{scen::evaluate_l06_setup_stream_single_setup,
                                       scen::evaluate_l06_setup_parameters_unique,
                                       scen::evaluate_l06_setup_unknown_parameter_ignored,
                                       scen::evaluate_l06_setup_duplicate_parameter_close,
                                       scen::evaluate_l06_setup_duplicate_stream_close,
                                       scen::evaluate_l06_setup_server_path_close,
                                       scen::evaluate_l06_setup_server_role_close,
                                       scen::evaluate_l06_errors_code_space};
    for (const auto* t : {&prefix, &life, &close})
        for (const auto evaluator : setup) EXPECT_EQ(evaluator(*t), kNotRun) << t->scenario_id;
    // And these evaluators judge none of the setup probes.
    for (const auto& definition :
         {scen::l06_setup_stream_probe(kDeadline), scen::l06_setup_unknown_parameter_probe(kDeadline),
          scen::l06_setup_duplicate_parameter_probe(kDeadline), scen::l06_setup_duplicate_stream_probe(kDeadline),
          scen::l06_setup_server_path_probe(kDeadline), scen::l06_setup_server_role_probe(kDeadline)}) {
        const auto t = run(definition);
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06AnnounceAttribution, OneDefectFailsExactlyItsEvaluators) {
    struct Case {
        std::string name;
        ConformingLitePublisherConfig config;
        PrefixVerdicts expected;
    };
    auto hop_zero = base_config();
    hop_zero.hop_id = 0;
    const std::vector<Case> cases{
        {"conforming", base_config(), {kPass, kPass, kPass}},
        {"active count", covered_answer(3, [](const std::string& s) { return start(s); }), {kFail, kPass, kPass}},
        {"hop count mismatch", covered_answer(1, [](const std::string& s) { return raw_start(s, 1, {5, 6}); }),
         {kFail, kPass, kNotRun}},
        {"duplicate hop", covered_answer(1, [](const std::string& s) { return raw_start(s, 3, {9, 4, 9}); }),
         {kFail, kPass, kNotRun}},
        {"own hop last", covered_answer(1, [](const std::string& s) { return start(s, {2, kHop}); }),
         {kPass, kPass, kFail}},
        {"hop id zero", hop_zero, {kPass, kFail, kNotRun}},
    };
    for (const auto& c : cases) {
        const auto t = run(prefix_probe(), c.config);
        EXPECT_EQ(judge_prefix(t), c.expected) << c.name;
        // The other scenarios' evaluators never judge this transcript.
        EXPECT_EQ(evaluate_l06_announce_retired_id_unused(t), kNotRun) << c.name;
        EXPECT_EQ(evaluate_l06_session_peer_closes_send(t), kNotRun) << c.name;
    }
}

// --- NotRun conditions shared by all three probes -------------------------------------------------------------------

// Publishers under which each probe would otherwise produce a verdict.
ConformingLitePublisherConfig verdict_publisher(const std::string& id) {
    if (id == scen::kL06AnnounceLifecycle) return lifecycle({{5, end(0)}});
    if (id == scen::kL06SessionStreamClose) return on_runner_fin(OnRunnerFin::Fin);
    return base_config();
}

TEST(Lite06AnnounceNotRun, EmptyTranscriptIsNeverAPass) {
    const LiteTranscript empty;
    for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(empty), kNotRun);
    for (const auto& probe : every_probe()) {
        LiteTranscript t;
        t.scenario_id = probe().id;
        t.broadcast_path = kBroadcast;
        t.track_name = kTrack;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06AnnounceNotRun, PublisherThatNeverConnects) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.connect_deadline = 100ms;
        const auto t = run(std::move(definition), verdict_publisher(probe().id),
                           [](ScriptedLitePeer& peer) { peer.establish_on_poll.reset(); });
        EXPECT_FALSE(t.established);
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06AnnounceNotRun, HarnessFailure) {
    for (const auto& probe : every_probe()) {
        ConformingLitePublisher publisher(verdict_publisher(probe().id));
        ScriptedLitePeer peer(publisher.reaction(), "moqt-22");
        ManualLiteClock clock;
        const auto t = run_lite_probe(peer, probe(), clock, kTick);
        ASSERT_TRUE(t.harness_failed) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06AnnounceNotRun, FlagsOnAnOtherwiseJudgedTranscript) {
    for (const auto& probe : every_probe()) {
        const auto base = run(probe(), verdict_publisher(probe().id));
        bool judged = false;
        for (const auto evaluator : all_evaluators()) judged = judged || evaluator(base).has_value();
        ASSERT_TRUE(judged) << base.scenario_id;
        for (const auto flag : {&LiteTranscript::harness_failed, &LiteTranscript::timed_out,
                                &LiteTranscript::event_limit_reached}) {
            auto t = base;
            t.*flag = true;
            for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
        }
        auto incomplete = base;
        incomplete.complete = false;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(incomplete), kNotRun);
    }
}

TEST(Lite06AnnounceNotRun, EventLimit) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.limits.max_bytes = 4;
        const auto t = run(std::move(definition), verdict_publisher(probe().id));
        ASSERT_TRUE(t.event_limit_reached) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06AnnounceNotRun, AHarnessClassIssueFromThePeer) {
    // Bytes after a FIN on a publisher Group stream (trailing_after_fin, Harness class): never a Fail, never a Pass.
    for (const auto& probe : every_probe()) {
        for (const bool defective : {false, true}) {
            auto config = verdict_publisher(probe().id);
            if (defective) config.defect = LiteDefect::SilentOnAnnounce;  // would be a Fail of 139
            config.hooks.on_start = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
                publisher.send_setup(peer);
                const auto id = peer.open_peer_uni();
                peer.data(id, join({stream_type(0x0), group_header({0, 0, 0})}), true);
                peer.data(id, bytes_of("late"));
                return true;
            };
            const auto t = run(probe(), config);
            ASSERT_FALSE(judgeable(t)) << t.scenario_id;
            for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
        }
    }
}

TEST(Lite06AnnounceNotRun, StimulusNeverDelivered) {
    // The publisher closes the session before the runner wrote anything.
    for (const auto& probe : every_probe()) {
        auto config = verdict_publisher(probe().id);
        config.hooks.on_start = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
            publisher.close(peer, 0x0);
            return true;
        };
        const auto t = run(probe(), config);
        ASSERT_TRUE(t.peer_close.has_value()) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06AnnounceNotRun, RunnerSetupRefusedByThePeer) {
    for (const auto& probe : every_probe()) {
        const auto t = run(probe(), verdict_publisher(probe().id), [](ScriptedLitePeer& peer) {
            peer.forced_status[3] = transport::TransportStatus::PeerStopped;
        });
        ASSERT_TRUE(t.runner_setup.refused.has_value()) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

TEST(Lite06AnnounceNotRun, AFixtureMismatchIsNotRun) {
    // A transcript whose recorded stimulus does not match its fixture (the definition was edited) is not judged.
    for (const auto& probe : every_probe()) {
        auto t = run(probe(), verdict_publisher(probe().id));
        t.broadcast_path = "elsewhere/live";
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), kNotRun) << t.scenario_id;
    }
}

}  // namespace
