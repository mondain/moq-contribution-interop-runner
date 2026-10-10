#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_session.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using moq::interop::scenarios::judgeable;
using moq::interop::scenarios::kLiteMaximumEvents;
using moq::interop::scenarios::kLiteMaximumEvidenceBytes;
using moq::interop::scenarios::LiteProbeContext;
using moq::interop::scenarios::LiteProbeController;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteStep;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
using moq::interop::session::LiteSession;
using moq::interop::session::LiteStreamKind;
using moq::interop::session::LiteStreamRecord;
using moq::interop::transport::TransportStatus;
using namespace moq::interop::test::lite;
namespace scen = moq::interop::scenarios;
namespace sess = moq::interop::session;
namespace transport = moq::interop::transport;

constexpr std::uint64_t kMs = 1000000;

Bytes subscribe_bytes(std::uint64_t id = 1, std::string track = "video", std::string broadcast = "demo/live") {
    l06::Subscribe subscribe;
    subscribe.subscribe_id = id;
    subscribe.broadcast_path = std::move(broadcast);
    subscribe.track_name = std::move(track);
    subscribe.range.subscriber_max_age_ms = 60000;
    return scen::lite_subscribe_stream_bytes(subscribe);
}

Bytes announce_bytes(std::string prefix = "demo/") { return scen::lite_announce_stream_bytes({std::move(prefix)}); }

template <class Message>
bool peer_sent_on_kind(const LiteSession& session, LiteStreamKind kind) {
    for (const auto* record : sess::runner_streams(session)) {
        if (record->kind != kind) continue;
        for (const auto* decoded : sess::peer_messages(*record))
            if (std::holds_alternative<Message>(decoded->message)) return true;
    }
    return false;
}

std::size_t finished_groups(const LiteSession& session) {
    std::size_t count = 0;
    for (const auto* record : sess::peer_streams(session))
        if (record->kind == LiteStreamKind::Group && record->fin_seen) ++count;
    return count;
}

// A stable text rendering of a transcript, for determinism and content checks.
std::string render(const LiteTranscript& t) {
    std::ostringstream out;
    out << t.scenario_id << " est=" << t.established << " alpn=" << t.alpn << " complete=" << t.complete
        << " hf=" << t.harness_failed << " to=" << t.timed_out << " lim=" << t.event_limit_reached
        << " early=" << t.peer_closed_early << " rc=" << t.runner_closed << " stim=" << t.stimulus_delivered
        << " start=" << t.started_ns << " estns=" << t.established_ns << " end=" << t.ended_ns << "\n";
    const auto step = [&](const scen::LiteStepRecord& s) {
        out << " step " << s.index << " " << s.label << " " << scen::to_string(s.kind) << " sid="
            << (s.stream_id ? std::to_string(*s.stream_id) : "-")
            << " cur=" << s.current_at_ns.value_or(0) << " gate=" << s.gate_opened_ns.value_or(0)
            << " exec=" << (s.executed_at_ns ? std::to_string(*s.executed_at_ns) : "-") << " exp=" << s.gate_expired
            << " acc=" << s.accepted << " fin=" << s.fin_accepted << " skip=" << s.skipped_reason << "\n";
    };
    step(t.runner_setup);
    for (const auto& s : t.steps) step(s);
    for (const auto& r : t.streams) {
        out << " stream " << r.stream_id << " " << sess::to_string(r.kind) << " bytes=" << r.bytes
            << " local=" << r.local_bytes << " fin=" << r.fin_seen << " lfin=" << r.local_fin
            << " reset=" << r.reset_seen << " opened=" << r.opened_ns << "\n";
        for (const auto& m : r.messages)
            out << "  msg " << sess::lite_message_name(m.message) << " @" << m.at_ns << " from"
                << static_cast<int>(m.from) << "\n";
        for (const auto& i : r.issues) out << "  issue " << i.code << " " << i.detail << "\n";
    }
    for (std::size_t i = 0; i < t.events.size(); ++i) {
        out << " event " << t.events[i].index() << " @" << t.event_times[i];
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&t.events[i]))
            out << " sid=" << data->stream_id << " n=" << data->data.size() << " fin=" << data->fin;
        out << "\n";
    }
    if (t.peer_close) out << " close " << t.peer_close->code << " @" << t.peer_close->at_ns << "\n";
    return out.str();
}

LiteProbeDefinition announce_then_subscribe() {
    LiteProbeDefinition definition;
    definition.id = "test-exchange";
    definition.deadline = 2000ms;
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    auto subscribe = scen::lite_open_bidi(subscribe_bytes(), false, "subscribe");
    subscribe.gate = [](const LiteSession& s) { return peer_sent_on_kind<l06::AnnounceOk>(s, LiteStreamKind::Announce); };
    subscribe.delay = 50ms;
    definition.steps.push_back(subscribe);
    definition.done = [](const LiteSession& s) { return finished_groups(s) >= 2; };
    return definition;
}

// --- establishment and the runner Setup stream ---

TEST(LiteProbe, RunnerSetupIsSentOnlyAfterEstablishment) {
    ScriptedLitePeer peer;
    peer.establish_on_poll = 3;
    ManualLiteClock clock(1000);
    LiteProbeDefinition definition;
    definition.id = "setup-only";
    definition.deadline = 100ms;
    LiteProbeController controller(definition, peer, clock);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(controller.poll());
        EXPECT_TRUE(peer.calls().empty()) << "runner wrote before the connection was established";
        clock.advance(1ms);
    }
    EXPECT_TRUE(controller.poll());
    ASSERT_NE(peer.runner_stream(3), nullptr);
    EXPECT_EQ(peer.runner_stream(3)->bytes, (Bytes{std::byte{0x01}, std::byte{0x01}, std::byte{0x00}}));
    EXPECT_TRUE(peer.runner_stream(3)->fin);
    const auto& t = controller.transcript();
    EXPECT_TRUE(t.established);
    EXPECT_EQ(t.alpn, "moq-lite-06");
    EXPECT_EQ(t.established_ns, 1000 + 3 * kMs);
    ASSERT_TRUE(t.runner_setup.stream_id.has_value());
    EXPECT_EQ(*t.runner_setup.stream_id, 3u);
    EXPECT_EQ(t.runner_setup.label, "runner-setup");
    EXPECT_TRUE(t.runner_setup.delivered());
    EXPECT_EQ(scen::lite_default_runner_setup(), (Bytes{std::byte{0x01}, std::byte{0x01}, std::byte{0x00}}));
}

TEST(LiteProbe, EmptyRunnerSetupSendsNothing) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.runner_setup.clear();
    definition.deadline = 10ms;
    definition.steps.push_back(scen::lite_mark("m"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(peer.runner_streams().empty());
    EXPECT_FALSE(t.runner_setup.executed());
    ASSERT_EQ(t.steps.size(), 1u);
    EXPECT_TRUE(t.steps[0].executed());
}

TEST(LiteProbe, WrongAlpnIsAHarnessFailure) {
    ScriptedLitePeer peer({}, "moqt-22");
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    LiteProbeController controller(definition, peer, clock);
    EXPECT_FALSE(controller.poll());
    EXPECT_FALSE(controller.poll());
    const auto& t = controller.transcript();
    EXPECT_TRUE(t.harness_failed);
    EXPECT_FALSE(t.harness_failure_reason.empty());
    EXPECT_EQ(t.alpn, "moqt-22");
    EXPECT_FALSE(t.complete);
    EXPECT_TRUE(peer.calls().empty());
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, StreamDataBeforeEstablishmentIsAHarnessFailure) {
    ScriptedLitePeer peer;
    peer.establish_on_poll = std::nullopt;
    peer.data(2, setup_stream(), true);
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, LiteProbeDefinition{}, clock);
    EXPECT_TRUE(t.harness_failed);
    EXPECT_FALSE(judgeable(t));
}

// --- steps, gates, delays ---

TEST(LiteProbe, StaticStepsRunInOrderWithGatesAndDelays) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, announce_then_subscribe(), clock);
    ASSERT_EQ(t.steps.size(), 2u);
    const auto& announce = t.steps[0];
    const auto& subscribe = t.steps[1];
    ASSERT_TRUE(announce.executed());
    ASSERT_TRUE(subscribe.executed());
    EXPECT_EQ(*announce.stream_id, 1u);
    EXPECT_EQ(*subscribe.stream_id, 5u);
    // The runner Setup goes first, then the steps in order.
    EXPECT_LE(*t.runner_setup.executed_at_ns, *announce.executed_at_ns);
    EXPECT_LT(*announce.executed_at_ns, *subscribe.executed_at_ns);
    // The subscribe gate opened only once ANNOUNCE_OK had been decoded, and the delay ran from there.
    ASSERT_TRUE(subscribe.gate_opened_ns.has_value());
    const auto* announce_stream = [&]() -> const LiteStreamRecord* {
        for (const auto& r : t.streams)
            if (r.stream_id == 1) return &r;
        return nullptr;
    }();
    ASSERT_NE(announce_stream, nullptr);
    const auto peer_messages = sess::peer_messages(*announce_stream);
    ASSERT_FALSE(peer_messages.empty());
    ASSERT_TRUE(std::holds_alternative<l06::AnnounceOk>(peer_messages.front()->message));
    EXPECT_GE(*subscribe.gate_opened_ns, peer_messages.front()->at_ns);
    EXPECT_GE(*subscribe.executed_at_ns - *subscribe.gate_opened_ns, 50 * kMs);
    EXPECT_LT(*subscribe.executed_at_ns - *subscribe.gate_opened_ns, 52 * kMs);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_TRUE(judgeable(t));
}

