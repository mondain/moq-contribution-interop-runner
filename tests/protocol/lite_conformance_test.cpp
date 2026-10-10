// The end-to-end conformance table (L1d Task 8, extended by L2a): every one of the 26 moq-lite-06 scenarios run against the
// conforming scripted publisher (tests/support/scripted_lite_peer.h, unmodified defaults plus the fixture and the
// session URL), and every one of the 39 evaluators judged on every transcript. A draft-conforming publisher must
// give true on every evaluator's own scenario(s), except the NotRun cases justified below, and NotRun everywhere
// else. A false here is a defect of the evaluator or of the publisher, never something to weaken silently.

#include <gtest/gtest.h>

#include <filesystem>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/lite_evaluators.h"
#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_errors.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/lite_conformance.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace moq::interop::test::lite;
using moq::interop::scenarios::LiteBinding;
using moq::interop::scenarios::LiteTranscript;
namespace s = moq::interop::scenarios;
using moq::interop::requirements::Applicability;
using moq::interop::requirements::CatalogLoadMode;
using moq::interop::requirements::LiteEvaluator;
using moq::interop::requirements::lite_evaluator_registry;
using moq::interop::requirements::load_draft_source;
using moq::interop::requirements::RequirementCatalog;
using moq::interop::requirements::Testability;

using Verdict = std::optional<bool>;

struct Entry {
    std::string id;
    LiteEvaluator evaluate;
    std::set<std::string> scenarios;  // the catalog's scenario lists of the evaluator's row(s)
};

const RequirementCatalog& catalog() {
    static const RequirementCatalog loaded = [] {
        const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
        const auto source = load_draft_source(106, root / "docs", root / "requirements/draft-digests.json");
        return RequirementCatalog::load(source, root / "requirements/moq-lite-06.json",
                                        CatalogLoadMode::AllowIncomplete);
    }();
    return loaded;
}

// Evaluator id -> function -> the scenarios it judges, derived from requirements/moq-lite-06.json (every
// reviewed Applicable + Testable row's scenarios x evaluators) and lite_evaluator_registry(), so a catalog edit
// cannot drift from the table silently.
const std::vector<Entry>& evaluators() {
    static const std::vector<Entry> table = [] {
        std::map<std::string, std::set<std::string>> scenarios;
        for (const auto& row : catalog().requirements) {
            if (!row.reviewed || row.applicability != Applicability::Applicable ||
                row.testability != Testability::Testable)
                continue;
            for (const auto& evaluator : row.evaluators)
                scenarios[evaluator].insert(row.scenarios.begin(), row.scenarios.end());
        }
        std::vector<Entry> out;
        for (const auto& [id, judged] : scenarios) {
            const auto found = lite_evaluator_registry().find(id);
            if (found == lite_evaluator_registry().end()) throw std::logic_error("unregistered evaluator " + id);
            out.push_back({id, found->second, judged});
        }
        return out;
    }();
    return table;
}

// The documented NotRun cases of a conforming publisher, per binding: (evaluator id, scenario id) -> reason.
std::map<std::pair<std::string_view, std::string_view>, std::string_view> expected_not_run(LiteBinding binding) {
    std::map<std::pair<std::string_view, std::string_view>, std::string_view> out{
        // Row 152 passes only after a retraction (ANNOUNCE_END) was seen within the window; a conforming publisher
        // whose broadcast stays up the whole session never retracts, so there is nothing to judge (catalog
        // rationale: "no retraction in the window is NotRun, never a Pass").
        {{"l06-announce-retired-id-unused", "l06-announce-lifecycle"}, "no retraction within the window"},
        // Row 075 is judged only for a publisher that advertised no Probe capability; the conforming publisher
        // advertises Report (row 072 is judged instead), so the level None row has nothing to judge.
        {{"l06-probe-none-reset", "l06-probe-report"}, "the publisher advertises Probe level Report"},
    };
    // Rows 124/120 are restricted to native QUIC, row 125 to WebTransport (draft 7.3.2); an Unknown binding
    // cannot be judged by any of the three.
    if (binding != LiteBinding::NativeQuic) {
        out[{"l06-setup-path-sent", "l06-setup-client-path"}] = "row 124 is judged on native QUIC only";
        out[{"l06-setup-path-query-appended", "l06-setup-client-path"}] = "row 120 is judged on native QUIC only";
    }
    if (binding != LiteBinding::WebTransport) {
        out[{"l06-setup-path-absent-on-uri-binding", "l06-setup-client-path"}] =
            "row 125 is judged on WebTransport only";
    }
    // Row 126 is specified on native QUIC (binding 1): NotRun on WebTransport (Task 4 ruling).
    if (binding == LiteBinding::WebTransport) {
        out[{"l06-setup-server-path-close", "l06-setup-server-path"}] = "row 126 is not judged on WebTransport";
    }
    return out;
}

