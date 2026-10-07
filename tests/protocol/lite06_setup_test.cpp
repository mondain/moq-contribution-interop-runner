// The moq-lite-06 setup scenarios (L1d Task 4): every evaluator against the conforming scripted publisher and
// against publishers that violate exactly one rule, plus the NotRun conditions of the catalog rows.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using namespace moq::interop::test::lite;
using moq::interop::scenarios::evaluate_l06_errors_code_space;
using moq::interop::scenarios::judgeable;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
namespace scen = moq::interop::scenarios;
namespace lite06 = moq::interop::scenarios::lite06;
namespace transport = moq::interop::transport;
namespace wire = moq::interop::wire;

using Verdict = std::optional<bool>;
using Evaluator = Verdict (*)(const LiteTranscript&);

constexpr auto kTick = 10ms;
constexpr auto kDeadline = 10000ms;

Bytes value(std::initializer_list<int> bytes) {
    Bytes out;
    for (const auto b : bytes) out.push_back(static_cast<std::byte>(b));
    return out;
}

// Runs `definition` against a ConformingLitePublisher built from `config`; `tweak` adjusts the peer first.
LiteTranscript run(LiteProbeDefinition definition, ConformingLitePublisherConfig config = {},
                   const std::function<void(ScriptedLitePeer&)>& tweak = {}) {
    ConformingLitePublisher publisher(std::move(config));
    ScriptedLitePeer peer(publisher.reaction());
    if (tweak) tweak(peer);
    ManualLiteClock clock;
    return run_lite_probe(peer, std::move(definition), clock, kTick);
}

ConformingLitePublisherConfig with_hooks(LitePublisherHooks hooks) {
    ConformingLitePublisherConfig config;
    config.hooks = std::move(hooks);
    return config;
}

ConformingLitePublisherConfig with_defect(LiteDefect defect) {
    ConformingLitePublisherConfig config;
    config.defect = defect;
    return config;
}

bool is_runner_setup(const LiteRunnerRequest& request) { return !request.bidirectional && request.stream_type == 0x1; }

// Two distinct, well-formed parameters (Cost 1, Hop 7), so row 111 has something to judge.
std::vector<l06::SetupParameter> two_parameters() { return {{l06::kParamCost, value({1})}, {l06::kParamHop, value({7})}}; }

ConformingLitePublisherConfig two_parameter_publisher() {
    ConformingLitePublisherConfig config;
    config.setup_parameters = two_parameters();
    return config;
}

// A publisher whose own Setup stream is replaced by `bytes` (with or without FIN), nothing else changed.
ConformingLitePublisherConfig own_setup(Bytes bytes, bool fin = true) {
    LitePublisherHooks hooks;
    hooks.on_start = [bytes = std::move(bytes), fin](ConformingLitePublisher&, ScriptedLitePeer& peer) {
        peer.data(peer.open_peer_uni(), bytes, fin);
        return true;
    };
    auto config = with_hooks(std::move(hooks));
    config.setup_parameters = two_parameters();
    return config;
}

// --- shared helpers -------------------------------------------------------------------------------------------

TEST(Lite06Common, ErrorCodeTablesMatchTheDraft) {
    // Draft 4.4.1 Table 2 and 4.4.2 Table 3 (pinned against the text in moqlite06_wire_audit_test.cpp).
    const std::vector<std::uint64_t> session{0x0, 0x1, 0x2, 0x3, 0x6, 0x10, 0x11, 0x15};
    const std::vector<std::uint64_t> stream{0x0,  0x1,  0x2,  0x3,  0x4,  0x5,  0x12, 0x30, 0x31,
                                            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39};
    EXPECT_EQ(std::vector<std::uint64_t>(std::begin(lite06::kSessionErrorCodes), std::end(lite06::kSessionErrorCodes)),
              session);
    EXPECT_EQ(std::vector<std::uint64_t>(std::begin(lite06::kStreamErrorCodes), std::end(lite06::kStreamErrorCodes)),
              stream);
    for (std::uint64_t code = 0; code < 0x80; ++code) {
        EXPECT_EQ(lite06::is_session_code(code), std::find(session.begin(), session.end(), code) != session.end());
        EXPECT_EQ(lite06::is_stream_code(code), std::find(stream.begin(), stream.end(), code) != stream.end());
    }
}