TEST(LiteProbe, GateDeadlineExpirySkipsTheStep) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    auto never = scen::lite_open_bidi(announce_bytes(), false, "never");
    never.gate = [](const LiteSession&) { return false; };
    never.gate_deadline = 100ms;
    definition.steps.push_back(never);
    definition.steps.push_back(scen::lite_mark("after"));
    definition.observation_window = 10ms;
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_EQ(t.steps.size(), 2u);
    EXPECT_TRUE(t.steps[0].gate_expired);
    EXPECT_FALSE(t.steps[0].executed());
    EXPECT_FALSE(t.steps[0].stream_id.has_value());
    ASSERT_TRUE(t.steps[1].executed());
    EXPECT_GE(*t.steps[1].executed_at_ns - *t.steps[0].current_at_ns, 100 * kMs);
    EXPECT_LT(*t.steps[1].executed_at_ns - *t.steps[0].current_at_ns, 102 * kMs);
    // Only the runner Setup stream was opened.
    EXPECT_EQ(peer.runner_streams().size(), 1u);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_FALSE(t.stimulus_delivered);
    EXPECT_LT(t.ended_ns, 200 * kMs) << "the observation window ends the probe well before the deadline";
}

TEST(LiteProbe, GateStillClosedAtTheProbeDeadlineExpiresAndTimesOut) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    auto never = scen::lite_wait(0ms, "never");
    never.gate = [](const LiteSession&) { return false; };
    definition.steps.push_back(never);
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_EQ(t.steps.size(), 1u);
    EXPECT_TRUE(t.steps[0].gate_expired);
    EXPECT_TRUE(t.timed_out);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, DynamicNextStepsSeesDecodedMessagesAndAppendsSteps) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "subscribe"));
    std::size_t calls = 0;
    std::optional<std::uint64_t> seen_group;
    definition.next_steps = [&](const LiteSession& s, LiteProbeContext& context) {
        ++calls;
        std::vector<LiteStep> more;
        if (context.finished) return more;
        for (const auto* record : sess::runner_streams(s)) {
            if (record->kind != LiteStreamKind::Subscribe) continue;
            for (const auto* decoded : sess::peer_messages(*record)) {
                if (const auto* ok = std::get_if<l06::SubscribeOk>(&decoded->message)) {
                    seen_group = ok->group;
                    context.values["latest"] = ok->group;
                    EXPECT_EQ(context.step_count(), 1u);
                    EXPECT_EQ(context.stream_of(0), std::optional<transport::StreamId>(1));
                    more.push_back(scen::lite_reset(0, 0x1, "cancel-subscribe"));
                    context.finished = true;
                }
            }
        }
        return more;
    };
    definition.observation_window = 20ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_GE(calls, 1u);
    ASSERT_TRUE(seen_group.has_value());
    EXPECT_EQ(*seen_group, 5u);
    ASSERT_EQ(t.steps.size(), 2u);
    EXPECT_TRUE(t.steps[1].dynamic);
    EXPECT_EQ(t.steps[1].label, "cancel-subscribe");
    ASSERT_TRUE(t.steps[1].executed());
    EXPECT_EQ(t.steps[1].stream_id, std::optional<transport::StreamId>(1));
    ASSERT_NE(peer.runner_stream(1), nullptr);
    EXPECT_EQ(peer.runner_stream(1)->reset_code, std::optional<std::uint64_t>(0x1));
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
}

TEST(LiteProbe, ResetStopSendingAndCloseIssueTheTransportCallsWithTheirCodes) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "subscribe"));
    definition.steps.push_back(scen::lite_send_on(0, bytes_of("xy"), false, "more"));
    definition.steps.push_back(scen::lite_reset(0, 0x5, "reset"));
    definition.steps.push_back(scen::lite_stop_sending(0, 0x6, "stop"));
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    definition.steps.push_back(scen::lite_fin(4, "fin-announce"));
    definition.steps.push_back(scen::lite_close(0x3, "bye", "close"));
    definition.steps.push_back(scen::lite_mark("never-reached"));
    const auto t = run_lite_probe(peer, definition, clock);
    using Kind = RunnerCall::Kind;
    std::vector<std::pair<Kind, std::uint64_t>> seen;
    for (const auto& call : peer.calls())
        if (call.kind == Kind::Reset || call.kind == Kind::StopSending || call.kind == Kind::Close)
            seen.emplace_back(call.kind, call.code);
    EXPECT_EQ(seen, (std::vector<std::pair<Kind, std::uint64_t>>{{Kind::Reset, 0x5}, {Kind::StopSending, 0x6},
                                                                 {Kind::Close, 0x3}}));
    ASSERT_NE(peer.runner_stream(1), nullptr);
    EXPECT_EQ(peer.runner_stream(1)->reset_code, std::optional<std::uint64_t>(0x5));
    EXPECT_EQ(peer.runner_stream(1)->stop_sending_code, std::optional<std::uint64_t>(0x6));
    EXPECT_EQ(peer.runner_stream(1)->bytes, join({subscribe_bytes(), bytes_of("xy")}));
    ASSERT_NE(peer.runner_stream(5), nullptr);
    EXPECT_TRUE(peer.runner_stream(5)->fin);
    ASSERT_TRUE(peer.runner_close().has_value());
    EXPECT_EQ(peer.runner_close()->code, 0x3u);
    EXPECT_EQ(peer.runner_close()->reason, "bye");
    EXPECT_TRUE(t.runner_closed);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_EQ(t.steps[2].code, 0x5u);
    EXPECT_EQ(t.steps[6].reason, "bye");
    EXPECT_FALSE(t.steps[7].executed());
}

TEST(LiteProbe, StepOnAStreamOfANonStreamStepIsAHarnessFailure) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.steps.push_back(scen::lite_mark("m"));
    definition.steps.push_back(scen::lite_reset(0, 1, "bad-ref"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.harness_failed);
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, StepTargetPicksAPeerStream) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "subscribe"));
    LiteStep stop;
    stop.kind = LiteStep::Kind::StopSending;
    stop.code = 0x9;
    stop.label = "stop-group";
    stop.target = [](const LiteSession& s) -> std::optional<transport::StreamId> {
        for (const auto* record : sess::peer_streams(s))
            if (record->kind == LiteStreamKind::Group) return record->stream_id;
        return std::nullopt;
    };
    definition.steps.push_back(stop);
    definition.observation_window = 5ms;
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_TRUE(t.steps[1].executed());
    ASSERT_TRUE(t.steps[1].stream_id.has_value());
    EXPECT_EQ(*t.steps[1].stream_id & 3u, 2u);
    ASSERT_NE(peer.runner_stream(*t.steps[1].stream_id), nullptr);
    EXPECT_EQ(peer.runner_stream(*t.steps[1].stream_id)->stop_sending_code, std::optional<std::uint64_t>(0x9));
}

TEST(LiteProbe, PartialAndBlockedWritesCompleteOverSeveralPolls) {
    ScriptedLitePeer peer;
    peer.max_write_chunk = 2;
    peer.would_block_writes = 3;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), true, "subscribe"));
    definition.observation_window = 1ms;
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_TRUE(t.steps[0].delivered());
    EXPECT_TRUE(t.steps[0].fin_accepted);
    EXPECT_EQ(t.steps[0].accepted, subscribe_bytes().size());
    EXPECT_EQ(peer.runner_stream(1)->bytes, subscribe_bytes());
    EXPECT_TRUE(peer.runner_stream(1)->fin);
    EXPECT_GT(peer.runner_stream(1)->writes, 1u);
    // The recorder saw the runner's bytes once each.
    for (const auto& record : t.streams)
        if (record.stream_id == 1) {
            EXPECT_EQ(record.local_bytes, subscribe_bytes().size());
            EXPECT_EQ(record.kind, LiteStreamKind::Subscribe);
        }
}