std::string verdict_name(const Verdict& v) { return !v ? "NotRun" : (*v ? "Pass" : "Fail"); }

// Judges every transcript with every evaluator against the expectation (the verdict is printed on a mismatch).
void check_table(const std::vector<LiteTranscript>& transcripts, LiteBinding binding) {
    const auto not_run = expected_not_run(binding);
    for (const auto& t : transcripts) {
        SCOPED_TRACE(t.scenario_id);
        ASSERT_TRUE(t.established);
        EXPECT_TRUE(t.complete);
        EXPECT_FALSE(t.timed_out);
        EXPECT_FALSE(t.harness_failed) << t.harness_failure_reason;
        EXPECT_FALSE(t.event_limit_reached) << t.event_limit_reason;
        EXPECT_TRUE(s::judgeable(t));
        // A conforming publisher leaves no incomplete message buffered on any stream.
        for (const auto& stream : t.streams) EXPECT_EQ(stream.peer_pending_bytes, 0u) << stream.stream_id;
        for (const auto& entry : evaluators()) {
            const auto verdict = entry.evaluate(t);
            const bool own = entry.scenarios.contains(t.scenario_id);
            const bool documented = not_run.contains({entry.id, t.scenario_id});
            if (!own || documented) {
                EXPECT_EQ(verdict, Verdict{})
                    << entry.id << " on " << t.scenario_id << ": " << verdict_name(verdict);
            } else {
                EXPECT_EQ(verdict, Verdict{true})
                    << entry.id << " on " << t.scenario_id << ": " << verdict_name(verdict);
            }
        }
    }
}

TEST(LiteConformance, TheTableCoversEveryScenarioAndEvaluator) {
    EXPECT_EQ(evaluators().size(), 39u);
    std::set<std::string_view> ids;
    std::set<std::string_view> scenarios;
    for (const auto& entry : evaluators()) {
        ids.insert(entry.id);
        scenarios.insert(entry.scenarios.begin(), entry.scenarios.end());
    }
    EXPECT_EQ(ids.size(), 39u);
    EXPECT_EQ(scenarios.size(), 26u);
    std::set<std::string> probed;
    for (const auto& probe : conformance_probes(LiteBinding::NativeQuic)) probed.insert(probe.id);
    EXPECT_EQ(probed.size(), 26u);
    for (const auto scenario : scenarios) EXPECT_TRUE(probed.contains(std::string(scenario))) << scenario;
}

TEST(LiteConformance, EveryScenarioOnNativeQuic) {
    check_table(conformance_transcripts(LiteBinding::NativeQuic), LiteBinding::NativeQuic);
}

// The client-path scenario once per binding (the other bindings' rows NotRun); the remaining scenarios are
// binding-independent except 126 (NotRun on WebTransport), checked here too.
TEST(LiteConformance, EveryScenarioOnWebTransport) {
    check_table(conformance_transcripts(LiteBinding::WebTransport), LiteBinding::WebTransport);
}