TEST(Lite06Common, RawSetupStreamKeepsRepeatedIds) {
    const auto bytes = lite06::raw_setup_stream({{0x4, value({1})}, {0x4, value({1})}});
    // STREAM_TYPE 1, Message Length 7, Count 2, (4,1,1), (4,1,1).
    EXPECT_EQ(bytes, value({0x01, 0x07, 0x02, 0x04, 0x01, 0x01, 0x04, 0x01, 0x01}));
    // decode_setup refuses it, which is why the probe builds it raw.
    wire::Cursor cursor(std::span<const std::byte>(bytes).subspan(1));
    EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(l06::decode_setup(cursor)));
    // Without a repetition it is exactly what encode_setup writes.
    EXPECT_EQ(lite06::raw_setup_stream(two_parameters()), setup_stream({two_parameters()}));
    EXPECT_EQ(lite06::raw_setup_stream({}), scen::lite_default_runner_setup());
}

TEST(Lite06Common, PeerSetupIsReadLenientlyFromThePeerOnly) {
    // The runner's own Setup stream is never taken for the publisher's.
    auto t = run(scen::l06_setup_stream_probe(kDeadline), with_defect(LiteDefect::NoSetupStream));
    ASSERT_TRUE(t.runner_setup.delivered());
    EXPECT_FALSE(lite06::peer_setup_message(t).has_value());

    t = run(scen::l06_setup_stream_probe(kDeadline), own_setup(lite06::raw_setup_stream({{0x4, value({1})}, {0x4, value({2})}})));
    const auto setup = lite06::peer_setup_message(t);
    ASSERT_TRUE(setup.has_value());
    EXPECT_EQ(setup->setup_streams, 1u);
    EXPECT_TRUE(setup->fin);
    EXPECT_FALSE(setup->malformed);
    EXPECT_FALSE(setup->trailing);
    ASSERT_TRUE(setup->message.has_value());
    ASSERT_EQ(setup->message->parameters.size(), 2u);
    EXPECT_EQ(setup->message->parameters[0].id, 0x4u);
    EXPECT_EQ(setup->message->parameters[1].id, 0x4u);
}

TEST(Lite06Common, CloseAndResetCodes) {
    LiteTranscript t;
    EXPECT_FALSE(lite06::session_close_code(t).has_value());
    t.peer_close = moq::interop::session::PeerCloseInfo{transport::CloseErrorSpace::Application, 0x3, {}, 0};
    EXPECT_EQ(lite06::session_close_code(t), 0x3u);
    t.peer_close->space = transport::CloseErrorSpace::Transport;
    EXPECT_FALSE(lite06::session_close_code(t).has_value());

    auto record = moq::interop::session::make_lite_stream_record(7, moq::interop::session::LiteOrigin::Runner, false);
    record.reset_seen = true;
    record.reset_code = 0x33;
    t.streams.push_back(record);
    EXPECT_EQ(lite06::stream_reset_code(t, 7), 0x33u);
    EXPECT_FALSE(lite06::stream_reset_code(t, 3).has_value());
}

TEST(Lite06Common, ProvedStimulusNeedsFullDelivery) {
    auto t = run(scen::l06_setup_duplicate_stream_probe(kDeadline));
    ASSERT_NE(lite06::proved_stimulus(t, lite06::kRunnerSetupLabel), nullptr);
    ASSERT_NE(lite06::proved_stimulus(t, scen::kL06SecondSetupLabel), nullptr);
    EXPECT_NE(lite06::proved_stimulus(t, lite06::kRunnerSetupLabel, scen::lite_default_runner_setup()), nullptr);
    EXPECT_EQ(lite06::proved_stimulus(t, lite06::kRunnerSetupLabel, value({0x01, 0x01})), nullptr);
    EXPECT_EQ(lite06::proved_stimulus(t, "no-such-step"), nullptr);
    // A runner Setup stream the peer refused (stopped) is not a delivered stimulus.
    t.runner_setup.refused = transport::TransportStatus::PeerStopped;
    EXPECT_EQ(lite06::proved_stimulus(t, lite06::kRunnerSetupLabel), nullptr);
    t.runner_setup.refused.reset();
    t.runner_setup.fin_accepted = false;
    EXPECT_EQ(lite06::proved_stimulus(t, lite06::kRunnerSetupLabel), nullptr);
}