TEST(LiteProbe, PeerRefusalOfAWriteEndsTheStepWithoutAHarnessFailure) {
    ScriptedLitePeer peer;
    peer.forced_status[1] = TransportStatus::PeerStopped;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "subscribe"));
    definition.steps.push_back(scen::lite_mark("after"));
    definition.observation_window = 1ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_FALSE(t.harness_failed);
    ASSERT_TRUE(t.steps[0].executed());
    EXPECT_EQ(t.steps[0].refused, std::optional<TransportStatus>(TransportStatus::PeerStopped));
    EXPECT_FALSE(t.steps[0].delivered());
    EXPECT_TRUE(t.steps[1].executed());
    EXPECT_FALSE(t.stimulus_delivered);
}

TEST(LiteProbe, TransportRejectionIsAHarnessFailure) {
    ScriptedLitePeer peer;
    peer.forced_status[1] = TransportStatus::InternalError;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "subscribe"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.harness_failed);
}

// --- peer close, deadline, completion ---

TEST(LiteProbe, PeerCloseBeforeTheStepsFinishIsFlaggedEarly) {
    ScriptedLitePeer peer([](ScriptedLitePeer& p) {
        if (p.polls() == 2) p.close_session(0x3, "violation");
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_wait(100ms, "wait"));
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.peer_closed_early);
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
    EXPECT_EQ(t.peer_close->space, transport::CloseErrorSpace::Application);
    EXPECT_EQ(t.peer_close->reason, "violation");
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_FALSE(t.steps[1].executed());
    EXPECT_LT(t.ended_ns, 10 * kMs);
    EXPECT_TRUE(judgeable(t)) << "the evaluators decide whether an early close is the observation";
}

TEST(LiteProbe, PeerCloseAfterTheStepsIsNotEarly) {
    ScriptedLitePeer peer([](ScriptedLitePeer& p) {
        if (p.polls() == 10) p.close_session(0x0);
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_FALSE(t.peer_closed_early);
    EXPECT_TRUE(t.peer_close.has_value());
    EXPECT_TRUE(t.complete);
}

TEST(LiteProbe, DoneNeverHoldingTimesOutAtTheDeadline) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 300ms;
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    definition.done = [](const LiteSession&) { return false; };
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.timed_out);
    EXPECT_TRUE(t.complete);
    EXPECT_TRUE(t.stimulus_delivered);
    EXPECT_EQ(t.ended_ns - t.established_ns, 300 * kMs);
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, WithoutDoneTheDeadlineIsANormalEnd) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 300ms;
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(t.complete);
    EXPECT_EQ(t.ended_ns - t.established_ns, 300 * kMs);
    EXPECT_TRUE(judgeable(t));
}

TEST(LiteProbe, DoneEndsTheProbeEarly) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, announce_then_subscribe(), clock);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_LT(t.ended_ns, 200 * kMs);
}

TEST(LiteProbe, NoConnectionTimesOutFromTheFirstPoll) {
    ScriptedLitePeer peer;
    peer.establish_on_poll = std::nullopt;
    ManualLiteClock clock(5 * kMs);
    LiteProbeDefinition definition;
    definition.deadline = 50ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_FALSE(t.established);
    EXPECT_TRUE(t.timed_out);
    EXPECT_FALSE(t.complete);
    EXPECT_EQ(t.started_ns, 5 * kMs);
    EXPECT_EQ(t.ended_ns, 55 * kMs);
    EXPECT_TRUE(peer.calls().empty());
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, PollAfterTheEndDoesNothing) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 5ms;
    LiteProbeController controller(definition, peer, clock);
    while (controller.poll()) clock.advance(1ms);
    const auto polls = peer.polls();
    const auto before = render(controller.transcript());
    clock.advance(1000ms);
    EXPECT_FALSE(controller.poll());
    EXPECT_EQ(peer.polls(), polls);
    EXPECT_EQ(render(controller.transcript()), before);
}

// --- limits ---

TEST(LiteProbe, EventCountLimitStopsRecordingButNotStepping) {
    bool flooded = false;
    ScriptedLitePeer peer([&](ScriptedLitePeer& p) {
        if (flooded) return;
        flooded = true;
        const auto id = p.open_peer_uni();
        p.data(id, stream_type(0x7));
        for (std::size_t i = 0; i < kLiteMaximumEvents + 100; ++i) p.data(id, Bytes{std::byte{0x1}});
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    auto late = scen::lite_open_bidi(announce_bytes(), false, "after-flood");
    late.delay = 200ms;
    definition.steps.push_back(late);
    definition.observation_window = 1ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.event_limit_reached);
    EXPECT_FALSE(t.event_limit_reason.empty());
    EXPECT_LE(t.events.size(), kLiteMaximumEvents);
    EXPECT_EQ(t.events.size(), t.event_times.size());
    EXPECT_TRUE(t.steps[0].executed()) << "stepping continues after the limit";
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, EvidenceByteLimitStopsRecording) {
    bool flooded = false;
    ScriptedLitePeer peer([&](ScriptedLitePeer& p) {
        if (flooded) return;
        flooded = true;
        const auto id = p.open_peer_uni();
        Bytes chunk(1u << 20, std::byte{0x7});
        for (std::size_t i = 0; i < kLiteMaximumEvidenceBytes / chunk.size() + 2; ++i) p.data(id, chunk);
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 50ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.event_limit_reached);
    std::size_t recorded = 0;
    for (const auto& event : t.events)
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) recorded += data->data.size();
    EXPECT_LE(recorded, kLiteMaximumEvidenceBytes);
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, RecorderLimitMarksTheTranscript) {
    ScriptedLitePeer peer([](ScriptedLitePeer& p) {
        if (p.polls() == 1) p.raw_event(transport::EventQueueOverflowEvent{});
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 20ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.event_limit_reached);
    EXPECT_FALSE(judgeable(t));
}

TEST(LiteProbe, AHarnessIssueOnAPeerStreamIsNotJudgeable) {
    // A FRAME claiming 2 MiB is over the default decode limit: length_exceeds_limit, class Harness.
    ScriptedLitePeer peer([](ScriptedLitePeer& p) {
        if (p.polls() != 1) return;
        const auto id = p.open_peer_uni();
        p.data(id, join({stream_type(0x0), group_header({1, 0, 0}),
                         Bytes{std::byte{0x00}, std::byte{0x80}, std::byte{0x20}, std::byte{0x00}, std::byte{0x00}}}));
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 20ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_FALSE(judgeable(t));
}

// --- a full exchange and determinism ---

TEST(LiteProbe, BindingDefaultsToUnknownAndIsCopiedToTheTranscript) {
    for (const auto binding :
         {scen::LiteBinding::Unknown, scen::LiteBinding::NativeQuic, scen::LiteBinding::WebTransport}) {
        ConformingLitePublisher publisher;
        ScriptedLitePeer peer(publisher.reaction());
        ManualLiteClock clock;
        auto definition = announce_then_subscribe();
        EXPECT_EQ(definition.binding, scen::LiteBinding::Unknown);
        definition.binding = binding;
        EXPECT_EQ(run_lite_probe(peer, definition, clock).binding, binding);
    }
    EXPECT_EQ(LiteTranscript{}.binding, scen::LiteBinding::Unknown);
}

TEST(LiteProbe, FullExchangeTranscriptContents) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    auto definition = announce_then_subscribe();
    definition.session_url_has_path = true;
    definition.binding = scen::LiteBinding::WebTransport;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_EQ(t.scenario_id, "test-exchange");
    EXPECT_TRUE(t.session_url_has_path);
    EXPECT_EQ(t.binding, scen::LiteBinding::WebTransport);
    EXPECT_TRUE(judgeable(t));
    ASSERT_FALSE(t.events.empty());
    EXPECT_TRUE(std::holds_alternative<transport::ConnectionEstablishedEvent>(t.events.front()));
    EXPECT_EQ(t.events.size(), t.event_times.size());
    for (std::size_t i = 1; i < t.event_times.size(); ++i) EXPECT_LE(t.event_times[i - 1], t.event_times[i]);

    std::size_t peer_setups = 0, runner_setups = 0, groups = 0;
    bool announce_ok = false, announce_start = false, subscribe_ok = false;
    for (const auto& record : t.streams) {
        if (record.kind == LiteStreamKind::Setup && record.origin == sess::LiteOrigin::Peer) {
            ++peer_setups;
            ASSERT_EQ(sess::peer_messages(record).size(), 1u);
            EXPECT_TRUE(record.fin_seen);
        }
        if (record.kind == LiteStreamKind::Setup && record.origin == sess::LiteOrigin::Runner) {
            ++runner_setups;
            EXPECT_TRUE(sess::peer_messages(record).empty());
            EXPECT_EQ(sess::runner_messages(record).size(), 1u);
            EXPECT_TRUE(record.local_fin);
        }
        if (record.kind == LiteStreamKind::Announce) {
            const auto messages = sess::peer_messages(record);
            ASSERT_EQ(messages.size(), 2u);
            announce_ok = std::holds_alternative<l06::AnnounceOk>(messages[0]->message);
            const auto* start = std::get_if<l06::AnnounceStart>(&messages[1]->message);
            announce_start = start != nullptr && start->suffix == "live";
        }
        if (record.kind == LiteStreamKind::Subscribe) {
            const auto messages = sess::peer_messages(record);
            ASSERT_EQ(messages.size(), 1u);
            const auto* ok = std::get_if<l06::SubscribeOk>(&messages[0]->message);
            subscribe_ok = ok != nullptr && ok->group == 5;
        }
        if (record.kind == LiteStreamKind::Group) {
            ++groups;
            const auto messages = sess::peer_messages(record);
            ASSERT_EQ(messages.size(), 3u);
            EXPECT_TRUE(std::holds_alternative<l06::GroupHeader>(messages[0]->message));
            EXPECT_TRUE(record.fin_seen);
        }
        EXPECT_TRUE(sess::peer_protocol_issues(record).empty());
    }
    EXPECT_EQ(peer_setups, 1u);
    EXPECT_EQ(runner_setups, 1u);
    EXPECT_TRUE(announce_ok);
    EXPECT_TRUE(announce_start);
    EXPECT_TRUE(subscribe_ok);
    EXPECT_EQ(groups, 2u);
    ASSERT_EQ(t.steps.size(), 2u);
    EXPECT_EQ(t.steps[0].label, "announce");
    EXPECT_EQ(t.steps[0].bytes, announce_bytes());
    EXPECT_EQ(t.steps[1].label, "subscribe");
    EXPECT_FALSE(t.peer_close.has_value());
    // The peer received exactly what the steps wrote.
    EXPECT_EQ(peer.runner_stream(1)->bytes, announce_bytes());
    EXPECT_EQ(peer.runner_stream(5)->bytes, subscribe_bytes());
    EXPECT_EQ(publisher.runner_setups(), 1u);
}

TEST(LiteProbe, SameScriptTwiceGivesTheSameTranscript) {
    const auto once = [] {
        ConformingLitePublisher publisher;
        ScriptedLitePeer peer(publisher.reaction());
        ManualLiteClock clock(777);
        return render(run_lite_probe(peer, announce_then_subscribe(), clock));
    };
    const auto first = once();
    const auto second = once();
    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("GROUP"), std::string::npos);
}

TEST(LiteProbe, TimingConstantsMatchThePlan) {
    EXPECT_EQ(scen::kLiteSetupAllowance, 2000ms);
    EXPECT_EQ(scen::kLiteResponseAllowance, 3000ms);
    EXPECT_EQ(scen::kLiteCloseAllowance, 3000ms);
    EXPECT_EQ(scen::kLiteObservationWindow, 6000ms);
}

// --- the conforming publisher and its defects ---

TEST(ConformingLitePublisher, ResetsUnknownStreamTypesAndRefusesUnknownTracks) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    definition.steps.push_back(scen::lite_open_bidi(stream_type(0x7), false, "unknown-bidi"));
    definition.steps.push_back(scen::lite_send_uni(stream_type(0x9), false, "unknown-uni"));
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(2, "nope"), false, "unknown-track"));
    const auto t = run_lite_probe(peer, definition, clock);
    const auto find = [&](transport::StreamId id) -> const LiteStreamRecord* {
        for (const auto& r : t.streams)
            if (r.stream_id == id) return &r;
        return nullptr;
    };
    ASSERT_TRUE(t.steps[0].stream_id && t.steps[1].stream_id && t.steps[2].stream_id);
    const auto* bidi = find(*t.steps[0].stream_id);
    ASSERT_NE(bidi, nullptr);
    EXPECT_TRUE(bidi->reset_seen);
    EXPECT_TRUE(bidi->stop_sending_seen);
    const auto* uni = find(*t.steps[1].stream_id);
    ASSERT_NE(uni, nullptr);
    EXPECT_TRUE(uni->stop_sending_seen);
    const auto* track = find(*t.steps[2].stream_id);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->reset_code, std::optional<std::uint64_t>(0x33));
    EXPECT_FALSE(t.peer_close.has_value());
}