TEST(LiteConformance, EveryScenarioOnAnUnknownBinding) {
    check_table(conformance_transcripts(LiteBinding::Unknown), LiteBinding::Unknown);
}

// The table is not vacuous: each behavior the Task 8 fixture update added, switched back to the earlier fixture's
// behavior, costs exactly the verdicts the ledger predicted (the earlier fixture gave 023 Fail, 159/172 NotRun,
// 020 Fail, 030/032/033 NotRun, and no open Group stream for the row 098 check).
TEST(LiteConformance, TheFixtureBehaviorsTheTableDependsOn) {
    const auto binding = LiteBinding::NativeQuic;
    const auto probe = [&](std::string_view id) {
        for (auto& definition : conformance_probes(binding))
            if (definition.id == id) return definition;
        throw std::logic_error("no probe " + std::string(id));
    };
    const auto run = [&](std::string_view id, const std::function<void(ConformingLitePublisherConfig&)>& tweak) {
        return run_conforming(probe(id), binding, tweak);
    };
    // Offset reading of Group Start: the floors resolve one group early, the reading conflict (NotRun).
    const auto offset = run(s::kL06SubscribeGroupFloor,
                            [](auto& config) { config.defect = LiteDefect::OffsetGroupStart; });
    EXPECT_EQ(s::evaluate_l06_subscribe_no_group_below_floor(offset), Verdict{});
    EXPECT_EQ(s::evaluate_l06_subscribe_ok_group_at_floor(offset), Verdict{});
    // Closing the session on the invalid-bounds SUBSCRIBE instead of resetting it: 023 Fail.
    const auto closing = run(s::kL06SubscribeInvalidFrameBounds,
                             [](auto& config) { config.defect = LiteDefect::CloseOnInvalidSubscribe; });
    EXPECT_EQ(s::evaluate_l06_subscribe_invalid_frame_bounds_reset(closing), Verdict{false});
    // No echo of the runner's FIN: 025 Fail (not closed within the allowance).
    const auto silent = run(s::kL06SessionStreamClose, [](auto& config) { config.echo_runner_fin = false; });
    EXPECT_EQ(s::evaluate_l06_session_peer_closes_send(silent), Verdict{false});
    // Every Group stream sent whole: the row 098 STOP_SENDING finds no open Group stream (its gate expires).
    const auto whole = run(s::kL06ErrorsUnknownResetCode, [](auto& config) { config.keep_live_group_open = false; });
    const auto* stop = s::lite06::step_labelled(whole, s::kL06StopGroupLabel);
    ASSERT_NE(stop, nullptr);
    EXPECT_TRUE(stop->gate_expired);
    // Without Path on native QUIC (the earlier fixture never sent one): 124 Fail, 120 NotRun.
    const auto no_path = run(s::kL06SetupClientPath, [](auto& config) { config.session_url_path.clear(); });
    EXPECT_EQ(s::evaluate_l06_setup_path_sent(no_path), Verdict{false});
    EXPECT_EQ(s::evaluate_l06_setup_path_query_appended(no_path), Verdict{});
}

// The publisher's own observable choices behind the verdicts above (so a later fixture change that keeps the
// verdicts by accident is noticed).
TEST(LiteConformance, PublisherBehaviorBehindTheTable) {
    const auto transcripts = conformance_transcripts(LiteBinding::NativeQuic);
    std::map<std::string, const LiteTranscript*> by_id;
    for (const auto& t : transcripts) by_id[t.scenario_id] = &t;
    ASSERT_EQ(by_id.size(), 26u);
    // The unknown-reset-code probe really stopped an OPEN Group stream of A (the row 098 note's check ran).
    const auto* stop = s::lite06::step_labelled(*by_id.at(std::string(s::kL06ErrorsUnknownResetCode)),
                                                s::kL06StopGroupLabel);
    ASSERT_NE(stop, nullptr);
    EXPECT_TRUE(stop->executed());
    EXPECT_FALSE(stop->gate_expired);
    // The code-space probe's SUBSCRIBE was refused before the length request (its `refused` gate opened).
    const auto* refused = s::lite06::step_labelled(*by_id.at(std::string(s::kL06ErrorsCodeSpace)), s::kL06RefusedLabel);
    ASSERT_NE(refused, nullptr);
    EXPECT_FALSE(refused->gate_expired);
    // The abutting probe reached its second subscription.
    const auto& abutting = *by_id.at(std::string(s::kL06SubscribeAbuttingFrameStart));
    EXPECT_NE(s::lite06::step_labelled(abutting, s::kL06AbuttingSecondLabel), nullptr);
    // The floor probe sent both floored subscriptions.
    const auto& floor = *by_id.at(std::string(s::kL06SubscribeGroupFloor));
    EXPECT_NE(s::lite06::step_labelled(floor, s::kL06FloorAtLatestLabel), nullptr);
    EXPECT_NE(s::lite06::step_labelled(floor, s::kL06FloorAboveLabel), nullptr);
}