TEST(Lite06Common, ProbeBuildersRejectADeadlineWithinTheAllowance) {
    EXPECT_THROW(scen::l06_setup_stream_probe(1000ms, 2000ms), std::invalid_argument);
    EXPECT_THROW(scen::l06_setup_duplicate_parameter_probe(3000ms), std::invalid_argument);
    EXPECT_NO_THROW(scen::l06_setup_server_role_probe(3001ms));
}

TEST(Lite06Common, ProbeStimuli) {
    EXPECT_EQ(scen::l06_setup_stream_probe(kDeadline).runner_setup, scen::lite_default_runner_setup());
    EXPECT_EQ(scen::l06_duplicate_parameter_runner_setup(),
              lite06::raw_setup_stream({{l06::kParamCost, value({1})}, {l06::kParamCost, value({1})}}));
    EXPECT_EQ(scen::l06_unknown_parameter_runner_setup(), setup_stream({{{0x7f, bytes_of("l1d")}}}));
    EXPECT_EQ(scen::l06_server_path_runner_setup(), setup_stream({{{l06::kParamPath, bytes_of("/")}}}));
    EXPECT_EQ(scen::l06_server_role_runner_setup(), setup_stream({{{l06::kParamRole, value({0})}}}));
    const auto stream = scen::l06_setup_duplicate_stream_probe(kDeadline);
    ASSERT_EQ(stream.steps.size(), 2u);
    EXPECT_EQ(stream.steps[0].label, scen::kL06SecondSetupLabel);
    EXPECT_EQ(stream.steps[0].bytes, scen::lite_default_runner_setup());
    EXPECT_TRUE(stream.steps[0].fin);
    const auto unknown = scen::l06_setup_unknown_parameter_probe(kDeadline);
    ASSERT_EQ(unknown.steps.size(), 2u);
    EXPECT_EQ(unknown.steps[0].label, scen::kL06AnnounceLabel);
    EXPECT_EQ(unknown.steps[0].bytes, scen::lite_announce_stream_bytes({""}));
    // Every probe ends on its allowance step: a Wait whose execution proves the allowance elapsed.
    for (const auto& probe : {scen::l06_setup_stream_probe(kDeadline), unknown, stream,
                              scen::l06_setup_duplicate_parameter_probe(kDeadline),
                              scen::l06_setup_server_path_probe(kDeadline), scen::l06_setup_server_role_probe(kDeadline)}) {
        ASSERT_FALSE(probe.steps.empty()) << probe.id;
        EXPECT_EQ(probe.steps.back().kind, scen::LiteStep::Kind::Wait) << probe.id;
        EXPECT_EQ(probe.steps.back().label, scen::kL06AllowanceLabel) << probe.id;
    }
    for (const auto& probe : {scen::l06_setup_stream_probe(kDeadline), unknown, stream,
                              scen::l06_setup_duplicate_parameter_probe(kDeadline),
                              scen::l06_setup_server_path_probe(kDeadline), scen::l06_setup_server_role_probe(kDeadline)})
        EXPECT_FALSE(probe.requires_track) << probe.id;
}

// --- l06-setup-stream: 014 and 111 ----------------------------------------------------------------------------

struct StreamVerdicts {
    Verdict single;
    Verdict unique;
};
StreamVerdicts stream_verdicts(const LiteTranscript& t) {
    return {scen::evaluate_l06_setup_stream_single_setup(t), scen::evaluate_l06_setup_parameters_unique(t)};
}

TEST(Lite06SetupStream, ConformingPublisherPassesBoth) {
    const auto t = run(scen::l06_setup_stream_probe(kDeadline), two_parameter_publisher());
    ASSERT_TRUE(judgeable(t));
    EXPECT_EQ(t.scenario_id, scen::kL06SetupStream);
    const auto v = stream_verdicts(t);
    EXPECT_EQ(v.single, true);
    EXPECT_EQ(v.unique, true);
    // The probe ended on the publisher's FIN, well before the allowance.
    EXPECT_LT(t.ended_ns - t.established_ns, 1000000000u);
}