TEST(ConformingLitePublisher, SecondRunnerSetupStreamClosesWithProtocolViolation) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    definition.steps.push_back(scen::lite_send_uni(scen::lite_default_runner_setup(), true, "second-setup-stream"));
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
    EXPECT_EQ(publisher.runner_setups(), 2u);
}

TEST(ConformingLitePublisher, DefectNoSetupStream) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::NoSetupStream;
    ConformingLitePublisher publisher(config);
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 50ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.established);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.streams.empty()) << "the runner Setup stream is recorded";
    for (const auto& record : t.streams) EXPECT_NE(record.origin, sess::LiteOrigin::Peer);
    // Without the defect the same probe records the publisher's Setup stream.
    ConformingLitePublisher conforming;
    ScriptedLitePeer conforming_peer(conforming.reaction());
    ManualLiteClock conforming_clock;
    const auto good = run_lite_probe(conforming_peer, definition, conforming_clock);
    bool peer_setup = false;
    for (const auto& record : good.streams)
        peer_setup = peer_setup || (record.origin == sess::LiteOrigin::Peer && record.kind == LiteStreamKind::Setup);
    EXPECT_TRUE(peer_setup);
}

TEST(ConformingLitePublisher, DefectSilentOnAnnounce) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::SilentOnAnnounce;
    ConformingLitePublisher publisher(config);
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    auto definition = announce_then_subscribe();
    definition.deadline = 200ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.steps[1].gate_expired);
    EXPECT_TRUE(t.timed_out);
    bool announce_seen = false;
    for (const auto& record : t.streams) {
        if (record.kind != LiteStreamKind::Announce) continue;
        announce_seen = true;
        EXPECT_TRUE(sess::peer_messages(record).empty());
    }
    EXPECT_TRUE(announce_seen);
}

TEST(ConformingLitePublisher, DefectIgnoreUnknownStreams) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::IgnoreUnknownStreams;
    ConformingLitePublisher publisher(config);
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    definition.steps.push_back(scen::lite_open_bidi(stream_type(0x7), false, "unknown-bidi"));
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_TRUE(t.steps[0].delivered());
    bool unknown_seen = false;
    for (const auto& record : t.streams) {
        unknown_seen = unknown_seen || record.kind == LiteStreamKind::UnregisteredBidi;
        EXPECT_FALSE(record.reset_seen);
        EXPECT_FALSE(record.stop_sending_seen);
    }
    EXPECT_TRUE(unknown_seen);
}


const LiteStreamRecord* find_stream(const LiteTranscript& t, transport::StreamId id) {
    for (const auto& r : t.streams)
        if (r.stream_id == id) return &r;
    return nullptr;
}

TEST(ConformingLitePublisher, OnRequestHookReplacesTheAnnounceAnswer) {
    // Without the hook: ANNOUNCE_OK then ANNOUNCE_START.
    {
        ConformingLitePublisher publisher;
        ScriptedLitePeer peer(publisher.reaction());
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.deadline = 50ms;
        definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
        const auto t = run_lite_probe(peer, definition, clock);
        const auto* record = find_stream(t, 1);
        ASSERT_NE(record, nullptr);
        const auto messages = sess::peer_messages(*record);
        ASSERT_EQ(messages.size(), 2u);
        EXPECT_TRUE(std::holds_alternative<l06::AnnounceStart>(messages[1]->message));
    }
    // With the hook: exactly the two ANNOUNCE_OKs it wrote, and no ANNOUNCE_START.
    ConformingLitePublisherConfig config;
    std::size_t hook_calls = 0;
    config.hooks.on_request = [&](ConformingLitePublisher& self, ScriptedLitePeer& p, const LiteRunnerRequest& request) {
        if (!std::holds_alternative<l06::AnnounceRequest>(request.message)) return false;
        ++hook_calls;
        p.data(request.stream, join({announce_ok({self.config().hop_id, 0}), announce_ok({self.config().hop_id, 0})}));
        return true;
    };
    ConformingLitePublisher publisher(config);
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 50ms;
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_EQ(hook_calls, 1u);
    Bytes received;
    for (const auto& event : t.events)
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event); data && data->stream_id == 1)
            received.insert(received.end(), data->data.begin(), data->data.end());
    EXPECT_EQ(received, join({announce_ok({7, 0}), announce_ok({7, 0})}));
    const auto* record = find_stream(t, 1);
    ASSERT_NE(record, nullptr);
    for (const auto* decoded : sess::peer_messages(*record))
        EXPECT_FALSE(std::holds_alternative<l06::AnnounceStart>(decoded->message));
    ASSERT_EQ(publisher.requests().size(), 2u);  // the runner SETUP and the ANNOUNCE_REQUEST
}