// --- media-sized Group payloads (L1e Task 1, item I2) --------------------------------------------------------------

// The subscribe-latest scenario against a publisher whose groups carry more than 20 MiB of FRAME payload in all:
// before L1e the payload bytes counted against kLiteMaximumEvidenceBytes (8 MiB), set event_limit_reached and lost
// every verdict of the context. Now they are kept as length and FIN only and the group rows are still judged.
TEST(LiteConformance, AMediaSizedGroupFloodIsStillJudged) {
    const auto binding = LiteBinding::NativeQuic;
    std::optional<s::LiteProbeDefinition> probe;
    for (auto& definition : conformance_probes(binding))
        if (definition.id == s::kL06SubscribeLatest) probe = std::move(definition);
    ASSERT_TRUE(probe.has_value());
    const auto t = run_conforming(std::move(*probe), binding, [](ConformingLitePublisherConfig& config) {
        config.frames_per_group = 8;
        config.frame_payload_bytes = 900 * 1000;
    });
    EXPECT_FALSE(t.event_limit_reached) << t.event_limit_reason;
    EXPECT_TRUE(s::judgeable(t));
    EXPECT_GE(t.payload_bytes_dropped, std::size_t{20} << 20);
    for (const auto& entry : evaluators()) {
        if (!entry.scenarios.contains(t.scenario_id)) continue;
        EXPECT_EQ(entry.evaluate(t), Verdict{true}) << entry.id;
    }
}

// --- the runner duties (L1e Task 1, item 3) ------------------------------------------------------------------------

// Every evaluator on every transcript, keyed (evaluator, scenario).
std::map<std::pair<std::string, std::string>, Verdict> verdict_table(const std::vector<LiteTranscript>& transcripts) {
    std::map<std::pair<std::string, std::string>, Verdict> out;
    for (const auto& t : transcripts)
        for (const auto& entry : evaluators()) out[{entry.id, t.scenario_id}] = entry.evaluate(t);
    return out;
}

// The 26 scenarios on `binding` with the duties as the builders set them (on) or switched off.
std::vector<LiteTranscript> table_with_duties(LiteBinding binding, bool duties,
                                              const std::function<void(ConformingLitePublisherConfig&)>& tweak = {}) {
    std::vector<LiteTranscript> out;
    for (auto& probe : conformance_probes(binding)) {
        if (!duties) probe.duties = {};
        out.push_back(run_conforming(std::move(probe), binding, tweak));
    }
    return out;
}

std::size_t engine_actions(const std::vector<LiteTranscript>& transcripts) {
    std::size_t count = 0;
    for (const auto& t : transcripts) count += t.engine_actions.size();
    return count;
}