TEST(Lite06SetupStream, EmptyParameterListPassesSingleSetupAndLeavesUniquenessNotRun) {
    const auto v = stream_verdicts(run(scen::l06_setup_stream_probe(kDeadline)));
    EXPECT_EQ(v.single, true);
    EXPECT_EQ(v.unique, std::nullopt);  // fewer than two parameters: no vacuous Pass
}

TEST(Lite06SetupStream, OneParameterLeavesUniquenessNotRun) {
    auto config = two_parameter_publisher();
    config.setup_parameters.pop_back();
    const auto v = stream_verdicts(run(scen::l06_setup_stream_probe(kDeadline), config));
    EXPECT_EQ(v.single, true);
    EXPECT_EQ(v.unique, std::nullopt);
}

TEST(Lite06SetupStream, RepeatedIdFailsOnlyUniqueness) {
    const auto t = run(scen::l06_setup_stream_probe(kDeadline),
                       own_setup(lite06::raw_setup_stream({{0x4, value({1})}, {0x5, value({7})}, {0x4, value({1})}})));
    ASSERT_TRUE(judgeable(t));
    const auto v = stream_verdicts(t);
    EXPECT_EQ(v.single, true);  // framing intact: a decoded SETUP for row 014
    EXPECT_EQ(v.unique, false);
}

TEST(Lite06SetupStream, MissingFinFailsOnlySingleSetup) {
    const auto t = run(scen::l06_setup_stream_probe(kDeadline), own_setup(setup_stream({two_parameters()}), false));
    ASSERT_TRUE(judgeable(t));
    EXPECT_FALSE(t.timed_out);  // the allowance ended it, not the deadline
    const auto v = stream_verdicts(t);
    EXPECT_EQ(v.single, false);
    EXPECT_EQ(v.unique, true);
}

TEST(Lite06SetupStream, SecondSetupOnTheStreamFailsOnlySingleSetup) {
    const auto t = run(scen::l06_setup_stream_probe(kDeadline),
                       own_setup(join({setup_stream({two_parameters()}), setup({two_parameters()})})));
    const auto v = stream_verdicts(t);
    EXPECT_EQ(v.single, false);
    EXPECT_EQ(v.unique, true);
}

TEST(Lite06SetupStream, TwoSetupStreamsFailOnlySingleSetup) {
    LitePublisherHooks hooks;
    hooks.on_start = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        publisher.send_setup(peer);
        publisher.send_setup(peer);
        return true;
    };
    auto config = with_hooks(std::move(hooks));
    config.setup_parameters = two_parameters();
    const auto v = stream_verdicts(run(scen::l06_setup_stream_probe(kDeadline), config));
    EXPECT_EQ(v.single, false);
    EXPECT_EQ(v.unique, true);
}

TEST(Lite06SetupStream, ResetSetupStreamFailsOnlySingleSetup) {
    LitePublisherHooks hooks;
    hooks.on_start = [](ConformingLitePublisher&, ScriptedLitePeer& peer) {
        const auto id = peer.open_peer_uni();
        peer.data(id, setup_stream({two_parameters()}), false);
        peer.peer_reset(id, 0x0);
        return true;
    };
    auto config = with_hooks(std::move(hooks));
    config.setup_parameters = two_parameters();
    const auto v = stream_verdicts(run(scen::l06_setup_stream_probe(kDeadline), config));
    EXPECT_EQ(v.single, false);
    EXPECT_EQ(v.unique, true);
}

TEST(Lite06SetupStream, BrokenFramingFailsSingleSetupAndLeavesUniquenessNotRun) {
    // Parameter Count 2 but one parameter in the body.
    const auto t = run(scen::l06_setup_stream_probe(kDeadline), own_setup(value({0x01, 0x04, 0x02, 0x04, 0x01, 0x01})));
    const auto v = stream_verdicts(t);
    EXPECT_EQ(v.single, false);
    EXPECT_EQ(v.unique, std::nullopt);
}