TEST(ConformingLitePublisher, OnStartHookSuppressesTheSetupAndOnPollRunsEveryReaction) {
    ConformingLitePublisherConfig config;
    std::size_t starts = 0, polls = 0;
    config.hooks.on_start = [&](ConformingLitePublisher&, ScriptedLitePeer&) {
        ++starts;
        return true;  // handled: no Setup stream
    };
    config.hooks.on_poll = [&](ConformingLitePublisher&, ScriptedLitePeer&) { ++polls; };
    ConformingLitePublisher publisher(config);
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 20ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_EQ(starts, 1u);
    EXPECT_EQ(polls, peer.polls());
    EXPECT_GE(polls, 20u);
    for (const auto& record : t.streams) EXPECT_NE(record.origin, sess::LiteOrigin::Peer);
}

// --- review fixes: the continuation (I1, M4) ---

TEST(LiteProbe, ContinuationNeverFinishedTimesOutAtTheDeadline) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "sub"));
    definition.next_steps = [](const LiteSession&, LiteProbeContext&) { return std::vector<LiteStep>{}; };
    definition.observation_window = 10ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.complete);
    EXPECT_TRUE(t.timed_out);
    EXPECT_FALSE(t.stimulus_delivered);
    EXPECT_FALSE(judgeable(t));
    EXPECT_EQ(t.ended_ns - t.established_ns, 100 * kMs);
}

TEST(LiteProbe, OpenContinuationMeansTheStimulusWasNotDelivered) {
    for (const bool finish : {false, true}) {
        ScriptedLitePeer peer;
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.deadline = 100ms;
        definition.steps.push_back(scen::lite_mark("m"));
        definition.next_steps = [finish](const LiteSession&, LiteProbeContext& context) {
            context.finished = finish;
            return std::vector<LiteStep>{};
        };
        definition.done = [](const LiteSession&) { return true; };
        const auto t = run_lite_probe(peer, definition, clock);
        EXPECT_TRUE(t.complete);
        EXPECT_FALSE(t.timed_out);
        EXPECT_EQ(t.stimulus_delivered, finish) << "finish=" << finish;
        EXPECT_EQ(scen::judgeable_with_stimulus(t), finish) << "finish=" << finish;
    }
}

TEST(LiteProbe, PeerCloseWithAnOpenContinuationIsEarly) {
    for (const bool finish_on_close : {false, true}) {
        ScriptedLitePeer peer([](ScriptedLitePeer& p) {
            if (p.polls() == 5) p.close_session(0x0);
        });
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.deadline = 1000ms;
        definition.steps.push_back(scen::lite_mark("m"));
        definition.next_steps = [finish_on_close](const LiteSession& s, LiteProbeContext& context) {
            if (finish_on_close && s.peer_close()) context.finished = true;
            return std::vector<LiteStep>{};
        };
        const auto t = run_lite_probe(peer, definition, clock);
        EXPECT_TRUE(t.peer_close.has_value());
        EXPECT_TRUE(t.steps[0].executed());
        EXPECT_EQ(t.peer_closed_early, !finish_on_close) << "finish_on_close=" << finish_on_close;
    }
}

TEST(LiteProbe, ASubscribeRefusedByAResetWakesTheContinuation) {
    ConformingLitePublisherConfig config;
    config.track = "other";  // the SUBSCRIBE for "video" is refused by RESET_STREAM + STOP_SENDING, no message
    ConformingLitePublisher publisher(config);
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "sub"));
    std::size_t calls_after_reset = 0;
    definition.next_steps = [&](const LiteSession& s, LiteProbeContext& context) {
        for (const auto* record : sess::runner_streams(s))
            if (record->kind == LiteStreamKind::Subscribe && record->reset_seen) {
                ++calls_after_reset;
                context.finished = true;
            }
        return std::vector<LiteStep>{};
    };
    definition.observation_window = 5ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_GE(calls_after_reset, 1u);
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
}

TEST(LiteProbe, AContinuationAppendingForeverHitsTheStepLimit) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1500ms;  // one new stream per poll: the recorder's 1024-stream cap comes first
    definition.next_steps = [](const LiteSession&, LiteProbeContext&) {
        std::vector<LiteStep> more;
        more.push_back(scen::lite_send_uni(stream_type(0x9), false, "u"));  // each is stopped: a new wake
        return more;
    };
    const auto t = run_lite_probe(peer, definition, clock);
    // Each step opens a stream, so the recorder's max_streams (1024) stops the wakes first; either bound ends it
    // unjudgeable.
    EXPECT_TRUE(t.harness_failed || t.event_limit_reached);
    EXPECT_LE(t.steps.size(), scen::kLiteMaximumSteps);
    EXPECT_FALSE(judgeable(t));
    // The step cap itself: one wake appending more than the cap.
    ScriptedLitePeer quiet;
    ManualLiteClock quiet_clock;
    LiteProbeDefinition burst;
    burst.deadline = 50ms;
    burst.next_steps = [](const LiteSession&, LiteProbeContext&) {
        return std::vector<LiteStep>(scen::kLiteMaximumSteps + 1, scen::lite_mark("m"));
    };
    const auto capped = run_lite_probe(quiet, burst, quiet_clock);
    EXPECT_TRUE(capped.harness_failed);
    EXPECT_LE(capped.steps.size(), scen::kLiteMaximumSteps);
}

// --- review fixes: connect deadline (M1) ---

TEST(LiteProbe, ConnectDeadlineBoundsOnlyTheConnectionWait) {
    ScriptedLitePeer peer;
    peer.establish_on_poll = std::nullopt;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.connect_deadline = 30ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_FALSE(t.established);
    EXPECT_TRUE(t.timed_out);
    EXPECT_EQ(t.ended_ns, 30 * kMs);
}

TEST(LiteProbe, AShortScenarioDeadlineNoLongerShortensTheConnectWait) {
    for (const bool with_connect_deadline : {false, true}) {
        ScriptedLitePeer peer;
        peer.establish_on_poll = 99;
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.deadline = 20ms;
        if (with_connect_deadline) definition.connect_deadline = 200ms;
        definition.steps.push_back(scen::lite_mark("m"));
        const auto t = run_lite_probe(peer, definition, clock);
        if (!with_connect_deadline) {
            EXPECT_FALSE(t.established);
            EXPECT_EQ(t.ended_ns, 20 * kMs);
            continue;
        }
        EXPECT_TRUE(t.established);
        EXPECT_EQ(t.established_ns, 99 * kMs);
        EXPECT_EQ(t.ended_ns, t.established_ns + 20 * kMs) << "the scenario deadline restarts at establishment";
        EXPECT_FALSE(t.timed_out);
    }
}

// --- review fixes: judgeable_with_stimulus (M2) and the Harness-issue rule (M3) ---

TEST(LiteProbe, JudgeableWithStimulusIsTheDefaultGate) {
    {
        ConformingLitePublisher publisher;
        ScriptedLitePeer peer(publisher.reaction());
        ManualLiteClock clock;
        EXPECT_TRUE(scen::judgeable_with_stimulus(run_lite_probe(peer, announce_then_subscribe(), clock)));
    }
    {  // the peer closes before the steps: judgeable (a close probe may judge it), but not with stimulus
        ScriptedLitePeer peer([](ScriptedLitePeer& p) {
            if (p.polls() == 2) p.close_session(0x3);
        });
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.steps.push_back(scen::lite_wait(100ms, "wait"));
        const auto t = run_lite_probe(peer, definition, clock);
        EXPECT_TRUE(judgeable(t));
        EXPECT_FALSE(scen::judgeable_with_stimulus(t));
    }
    {  // a write the peer refused
        ScriptedLitePeer peer;
        peer.forced_status[1] = TransportStatus::PeerReset;
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.deadline = 20ms;
        definition.steps.push_back(scen::lite_open_bidi(subscribe_bytes(), false, "subscribe"));
        const auto t = run_lite_probe(peer, definition, clock);
        EXPECT_TRUE(judgeable(t));
        EXPECT_FALSE(scen::judgeable_with_stimulus(t));
    }
}