TEST(LiteConformanceDuties, EveryProductionBuilderTurnsTheDutiesOn) {
    for (const auto binding : {LiteBinding::NativeQuic, LiteBinding::WebTransport, LiteBinding::Unknown}) {
        for (const auto& probe : conformance_probes(binding)) {
            EXPECT_TRUE(probe.duties.close_send_after_peer_end) << probe.id;
            EXPECT_TRUE(probe.duties.close_on_webtransport_path) << probe.id;
            EXPECT_TRUE(probe.duties.reset_on_zero_timescale) << probe.id;
            EXPECT_TRUE(probe.duties.reset_on_undecodable_frames) << probe.id;
            EXPECT_TRUE(probe.duties.close_on_goaway_uri) << probe.id;
        }
    }
}

// The table (all 39 evaluators, every scenario, every binding) gives the same verdicts with the duties on and off.
// The L1 scenarios never fire a duty against the conforming publisher (it never ends a runner stream first except
// with a STOP_SENDING, and sends no Path on WebTransport). The track and fetch scenarios do: the publisher answers a
// Track Stream with TRACK_INFO and FIN, and a Fetch Stream with its frames and FIN (draft 5.1.3, 5.1.4), so the
// runner closes its own send direction (draft 4.3). Those FINs are no step, so no verdict moves.
TEST(LiteConformanceDuties, TheTableIsTheSameWithTheDutiesOnAndOff) {
    for (const auto binding : {LiteBinding::NativeQuic, LiteBinding::WebTransport, LiteBinding::Unknown}) {
        SCOPED_TRACE(static_cast<int>(binding));
        const auto on = table_with_duties(binding, true);
        const auto off = table_with_duties(binding, false);
        // Track lookups: l06-track-info 2, l06-fetch-group 1, l06-fetch-unknown-group 1; Fetch Streams: 3 and 0
        // (the unknown group is reset, not finished); the goaway scenario's subscription is never finished.
        std::map<std::string, std::size_t> fired;
        for (const auto& t : on) {
            if (!t.engine_actions.empty()) fired[t.scenario_id] = t.engine_actions.size();
        }
        EXPECT_EQ(fired, (std::map<std::string, std::size_t>{{"l06-track-info", 2},
                                                              {"l06-fetch-group", 4},
                                                              {"l06-fetch-unknown-group", 1}}));
        EXPECT_EQ(engine_actions(off), 0u);
        check_table(on, binding);
        EXPECT_EQ(verdict_table(on), verdict_table(off));
    }
}

// A publisher that ends every announce answer (FIN right after ANNOUNCE_OK and its ANNOUNCE_STARTs) and every
// served subscription (FIN right after SUBSCRIBE_OK) itself: the send-close duty fires, and no verdict of the 39
// evaluators on the 26 scenarios differs from the duties-off run (the engine's FINs are no step: no stimulus proof,
// no allowance and no same-poll ordering sees them).
TEST(LiteConformanceDuties, TheDutiesFiringChangeNoVerdict) {
    const auto ends_answers = [](ConformingLitePublisherConfig& config) {
        config.hooks.on_request = [](ConformingLitePublisher& publisher, ScriptedLitePeer& peer,
                                     const LiteRunnerRequest& request) {
            if (const auto* announce = std::get_if<l06::AnnounceRequest>(&request.message)) {
                publisher.answer_announce(peer, request.stream, *announce);
                publisher.fin_answer(peer, request.stream);
                return true;
            }
            if (const auto* subscribe = std::get_if<l06::Subscribe>(&request.message)) {
                publisher.answer_subscribe(peer, request.stream, *subscribe);
                publisher.fin_answer(peer, request.stream);
                return true;
            }
            return false;
        };
    };
    for (const auto binding : {LiteBinding::NativeQuic, LiteBinding::WebTransport}) {
        SCOPED_TRACE(static_cast<int>(binding));
        const auto on = table_with_duties(binding, true, ends_answers);
        const auto off = table_with_duties(binding, false, ends_answers);
        EXPECT_GT(engine_actions(on), 0u) << "the duty fired";
        for (const auto& t : on) {
            EXPECT_FALSE(t.harness_failed) << t.scenario_id << ": " << t.harness_failure_reason;
            for (const auto& action : t.engine_actions)
                EXPECT_EQ(action.kind, s::LiteEngineAction::Kind::FinSendAfterPeerEnd) << t.scenario_id;
        }
        const auto with = verdict_table(on);
        const auto without = verdict_table(off);
        for (const auto& [key, verdict] : without)
            EXPECT_EQ(with.at(key), verdict) << key.first << " on " << key.second << ": on "
                                             << verdict_name(with.at(key)) << ", off " << verdict_name(verdict);
    }
}