TEST(Lite06SetupStream, NoSetupStreamFailsSingleSetupWithinTheAllowance) {
    const auto t = run(scen::l06_setup_stream_probe(kDeadline), with_defect(LiteDefect::NoSetupStream));
    ASSERT_TRUE(judgeable(t));
    EXPECT_FALSE(t.timed_out);
    EXPECT_GE(t.ended_ns - t.established_ns, 2000000000u);
    const auto v = stream_verdicts(t);
    EXPECT_EQ(v.single, false);
    EXPECT_EQ(v.unique, std::nullopt);
}

// --- l06-setup-unknown-parameter: 110 --------------------------------------------------------------------------

TEST(Lite06SetupUnknownParameter, IgnoringPublisherPasses) {
    const auto t = run(scen::l06_setup_unknown_parameter_probe(kDeadline));
    ASSERT_TRUE(judgeable(t));
    EXPECT_FALSE(t.peer_close.has_value());
    EXPECT_EQ(scen::evaluate_l06_setup_unknown_parameter_ignored(t), true);
}

TEST(Lite06SetupUnknownParameter, TheRunnerSetupCarriesTheUnknownId) {
    ConformingLitePublisher publisher;
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    run_lite_probe(peer, scen::l06_setup_unknown_parameter_probe(kDeadline), clock, kTick);
    const l06::SetupMessage* setup = nullptr;
    for (const auto& request : publisher.requests())
        if (is_runner_setup(request)) setup = std::get_if<l06::SetupMessage>(&request.message);
    ASSERT_NE(setup, nullptr);
    ASSERT_EQ(setup->parameters.size(), 1u);
    EXPECT_EQ(setup->parameters[0].id, scen::kL06UnknownParameterId);
}

TEST(Lite06SetupUnknownParameter, ClosingOnTheUnknownIdFails) {
    LitePublisherHooks hooks;
    hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        const auto* setup = std::get_if<l06::SetupMessage>(&request.message);
        if (!setup) return false;
        for (const auto& parameter : setup->parameters)
            if (parameter.id == scen::kL06UnknownParameterId) publisher.close(peer, 0x3);
        return true;
    };
    const auto t = run(scen::l06_setup_unknown_parameter_probe(kDeadline), with_hooks(std::move(hooks)));
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(scen::evaluate_l06_setup_unknown_parameter_ignored(t), false);
}

TEST(Lite06SetupUnknownParameter, ClosingLaterInTheWindowFails) {
    LitePublisherHooks hooks;
    auto polls = std::make_shared<int>(0);
    hooks.on_poll = [polls](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        if (++*polls == 150) publisher.close(peer, 0x0);
    };
    const auto t = run(scen::l06_setup_unknown_parameter_probe(kDeadline), with_hooks(std::move(hooks)));
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(scen::evaluate_l06_setup_unknown_parameter_ignored(t), false);
}

TEST(Lite06SetupUnknownParameter, RefusingTheRequestFails) {
    LitePublisherHooks hooks;
    hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!std::holds_alternative<l06::AnnounceRequest>(request.message)) return false;
        publisher.refuse(peer, request.stream, 0x0);
        return true;
    };
    const auto t = run(scen::l06_setup_unknown_parameter_probe(kDeadline), with_hooks(std::move(hooks)));
    EXPECT_FALSE(t.peer_close.has_value());
    EXPECT_EQ(scen::evaluate_l06_setup_unknown_parameter_ignored(t), false);
}

TEST(Lite06SetupUnknownParameter, DefectsOfOtherRowsAreNotRun) {
    // Silence on the request and a missing publisher SETUP are judged by their own rows, not as row 110 Fails.
    EXPECT_EQ(scen::evaluate_l06_setup_unknown_parameter_ignored(
                  run(scen::l06_setup_unknown_parameter_probe(kDeadline), with_defect(LiteDefect::SilentOnAnnounce))),
              std::nullopt);
    EXPECT_EQ(scen::evaluate_l06_setup_unknown_parameter_ignored(
                  run(scen::l06_setup_unknown_parameter_probe(kDeadline), with_defect(LiteDefect::NoSetupStream))),
              std::nullopt);
}

// --- the four named close probes: 112, 092, 126, 131 (+ 027) -------------------------------------------------