TEST(LiteProbe, TheRunnersDeliberatelyMalformedProbeBytesStayJudgeable) {
    const Bytes malformed_subscribe{std::byte{0x02}, std::byte{0x00}, std::byte{0xBF},
                                    std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    const Bytes oversize_subscribe{std::byte{0x02}, std::byte{0x80}, std::byte{0x20}, std::byte{0x00}, std::byte{0x00}};
    for (const auto& probe : {malformed_subscribe, oversize_subscribe}) {
        ScriptedLitePeer peer;
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.deadline = 20ms;
        definition.steps.push_back(scen::lite_open_bidi(probe, true, "bad"));
        const auto t = run_lite_probe(peer, definition, clock);
        const auto* record = find_stream(t, 1);
        ASSERT_NE(record, nullptr);
        ASSERT_FALSE(record->issues.empty()) << "the runner's bytes raise an issue";
        for (const auto& issue : record->issues) EXPECT_EQ(issue.from, sess::LiteOrigin::Runner);
        EXPECT_TRUE(sess::harness_issues(*record).empty());
        EXPECT_TRUE(judgeable(t));
        EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    }
}

// --- review fixes: transport realism (M5) ---

TEST(LiteProbe, WritingAPublisherUniStreamIsAHarnessFailure) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    LiteStep write;
    write.kind = LiteStep::Kind::SendOnStream;
    write.bytes = bytes_of("x");
    write.label = "write-peer-setup-stream";
    write.target = [](const LiteSession& s) -> std::optional<transport::StreamId> {
        for (const auto* record : sess::peer_streams(s))
            if (record->kind == LiteStreamKind::Setup) return record->stream_id;
        return std::nullopt;
    };
    definition.steps.push_back(write);
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.harness_failed);
    ScriptedLitePeer fresh;
    EXPECT_EQ(fresh.write(2, bytes_of("x"), false).status, TransportStatus::InvalidState);
    EXPECT_EQ(fresh.reset(2, 0).status, TransportStatus::InvalidState);
}

TEST(LiteProbe, StopSendingOnARunnerUniStreamIsAHarnessFailure) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 100ms;
    definition.steps.push_back(scen::lite_send_uni(stream_type(0x9), false, "uni"));
    definition.steps.push_back(scen::lite_stop_sending(0, 0x1, "stop-own-uni"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.harness_failed);
    ScriptedLitePeer fresh;
    EXPECT_EQ(fresh.stop_sending(3, 0).status, TransportStatus::InvalidState);
}

// --- review fixes: remaining engine paths (M7) ---

TEST(LiteProbe, MoreThanTheMaximumStepsIsAHarnessFailure) {
    ScriptedLitePeer peer;
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    for (std::size_t i = 0; i <= scen::kLiteMaximumSteps; ++i) definition.steps.push_back(scen::lite_mark("m"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.harness_failed);
    EXPECT_FALSE(t.harness_failure_reason.empty());
    EXPECT_TRUE(peer.calls().empty());
}

TEST(LiteProbe, TransportEndingsAreClassified) {
    struct Case {
        const char* name;
        transport::TransportEvent event;
        bool harness_failed;
        bool timed_out;
    };
    transport::ConnectionEstablishedEvent second;
    second.alpn = bytes_of("moq-lite-06");
    const std::vector<Case> cases{
        {"second-connection", second, true, false},
        {"transport-error", transport::TransportErrorEvent{}, true, false},
        {"idle-timeout", transport::IdleTimeoutEvent{}, false, true},
        {"local-close-without-step", transport::LocalCloseEvent{}, true, false},
    };
    for (const auto& c : cases) {
        ScriptedLitePeer peer([&c](ScriptedLitePeer& p) {
            if (p.polls() == 3) p.raw_event(c.event);
        });
        ManualLiteClock clock;
        LiteProbeDefinition definition;
        definition.deadline = 100ms;
        const auto t = run_lite_probe(peer, definition, clock);
        EXPECT_EQ(t.harness_failed, c.harness_failed) << c.name;
        EXPECT_EQ(t.timed_out, c.timed_out) << c.name;
        EXPECT_EQ(t.complete, !c.harness_failed) << c.name;
        EXPECT_LT(t.ended_ns, 10 * kMs) << c.name;
        EXPECT_FALSE(judgeable(t)) << c.name;
    }
}

TEST(LiteProbe, APeerThatDoesNotReactIsJudgeableThroughTheObservationWindow) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::IgnoreUnknownStreams;
    ConformingLitePublisher publisher(config);
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_open_bidi(stream_type(0x7), false, "unknown-bidi"));
    definition.observation_window = 50ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_TRUE(judgeable(t));
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    const auto* record = find_stream(t, 1);
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(record->reset_seen) << "the absence of the reset is the observation";
    EXPECT_LT(t.ended_ns, 100 * kMs);
}

// --- the evidence cap and Group payloads (L1e Task 1, item I2) ---

// One whole Group stream of `frames` FRAMEs of `payload` bytes each (STREAM_TYPE 0x0, GROUP, FRAMEs).
Bytes big_group(std::uint64_t sequence, std::size_t frames, std::size_t payload) {
    Bytes bytes = join({stream_type(0x0), group_header({1, sequence, 0})});
    for (std::size_t f = 0; f < frames; ++f) {
        l06::Frame value;
        value.timestamp_delta = 33;
        value.payload = Bytes(payload, static_cast<std::byte>(0x40 + (f & 0x3f)));
        const auto encoded = frame(value);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    return bytes;
}

// The publisher's Setup stream, the answer to the runner's announce request and then 20 MiB of Group streams, each
// written in several chunks: the Group payloads are kept as length and FIN only, so the context stays judgeable,
// while the Setup stream and the bidirectional answer keep their bytes.
TEST(LiteProbe, AGroupPayloadFloodKeepsLengthAndFinButNotTheBytes) {
    constexpr std::size_t kGroups = 20;
    constexpr std::size_t kFrames = 2;
    constexpr std::size_t kPayload = 512 * 1024;
    constexpr std::size_t kChunk = 300 * 1000;
    std::size_t group_bytes = 0;
    std::size_t group_events = 0;
    std::vector<transport::StreamId> group_ids;
    const Bytes publisher_setup = setup_stream(l06::SetupMessage{{{l06::kParamHop, Bytes{std::byte{7}}}}});
    const Bytes answer = announce_ok({7, 0});
    bool answered = false;
    bool flooded = false;
    ScriptedLitePeer peer([&](ScriptedLitePeer& p) {
        if (!flooded) {
            flooded = true;
            p.data(p.open_peer_uni(), publisher_setup, true);
            for (std::size_t g = 0; g < kGroups; ++g) {
                const auto id = p.open_peer_uni();
                group_ids.push_back(id);
                const auto bytes = big_group(g, kFrames, kPayload);
                for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk) {
                    const auto end = std::min(bytes.size(), offset + kChunk);
                    p.data(id, Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(end)),
                           end == bytes.size());
                    group_bytes += end - offset;
                    ++group_events;
                }
            }
        }
        if (!answered && p.runner_stream(1) != nullptr) {
            answered = true;
            p.data(1, answer);
        }
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 2000ms;
    definition.steps.push_back(scen::lite_open_bidi(announce_bytes(), false, "announce"));
    definition.observation_window = 100ms;
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_GE(group_bytes, std::size_t{20} << 20);
    EXPECT_FALSE(t.event_limit_reached) << t.event_limit_reason;
    EXPECT_TRUE(judgeable(t));
    EXPECT_TRUE(scen::judgeable_with_stimulus(t));
    EXPECT_EQ(t.payload_bytes_dropped, group_bytes);
    EXPECT_EQ(t.payload_events_elided, group_events);
    ASSERT_EQ(t.event_data_sizes.size(), t.events.size());
    std::size_t kept_group_fins = 0;
    std::size_t sized_group_bytes = 0;
    Bytes setup_seen;
    Bytes answer_seen;
    for (std::size_t i = 0; i < t.events.size(); ++i) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&t.events[i]);
        if (!data) {
            EXPECT_EQ(t.event_data_sizes[i], 0u);
            continue;
        }
        if (std::find(group_ids.begin(), group_ids.end(), data->stream_id) != group_ids.end()) {
            EXPECT_TRUE(data->data.empty()) << "Group payload bytes are not kept";
            sized_group_bytes += t.event_data_sizes[i];
            if (data->fin) ++kept_group_fins;
        } else {
            EXPECT_EQ(t.event_data_sizes[i], data->data.size()) << "other streams keep their bytes";
            if (data->stream_id == 2) setup_seen.insert(setup_seen.end(), data->data.begin(), data->data.end());
            if (data->stream_id == 1) answer_seen.insert(answer_seen.end(), data->data.begin(), data->data.end());
        }
    }
    EXPECT_EQ(sized_group_bytes, group_bytes);
    EXPECT_EQ(kept_group_fins, kGroups);
    EXPECT_EQ(setup_seen, publisher_setup);
    EXPECT_EQ(answer_seen, answer);
    // The recorder decoded every GROUP and FRAME before the bytes were dropped.
    std::size_t frames = 0;
    for (const auto id : group_ids) {
        const auto* record = find_stream(t, id);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->kind, LiteStreamKind::Group);
        EXPECT_TRUE(record->fin_seen);
        for (const auto* message : sess::peer_messages(*record)) {
            if (const auto* f = std::get_if<l06::Frame>(&message->message)) {
                EXPECT_EQ(f->payload.size(), kPayload);
                ++frames;
            }
        }
    }
    EXPECT_EQ(frames, kGroups * kFrames);
}