// Duty (c): a publisher sending Path on WebTransport. The runner closes the session with PROTOCOL_VIOLATION as soon
// as it decodes the SETUP; row 125's evaluator still judges the Path it observed (Fail), as it did with the duty off
// after the allowance. A conforming WebTransport publisher (no Path) is not closed and passes.
TEST(LiteConformanceDuties, Row125IsStillJudgedWhenTheRunnerClosedForThePath) {
    const auto binding = LiteBinding::WebTransport;
    const auto client_path = [&] {
        for (auto& definition : conformance_probes(binding))
            if (definition.id == s::kL06SetupClientPath) return definition;
        throw std::logic_error("no client-path probe");
    };
    // The publisher believes it is on native QUIC, so it sends Path.
    const auto sends_path = [](ConformingLitePublisherConfig& config) { config.binding = LiteBinding::NativeQuic; };
    const auto closed = run_conforming(client_path(), binding, sends_path);
    ASSERT_TRUE(closed.runner_closed_for_path);
    EXPECT_TRUE(closed.runner_closed);
    EXPECT_FALSE(closed.stimulus_delivered);
    EXPECT_TRUE(s::judgeable(closed));
    ASSERT_EQ(closed.engine_actions.size(), 1u);
    EXPECT_EQ(closed.engine_actions[0].kind, s::LiteEngineAction::Kind::CloseForWebTransportPath);
    EXPECT_EQ(s::evaluate_l06_setup_path_absent_on_uri_binding(closed), Verdict{false});
    auto off = client_path();
    off.duties = {};
    const auto open = run_conforming(std::move(off), binding, sends_path);
    EXPECT_FALSE(open.runner_closed);
    EXPECT_EQ(s::evaluate_l06_setup_path_absent_on_uri_binding(open), Verdict{false});
    // Rows 120/124 stay native-only (NotRun) either way.
    EXPECT_EQ(s::evaluate_l06_setup_path_sent(closed), Verdict{});
    EXPECT_EQ(s::evaluate_l06_setup_path_query_appended(closed), Verdict{});
    const auto conforming = run_conforming(client_path(), binding);
    EXPECT_FALSE(conforming.runner_closed);
    EXPECT_TRUE(conforming.engine_actions.empty());
    EXPECT_EQ(s::evaluate_l06_setup_path_absent_on_uri_binding(conforming), Verdict{true});
}

// The Path close in every other scenario: the runner closed before the stimulus ran, so no evaluator that needs
// the stimulus or the allowance judges (NotRun, never a Pass or a Fail of the publisher for a session the runner
// ended). The two that read the publisher's SETUP bytes themselves still judge them: row 125 (the Path, Fail) and
// row 111 (Parameter ID uniqueness of that same SETUP, Pass here; its gate is judgeable() plus the runner's Setup,
// as with the duty off, where it gives the same Pass after the allowance).
TEST(LiteConformanceDuties, ThePathCloseLeavesEveryOtherRowNotRun) {
    const auto binding = LiteBinding::WebTransport;
    const auto sends_path = [](ConformingLitePublisherConfig& config) { config.binding = LiteBinding::NativeQuic; };
    const auto on = table_with_duties(binding, true, sends_path);
    const auto off = verdict_table(table_with_duties(binding, false, sends_path));
    for (const auto& t : on) {
        SCOPED_TRACE(t.scenario_id);
        EXPECT_TRUE(t.runner_closed_for_path);
        for (const auto& entry : evaluators()) {
            const auto verdict = entry.evaluate(t);
            if (entry.id == "l06-setup-path-absent-on-uri-binding" && t.scenario_id == s::kL06SetupClientPath) {
                EXPECT_EQ(verdict, Verdict{false});
            } else if (entry.id == "l06-setup-parameters-unique" && t.scenario_id == s::kL06SetupStream) {
                EXPECT_EQ(verdict, Verdict{true});
                EXPECT_EQ(off.at({entry.id, t.scenario_id}), Verdict{true});
            } else {
                EXPECT_EQ(verdict, Verdict{}) << entry.id;
            }
        }
    }
}