struct CloseProbe {
    const char* name;
    std::function<LiteProbeDefinition()> probe;
    Evaluator evaluator;
};

std::vector<CloseProbe> close_probes() {
    return {
        {"duplicate-parameter", [] { return scen::l06_setup_duplicate_parameter_probe(kDeadline); },
         &scen::evaluate_l06_setup_duplicate_parameter_close},
        {"duplicate-stream", [] { return scen::l06_setup_duplicate_stream_probe(kDeadline); },
         &scen::evaluate_l06_setup_duplicate_stream_close},
        {"server-path", [] { return scen::l06_setup_server_path_probe(kDeadline); },
         &scen::evaluate_l06_setup_server_path_close},
        {"server-role", [] { return scen::l06_setup_server_role_probe(kDeadline); },
         &scen::evaluate_l06_setup_server_role_close},
    };
}

// Every evaluator of this task, for the "nothing else is judged" assertions.
std::vector<Evaluator> all_evaluators() {
    return {&scen::evaluate_l06_setup_stream_single_setup,       &scen::evaluate_l06_setup_parameters_unique,
            &scen::evaluate_l06_setup_unknown_parameter_ignored, &scen::evaluate_l06_setup_duplicate_parameter_close,
            &scen::evaluate_l06_setup_duplicate_stream_close,    &scen::evaluate_l06_setup_server_path_close,
            &scen::evaluate_l06_setup_server_role_close,         &evaluate_l06_errors_code_space};
}

// A publisher that does nothing about any runner Setup stream (it neither closes nor refuses).
ConformingLitePublisherConfig ignores_runner_setup() {
    LitePublisherHooks hooks;
    hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer&, const LiteRunnerRequest& request) {
        return is_runner_setup(request);
    };
    return with_hooks(std::move(hooks));
}

TEST(Lite06CloseProbes, ConformingPublisherClosesWithProtocolViolation) {
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe());
        ASSERT_TRUE(judgeable(t)) << probe.name;
        ASSERT_TRUE(t.peer_close.has_value()) << probe.name;
        EXPECT_EQ(t.peer_close->code, 0x3u) << probe.name;
        EXPECT_EQ(probe.evaluator(t), true) << probe.name;
        EXPECT_EQ(evaluate_l06_errors_code_space(t), true) << probe.name;
    }
}

TEST(Lite06CloseProbes, EachEvaluatorJudgesOnlyItsOwnScenario) {
    const auto probes = close_probes();
    for (const auto& probe : probes) {
        const auto t = run(probe.probe());
        for (const auto& other : probes) {
            if (other.evaluator != probe.evaluator) {
                EXPECT_EQ(other.evaluator(t), std::nullopt) << probe.name;
            }
        }
        EXPECT_EQ(scen::evaluate_l06_setup_stream_single_setup(t), std::nullopt) << probe.name;
        EXPECT_EQ(scen::evaluate_l06_setup_parameters_unique(t), std::nullopt) << probe.name;
        EXPECT_EQ(scen::evaluate_l06_setup_unknown_parameter_ignored(t), std::nullopt) << probe.name;
    }
}

TEST(Lite06CloseProbes, StillOpenAtTheEndOfTheAllowanceFails) {
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe(), ignores_runner_setup());
        ASSERT_TRUE(judgeable(t)) << probe.name;
        EXPECT_FALSE(t.timed_out) << probe.name;
        EXPECT_FALSE(t.peer_close.has_value()) << probe.name;
        EXPECT_GE(t.ended_ns - t.established_ns, 3000000000u) << probe.name;
        EXPECT_EQ(probe.evaluator(t), false) << probe.name;
        EXPECT_EQ(evaluate_l06_errors_code_space(t), std::nullopt) << probe.name;  // no close: no code to judge
    }
}

TEST(Lite06CloseProbes, StreamLevelReactionAloneFails) {
    LitePublisherHooks hooks;
    hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!is_runner_setup(request)) return false;
        peer.peer_stop_sending(request.stream, 0x0);
        return true;
    };
    const auto config = with_hooks(std::move(hooks));
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe(), config);
        ASSERT_TRUE(judgeable(t)) << probe.name;
        EXPECT_EQ(probe.evaluator(t), false) << probe.name;
        EXPECT_EQ(evaluate_l06_errors_code_space(t), std::nullopt) << probe.name;
    }
}