// A FETCH response is media too (L2a): 20 MiB of FRAMEs on the runner's Fetch stream keep their length and FIN in
// the transcript but not their bytes, the recorder decodes every FRAME first, and the context stays judgeable. The
// runner's own FETCH request keeps its bytes.
TEST(LiteProbe, AFetchResponseFloodKeepsLengthAndFinButNotTheBytes) {
    constexpr std::size_t kFrames = 40;
    constexpr std::size_t kPayload = 512 * 1024;
    constexpr std::size_t kChunk = 300 * 1000;
    l06::FetchRequest request;
    request.broadcast_path = "demo/live";
    request.track_name = "video";
    const Bytes fetch_bytes = join({stream_type(0x3), fetch_request(request)});
    Bytes response;
    for (std::size_t f = 0; f < kFrames; ++f) {
        l06::Frame value;
        value.timestamp_delta = 2;
        value.payload = Bytes(kPayload, static_cast<std::byte>(0x40 + (f & 0x3f)));
        const auto encoded = frame(value);
        response.insert(response.end(), encoded.begin(), encoded.end());
    }
    std::size_t sent_bytes = 0;
    bool answered = false;
    ScriptedLitePeer peer([&](ScriptedLitePeer& p) {
        if (answered || p.runner_stream(1) == nullptr) return;
        answered = true;
        for (std::size_t offset = 0; offset < response.size(); offset += kChunk) {
            const auto end = std::min(response.size(), offset + kChunk);
            p.data(1, Bytes(response.begin() + static_cast<std::ptrdiff_t>(offset),
                            response.begin() + static_cast<std::ptrdiff_t>(end)),
                   end == response.size());
            sent_bytes += end - offset;
        }
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 2000ms;
    definition.steps.push_back(scen::lite_open_bidi(fetch_bytes, false, "fetch"));
    definition.observation_window = 100ms;
    const auto t = run_lite_probe(peer, definition, clock);
    ASSERT_GE(sent_bytes, std::size_t{20} << 20);
    EXPECT_FALSE(t.event_limit_reached) << t.event_limit_reason;
    EXPECT_TRUE(judgeable(t));
    EXPECT_EQ(t.payload_bytes_dropped, sent_bytes);
    Bytes request_seen;
    std::size_t kept_response_bytes = 0;
    std::size_t sized_response_bytes = 0;
    bool fin_kept = false;
    for (std::size_t i = 0; i < t.events.size(); ++i) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&t.events[i]);
        if (data == nullptr || data->stream_id != 1) continue;
        kept_response_bytes += data->data.size();
        sized_response_bytes += t.event_data_sizes[i];
        fin_kept = fin_kept || data->fin;
    }
    EXPECT_EQ(kept_response_bytes, 0u);
    EXPECT_EQ(sized_response_bytes, sent_bytes);
    EXPECT_TRUE(fin_kept);
    // The runner's request is a step, not a peer event, and keeps its bytes.
    ASSERT_FALSE(t.steps.empty());
    EXPECT_EQ(t.steps[0].bytes, fetch_bytes);
    const auto* record = find_stream(t, 1);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->kind, LiteStreamKind::Fetch);
    EXPECT_EQ(sess::peer_fetch_frames(*record).size(), kFrames);
    EXPECT_TRUE(record->fin_seen);
}

// Only Group streams lose their payload: a flood on a peer Setup stream (or an unknown uni type, above:
// EvidenceByteLimitStopsRecording) still reaches the cap.
TEST(LiteProbe, AFloodOnAPeerSetupStreamStillReachesTheEvidenceCap) {
    bool flooded = false;
    ScriptedLitePeer peer([&](ScriptedLitePeer& p) {
        if (flooded) return;
        flooded = true;
        const auto id = p.open_peer_uni();
        p.data(id, stream_type(0x1));
        Bytes chunk(1u << 20, std::byte{0x7});
        for (std::size_t i = 0; i < kLiteMaximumEvidenceBytes / chunk.size() + 2; ++i) p.data(id, chunk);
    });
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 50ms;
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_TRUE(t.event_limit_reached);
    EXPECT_EQ(t.payload_bytes_dropped, 0u);
    EXPECT_FALSE(judgeable(t));
}

// --- the runner duties (L1e Task 1, item 3) ---

using scen::LiteEngineAction;

// Write calls with FIN the runner made on `stream`.
std::size_t fin_writes(const ScriptedLitePeer& peer, transport::StreamId stream) {
    std::size_t count = 0;
    for (const auto& call : peer.calls())
        if (call.kind == RunnerCall::Kind::Write && call.stream == stream && call.fin) ++count;
    return count;
}

// One runner bidi stream (`bytes`, no FIN), observed for 100 ms, with the send-close duty as given.
LiteProbeDefinition one_request(Bytes bytes, bool duty) {
    LiteProbeDefinition definition;
    definition.id = "duty";
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_open_bidi(std::move(bytes), false, "request"));
    definition.observation_window = 100ms;
    definition.duties.close_send_after_peer_end = duty;
    return definition;
}

// A publisher that ends its answer on runner stream 1 the poll after it saw the request: FIN, reset or both a reset
// and STOP_SENDING.
enum class PeerEnd { Fin, Reset, ResetAndStop };
ScriptedLitePeer::Reaction ending(PeerEnd how, const Bytes& answer) {
    return [how, answer, done = false](ScriptedLitePeer& p) mutable {
        if (done || p.runner_stream(1) == nullptr) return;
        done = true;
        if (how == PeerEnd::Fin) {
            p.data(1, answer, true);
            return;
        }
        p.data(1, answer);
        p.peer_reset(1, 0x1);
        if (how == PeerEnd::ResetAndStop) p.peer_stop_sending(1, 0x1);
    };
}

TEST(LiteProbeDuties, OffByDefault) {
    const LiteProbeDefinition definition;
    EXPECT_FALSE(definition.duties.close_send_after_peer_end);
    EXPECT_FALSE(definition.duties.close_on_webtransport_path);
}

// Duty (a): the publisher FINs (or resets) its announce response; the runner FINs its request side. The engine's
// FIN is an engine action, not a step: the steps, their proof and the stimulus flags are those of the script.
TEST(LiteProbeDuties, ThePublisherEndingAnAnnounceResponseIsAnsweredWithTheRunnersFin) {
    for (const auto how : {PeerEnd::Fin, PeerEnd::Reset}) {
        SCOPED_TRACE(static_cast<int>(how));
        ScriptedLitePeer peer(ending(how, announce_ok({7, 0})));
        ManualLiteClock clock;
        const auto t = run_lite_probe(peer, one_request(announce_bytes(), true), clock);
        EXPECT_TRUE(peer.runner_stream(1)->fin);
        EXPECT_EQ(fin_writes(peer, 1), 1u);
        ASSERT_EQ(t.engine_actions.size(), 1u);
        EXPECT_EQ(t.engine_actions[0].kind, LiteEngineAction::Kind::FinSendAfterPeerEnd);
        EXPECT_EQ(t.engine_actions[0].stream_id, std::optional<transport::StreamId>{1});
        EXPECT_EQ(t.engine_actions[0].status, TransportStatus::Success);
        // The FIN came after the publisher's end (the next poll at the earliest is not required: same poll is fine).
        const auto* record = find_stream(t, 1);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(record->local_fin);
        ASSERT_EQ(t.steps.size(), 1u);
        EXPECT_FALSE(t.steps[0].fin_accepted) << "the step itself did not FIN";
        EXPECT_TRUE(t.stimulus_delivered);
        EXPECT_TRUE(scen::judgeable_with_stimulus(t));
        EXPECT_FALSE(t.runner_closed);
    }
}

// The old behavior through the option: no FIN.
TEST(LiteProbeDuties, WithTheDutyOffTheRunnerLeavesItsSideOpen) {
    ScriptedLitePeer peer(ending(PeerEnd::Fin, announce_ok({7, 0})));
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, one_request(announce_bytes(), false), clock);
    EXPECT_FALSE(peer.runner_stream(1)->fin);
    EXPECT_TRUE(t.engine_actions.empty());
}