// --- the fixture's SUBSCRIBE decode handling -----------------------------------------------------------------------

// A probe opening one SUBSCRIBE stream with `bytes` (no FIN), observed for 500 ms.
s::LiteProbeDefinition one_subscribe(std::vector<std::byte> bytes) {
    s::LiteProbeDefinition definition;
    definition.id = "fixture-subscribe";
    definition.deadline = std::chrono::milliseconds(2000);
    definition.steps.push_back(s::lite_open_bidi(std::move(bytes), false, "subscribe"));
    definition.observation_window = std::chrono::milliseconds(500);
    return definition;
}

const moq::interop::session::LiteStreamRecord* subscribe_stream(const LiteTranscript& t) {
    const auto* step = s::lite06::step_labelled(t, "subscribe");
    return step && step->stream_id ? s::lite06::find_stream(t, *step->stream_id) : nullptr;
}

// The bounds rule (Frame End without Group End, draft 3.6) alone: the SUBSCRIBE stream is reset, the session stays.
TEST(ConformingLitePublisherSubscribe, TheBoundsViolationIsResetAndTheSessionStaysOpen) {
    const auto t = run_conforming(
        one_subscribe(s::l06_invalid_bounds_subscribe_bytes(kConformanceBroadcast, kConformanceTrack)),
        LiteBinding::NativeQuic);
    EXPECT_FALSE(t.peer_close.has_value());
    const auto* record = subscribe_stream(t);
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->reset_seen);
    EXPECT_EQ(record->reset_code, std::optional<std::uint64_t>(0x0));
}

// Any other undecodable SUBSCRIBE (here a Message Length of 0: no Subscribe ID) closes the session with
// PROTOCOL_VIOLATION (draft 7.1), and nothing answers the stream first.
TEST(ConformingLitePublisherSubscribe, AnyOtherUndecodableSubscribeClosesTheSession) {
    for (const auto& bytes : {std::vector<std::byte>{std::byte{0x02}, std::byte{0x00}},
                              std::vector<std::byte>{std::byte{0x02}, std::byte{0x02}, std::byte{0x00},
                                                     std::byte{0x05}}}) {
        const auto t = run_conforming(one_subscribe(bytes), LiteBinding::NativeQuic);
        ASSERT_TRUE(t.peer_close.has_value());
        EXPECT_EQ(t.peer_close->code, 0x3u);
        const auto* record = subscribe_stream(t);
        ASSERT_NE(record, nullptr);
        EXPECT_FALSE(record->reset_seen);
    }
    // The named defect closes on the bounds violation too.
    const auto closing = run_conforming(
        one_subscribe(s::l06_invalid_bounds_subscribe_bytes(kConformanceBroadcast, kConformanceTrack)),
        LiteBinding::NativeQuic, [](auto& config) { config.defect = LiteDefect::CloseOnInvalidSubscribe; });
    ASSERT_TRUE(closing.peer_close.has_value());
    EXPECT_EQ(closing.peer_close->code, 0x3u);
}

// A malformed runner SETUP (a repeated Parameter ID) still closes the session with PROTOCOL_VIOLATION.
TEST(ConformingLitePublisherSubscribe, AMalformedSetupStillClosesTheSession) {
    const auto t =
        run_conforming(s::l06_setup_duplicate_parameter_probe(kConformanceDeadline), LiteBinding::NativeQuic);
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
}

}  // namespace