TEST(Lite06CloseProbes, StreamOnlyCloseCodeFailsTheProbeAndTheCodeSpace) {
    // NOT_FOUND (0x33) exists only in the stream table: the catalog's documented 027 attribution.
    auto config = ConformingLitePublisherConfig{};
    config.protocol_violation_code = 0x33;
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe(), config);
        ASSERT_TRUE(t.peer_close.has_value()) << probe.name;
        EXPECT_EQ(probe.evaluator(t), false) << probe.name;
        EXPECT_EQ(evaluate_l06_errors_code_space(t), false) << probe.name;
    }
}

TEST(Lite06CloseProbes, AnotherSessionCodeFailsOnlyTheProbe) {
    for (const std::uint64_t code : {0x0, 0x1, 0x6, 0x15}) {
        auto config = ConformingLitePublisherConfig{};
        config.protocol_violation_code = code;
        for (const auto& probe : close_probes()) {
            const auto t = run(probe.probe(), config);
            EXPECT_EQ(probe.evaluator(t), false) << probe.name << " " << code;
            EXPECT_EQ(evaluate_l06_errors_code_space(t), true) << probe.name << " " << code;
        }
    }
}

TEST(Lite06CloseProbes, UnregisteredCloseCodeFailsTheProbeAndIsNotJudgedForItsSpace) {
    auto config = ConformingLitePublisherConfig{};
    config.protocol_violation_code = 0x40;
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe(), config);
        EXPECT_EQ(probe.evaluator(t), false) << probe.name;
        EXPECT_EQ(evaluate_l06_errors_code_space(t), std::nullopt) << probe.name;
    }
}

TEST(Lite06CloseProbes, TransportSpaceCloseFailsTheProbeAndIsNotJudgedForItsSpace) {
    LitePublisherHooks hooks;
    hooks.on_request = [](ConformingLitePublisher&, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!is_runner_setup(request)) return false;
        if (request.setup_streams_seen > 1 || request.message.index() == 0 ||
            !std::get<l06::SetupMessage>(request.message).parameters.empty())
            peer.close_session(0x3, "", transport::CloseErrorSpace::Transport);
        return true;
    };
    const auto config = with_hooks(std::move(hooks));
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe(), config);
        ASSERT_TRUE(t.peer_close.has_value()) << probe.name;
        EXPECT_EQ(probe.evaluator(t), false) << probe.name;
        EXPECT_EQ(evaluate_l06_errors_code_space(t), std::nullopt) << probe.name;
    }
}

TEST(Lite06CloseProbes, SessionOnlyCodeOnAStreamTerminationFailsOnlyTheCodeSpace) {
    // The publisher stops the offending Setup stream with GOAWAY_TIMEOUT (0x10, session table only), then closes
    // correctly: one defect, row 027 only.
    LitePublisherHooks hooks;
    hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!is_runner_setup(request)) return false;
        const bool offending = request.setup_streams_seen > 1 || request.message.index() == 0 ||
                               !std::get<l06::SetupMessage>(request.message).parameters.empty();
        if (!offending) return true;
        peer.peer_stop_sending(request.stream, 0x10);
        publisher.close(peer, 0x3);
        return true;
    };
    const auto config = with_hooks(std::move(hooks));
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe(), config);
        EXPECT_EQ(probe.evaluator(t), true) << probe.name;
        EXPECT_EQ(evaluate_l06_errors_code_space(t), false) << probe.name;
    }
}

TEST(Lite06CloseProbes, CloseBeforeTheStimulusIsNotRun) {
    // The publisher closes as soon as the connection is up, before the runner's Setup stream is written.
    LitePublisherHooks hooks;
    hooks.on_start = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer) {
        publisher.close(peer, 0x3);
        return true;
    };
    const auto config = with_hooks(std::move(hooks));
    for (const auto& probe : close_probes()) {
        const auto t = run(probe.probe(), config);
        ASSERT_TRUE(t.peer_close.has_value()) << probe.name;
        EXPECT_TRUE(t.peer_closed_early) << probe.name;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << probe.name;
    }
}