// Duty (b): the same for every runner-opened bidirectional stream (here a Subscribe stream).
TEST(LiteProbeDuties, ThePublisherEndingASubscribeResponseIsAnsweredWithTheRunnersFin) {
    for (const auto how : {PeerEnd::Fin, PeerEnd::Reset}) {
        ScriptedLitePeer peer(ending(how, subscribe_response(l06::SubscribeOk{5})));
        ManualLiteClock clock;
        const auto t = run_lite_probe(peer, one_request(subscribe_bytes(), true), clock);
        EXPECT_TRUE(peer.runner_stream(1)->fin);
        ASSERT_EQ(t.engine_actions.size(), 1u);
        EXPECT_EQ(t.engine_actions[0].stream_id, std::optional<transport::StreamId>{1});
    }
}

// A STOP_SENDING beside the reset: the runner may not FIN any more (nothing is written).
TEST(LiteProbeDuties, NoFinAfterThePublishersStopSending) {
    ScriptedLitePeer peer(ending(PeerEnd::ResetAndStop, announce_ok({7, 0})));
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, one_request(announce_bytes(), true), clock);
    EXPECT_FALSE(peer.runner_stream(1)->fin);
    EXPECT_TRUE(t.engine_actions.empty());
}

// The runner already ended its side (a FIN on the opening write, or a reset step): nothing more.
TEST(LiteProbeDuties, NothingWhenTheRunnerAlreadyEndedItsSide) {
    {
        ScriptedLitePeer peer(ending(PeerEnd::Fin, announce_ok({7, 0})));
        ManualLiteClock clock;
        auto definition = one_request(announce_bytes(), true);
        definition.steps[0].fin = true;
        const auto t = run_lite_probe(peer, definition, clock);
        EXPECT_EQ(fin_writes(peer, 1), 1u);
        EXPECT_TRUE(t.engine_actions.empty());
    }
    {
        // The runner resets first; the publisher answers by ending its own side.
        ScriptedLitePeer peer([done = false](ScriptedLitePeer& p) mutable {
            const auto* stream = p.runner_stream(1);
            if (done || stream == nullptr || !stream->reset_code) return;
            done = true;
            p.peer_reset(1, 0x1);
        });
        ManualLiteClock clock;
        auto definition = one_request(announce_bytes(), true);
        definition.steps.push_back(scen::lite_reset(0, 0x1, "reset"));
        const auto t = run_lite_probe(peer, definition, clock);
        EXPECT_EQ(fin_writes(peer, 1), 0u);
        EXPECT_TRUE(t.engine_actions.empty());
        EXPECT_TRUE(t.stimulus_delivered);
    }
}

// A step still to come acts on the stream's send side: the engine defers to it (no write after FIN reaches the
// transport), and its FIN is the step's own, proved as before.
TEST(LiteProbeDuties, APendingStepOnTheStreamComesFirst) {
    ScriptedLitePeer peer(ending(PeerEnd::Fin, announce_ok({7, 0})));
    ManualLiteClock clock;
    auto definition = one_request(announce_bytes(), true);
    definition.steps.push_back(scen::lite_wait(50ms, "later"));
    definition.steps.push_back(scen::lite_fin(0, "script-fin"));
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_EQ(fin_writes(peer, 1), 1u);
    EXPECT_TRUE(t.engine_actions.empty());
    ASSERT_EQ(t.steps.size(), 3u);
    EXPECT_TRUE(t.steps[2].delivered());
    EXPECT_TRUE(t.steps[2].fin_accepted);
    EXPECT_TRUE(t.stimulus_delivered);
}

// A step appended after the engine's FIN that would write the stream is skipped (never a write after FIN), so the
// stimulus is not delivered: NotRun rather than a harness failure.
TEST(LiteProbeDuties, AStepAfterTheEnginesFinIsSkipped) {
    ScriptedLitePeer peer(ending(PeerEnd::Fin, announce_ok({7, 0})));
    ManualLiteClock clock;
    auto definition = one_request(announce_bytes(), true);
    definition.next_steps = [](const LiteSession& session, LiteProbeContext& context) {
        std::vector<LiteStep> more;
        const auto* record = session.find(1);
        if (record == nullptr || !record->local_fin) return more;
        context.finished = true;
        more.push_back(scen::lite_fin(0, "too-late"));
        return more;
    };
    const auto t = run_lite_probe(peer, definition, clock);
    EXPECT_EQ(fin_writes(peer, 1), 1u);
    ASSERT_EQ(t.engine_actions.size(), 1u);
    ASSERT_EQ(t.steps.size(), 2u);
    EXPECT_FALSE(t.steps[1].executed());
    EXPECT_EQ(t.steps[1].skipped_reason, scen::kLiteSendClosedByEngine);
    EXPECT_FALSE(t.harness_failed);
    EXPECT_FALSE(t.stimulus_delivered);
}

// The transport refusing the engine's FIN because of the peer ends the duty without a harness failure.
TEST(LiteProbeDuties, APeerRefusalOfTheEnginesFinIsRecorded) {
    bool armed = false;
    ScriptedLitePeer peer([&armed, inner = ending(PeerEnd::Fin, announce_ok({7, 0}))](ScriptedLitePeer& p) mutable {
        inner(p);
        if (!armed && p.runner_stream(1) != nullptr) {
            armed = true;
            p.forced_status[1] = TransportStatus::PeerStopped;
        }
    });
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, one_request(announce_bytes(), true), clock);
    ASSERT_EQ(t.engine_actions.size(), 1u);
    EXPECT_EQ(t.engine_actions[0].status, TransportStatus::PeerStopped);
    EXPECT_FALSE(t.harness_failed);
    EXPECT_TRUE(judgeable(t));
}

// Duty (c): Path in the publisher's SETUP on WebTransport closes the session with PROTOCOL_VIOLATION.
LiteProbeDefinition path_probe(scen::LiteBinding binding, bool duty) {
    LiteProbeDefinition definition;
    definition.id = "duty-path";
    definition.binding = binding;
    definition.deadline = 1000ms;
    definition.steps.push_back(scen::lite_wait(200ms, "allowance"));
    definition.observation_window = 0ms;
    definition.duties.close_on_webtransport_path = duty;
    return definition;
}

ScriptedLitePeer::Reaction setup_with(std::vector<l06::SetupParameter> parameters) {
    return [parameters, sent = false](ScriptedLitePeer& p) mutable {
        if (sent) return;
        sent = true;
        p.data(p.open_peer_uni(), setup_stream(l06::SetupMessage{parameters}), true);
    };
}

TEST(LiteProbeDuties, APathOnWebTransportClosesTheSessionWithProtocolViolation) {
    const l06::SetupParameter path{l06::kParamPath, bytes_of("/moq?token=l1d")};
    ScriptedLitePeer peer(setup_with({path}));
    ManualLiteClock clock;
    const auto t = run_lite_probe(peer, path_probe(scen::LiteBinding::WebTransport, true), clock);
    ASSERT_TRUE(peer.runner_close().has_value());
    EXPECT_EQ(peer.runner_close()->code, scen::kLiteProtocolViolation);
    EXPECT_EQ(peer.runner_close()->reason, scen::kLiteWebTransportPathCloseReason);
    EXPECT_TRUE(t.runner_closed);
    EXPECT_TRUE(t.runner_closed_for_path);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.timed_out);
    EXPECT_FALSE(t.harness_failed);
    EXPECT_TRUE(judgeable(t));
    EXPECT_FALSE(t.stimulus_delivered) << "the allowance step never ran";
    ASSERT_EQ(t.engine_actions.size(), 1u);
    EXPECT_EQ(t.engine_actions[0].kind, LiteEngineAction::Kind::CloseForWebTransportPath);
    EXPECT_EQ(t.engine_actions[0].code, scen::kLiteProtocolViolation);
    EXPECT_LT(t.ended_ns, 100 * kMs);
}

TEST(LiteProbeDuties, NoCloseWithoutPathOffWebTransportOrWithTheDutyOff) {
    const l06::SetupParameter path{l06::kParamPath, bytes_of("/moq?token=l1d")};
    const l06::SetupParameter hop{l06::kParamHop, Bytes{std::byte{7}}};
    const std::vector<std::tuple<std::vector<l06::SetupParameter>, scen::LiteBinding, bool>> cases{
        {{hop}, scen::LiteBinding::WebTransport, true},
        {{path}, scen::LiteBinding::NativeQuic, true},
        {{path}, scen::LiteBinding::Unknown, true},
        {{path}, scen::LiteBinding::WebTransport, false},
    };
    for (const auto& [parameters, binding, duty] : cases) {
        ScriptedLitePeer peer(setup_with(parameters));
        ManualLiteClock clock;
        const auto t = run_lite_probe(peer, path_probe(binding, duty), clock);
        EXPECT_FALSE(peer.runner_close().has_value());
        EXPECT_FALSE(t.runner_closed);
        EXPECT_FALSE(t.runner_closed_for_path);
        EXPECT_TRUE(t.engine_actions.empty());
        EXPECT_TRUE(t.stimulus_delivered);
    }
}

}  // namespace