TEST(Lite06CloseProbes, DuplicateStreamCloseBeforeTheSecondStreamIsNotRun) {
    // A close right after the first runner Setup stream, before the second one went out, was not provoked by it.
    LitePublisherHooks hooks;
    hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer, const LiteRunnerRequest& request) {
        if (!is_runner_setup(request)) return false;
        if (request.setup_streams_seen == 1) publisher.close(peer, 0x3);
        return true;
    };
    ConformingLitePublisher publisher(with_hooks(std::move(hooks)));
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    auto definition = scen::l06_setup_duplicate_stream_probe(kDeadline);
    definition.steps[0].delay = 50ms;  // the publisher sees the first Setup stream alone
    const auto t = run_lite_probe(peer, std::move(definition), clock, kTick);
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(lite06::proved_stimulus(t, scen::kL06SecondSetupLabel), nullptr);
    EXPECT_EQ(scen::evaluate_l06_setup_duplicate_stream_close(t), std::nullopt);
    // The close itself still carries a session code, judged for its space only.
    EXPECT_EQ(evaluate_l06_errors_code_space(t), true);
}

// --- NotRun conditions common to all scenarios -----------------------------------------------------------------

std::vector<std::function<LiteProbeDefinition()>> every_probe() {
    std::vector<std::function<LiteProbeDefinition()>> probes{
        [] { return scen::l06_setup_stream_probe(kDeadline); },
        [] { return scen::l06_setup_unknown_parameter_probe(kDeadline); }};
    for (const auto& probe : close_probes()) probes.push_back(probe.probe);
    return probes;
}

TEST(Lite06SetupNotRun, EmptyTranscriptIsNeverAPass) {
    const LiteTranscript empty;
    for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(empty), std::nullopt);
    // A transcript carrying only the scenario id is still empty.
    for (const auto& probe : every_probe()) {
        LiteTranscript t;
        t.scenario_id = probe().id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << t.scenario_id;
    }
}

TEST(Lite06SetupNotRun, PublisherThatNeverConnects) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.connect_deadline = 100ms;
        const auto t = run(std::move(definition), {}, [](ScriptedLitePeer& peer) { peer.establish_on_poll.reset(); });
        EXPECT_FALSE(t.established);
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << t.scenario_id;
    }
}

TEST(Lite06SetupNotRun, HarnessFailure) {
    for (const auto& probe : every_probe()) {
        ConformingLitePublisher publisher;
        ScriptedLitePeer peer(publisher.reaction(), "moqt-22");
        ManualLiteClock clock;
        const auto t = run_lite_probe(peer, probe(), clock, kTick);
        ASSERT_TRUE(t.harness_failed) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << t.scenario_id;
    }
}

TEST(Lite06SetupNotRun, HarnessFlagOnAnOtherwisePassingTranscript) {
    for (const auto& probe : every_probe()) {
        auto t = run(probe(), two_parameter_publisher());
        t.harness_failed = true;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << t.scenario_id;
    }
}

TEST(Lite06SetupNotRun, EventLimit) {
    for (const auto& probe : every_probe()) {
        auto definition = probe();
        definition.limits.max_bytes = 4;
        const auto t = run(std::move(definition), two_parameter_publisher());
        ASSERT_TRUE(t.event_limit_reached) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << t.scenario_id;
    }
}

TEST(Lite06SetupNotRun, TimedOutTranscript) {
    for (const auto& probe : every_probe()) {
        auto t = run(probe(), two_parameter_publisher());
        t.timed_out = true;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << t.scenario_id;
    }
}

TEST(Lite06SetupNotRun, RunnerSetupRefusedByThePeer) {
    // The publisher stops the runner's Setup stream before it is written: the stimulus never arrived.
    for (const auto& probe : every_probe()) {
        const auto t = run(probe(), {}, [](ScriptedLitePeer& peer) {
            peer.forced_status[3] = transport::TransportStatus::PeerStopped;
        });
        ASSERT_TRUE(t.runner_setup.refused.has_value()) << t.scenario_id;
        for (const auto evaluator : all_evaluators()) EXPECT_EQ(evaluator(t), std::nullopt) << t.scenario_id;
    }
}

}  // namespace
