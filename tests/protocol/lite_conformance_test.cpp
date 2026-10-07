// The end-to-end conformance table (L1d Task 8): every one of the 19 moq-lite-06 scenarios run against the
// conforming scripted publisher (tests/support/scripted_lite_peer.h, unmodified defaults plus the fixture and the
// session URL), and every one of the 30 evaluators judged on every transcript. A draft-conforming publisher must
// give true on every evaluator's own scenario(s), except the NotRun cases justified below, and NotRun everywhere
// else. A false here is a defect of the evaluator or of the publisher, never something to weaken silently.

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

using Verdict = std::optional<bool>;
using Evaluator = Verdict (*)(const LiteTranscript&);

struct Entry {
    std::string_view id;
    Evaluator evaluate;
    std::set<std::string_view> scenarios;  // the catalog's scenario list of the evaluator's row(s)
};

// Evaluator id -> function -> the scenarios it judges (requirements/moq-lite-06.json).
const std::vector<Entry>& evaluators() {
    static const std::vector<Entry> table{
        {"l06-setup-stream-single-setup", s::evaluate_l06_setup_stream_single_setup, {"l06-setup-stream"}},
        {"l06-setup-parameters-unique", s::evaluate_l06_setup_parameters_unique, {"l06-setup-stream"}},
        {"l06-setup-unknown-parameter-ignored", s::evaluate_l06_setup_unknown_parameter_ignored,
         {"l06-setup-unknown-parameter"}},
        {"l06-setup-duplicate-parameter-close", s::evaluate_l06_setup_duplicate_parameter_close,
         {"l06-setup-duplicate-parameter"}},
        {"l06-setup-duplicate-stream-close", s::evaluate_l06_setup_duplicate_stream_close,
         {"l06-setup-duplicate-stream"}},
        {"l06-setup-server-path-close", s::evaluate_l06_setup_server_path_close, {"l06-setup-server-path"}},
        {"l06-setup-server-role-close", s::evaluate_l06_setup_server_role_close, {"l06-setup-server-role"}},
        {"l06-errors-code-space", s::evaluate_l06_errors_code_space,
         {"l06-errors-code-space", "l06-setup-duplicate-stream", "l06-setup-duplicate-parameter",
          "l06-setup-server-path", "l06-setup-server-role"}},
        {"l06-announce-ok-then-starts", s::evaluate_l06_announce_ok_then_starts, {"l06-announce-prefix"}},
        {"l06-announce-ok-hop-assigned", s::evaluate_l06_announce_ok_hop_assigned, {"l06-announce-prefix"}},
        {"l06-announce-hop-list-excludes-own", s::evaluate_l06_announce_hop_list_excludes_own,
         {"l06-announce-prefix"}},
        {"l06-announce-retired-id-unused", s::evaluate_l06_announce_retired_id_unused, {"l06-announce-lifecycle"}},
        {"l06-session-peer-closes-send", s::evaluate_l06_session_peer_closes_send, {"l06-session-stream-close"}},
        {"l06-group-starts-with-group", s::evaluate_l06_group_starts_with_group, {"l06-subscribe-latest"}},
        {"l06-group-unique-sequence", s::evaluate_l06_group_unique_sequence, {"l06-subscribe-latest"}},
        {"l06-group-sequence-increments", s::evaluate_l06_group_sequence_increments, {"l06-subscribe-latest"}},
        {"l06-subscribe-refused-reset", s::evaluate_l06_subscribe_refused_reset, {"l06-subscribe-refused"}},
        {"l06-subscribe-invalid-frame-bounds-reset", s::evaluate_l06_subscribe_invalid_frame_bounds_reset,
         {"l06-subscribe-invalid-frame-bounds"}},
        {"l06-subscribe-no-group-below-floor", s::evaluate_l06_subscribe_no_group_below_floor,
         {"l06-subscribe-group-floor"}},
        {"l06-subscribe-ok-group-at-floor", s::evaluate_l06_subscribe_ok_group_at_floor,
         {"l06-subscribe-group-floor"}},
        {"l06-subscribe-resolved-start", s::evaluate_l06_subscribe_resolved_start,
         {"l06-subscribe-abutting-frame-start"}},
        {"l06-errors-unknown-stream-type-reset", s::evaluate_l06_errors_unknown_stream_type_reset,
         {"l06-errors-unknown-stream-type"}},
        {"l06-errors-unknown-stream-type-not-fatal", s::evaluate_l06_errors_unknown_stream_type_not_fatal,
         {"l06-errors-unknown-stream-type"}},
        {"l06-errors-unknown-code-tolerated", s::evaluate_l06_errors_unknown_code_tolerated,
         {"l06-errors-unknown-reset-code"}},
        {"l06-errors-no-assumed-unauthorized", s::evaluate_l06_errors_no_assumed_unauthorized,
         {"l06-errors-unknown-reset-code"}},
        {"l06-errors-reserved-code-tolerated", s::evaluate_l06_errors_reserved_code_tolerated,
         {"l06-errors-reserved-reset-code"}},
        {"l06-errors-message-length-close", s::evaluate_l06_errors_message_length_close, {"l06-errors-code-space"}},
        {"l06-setup-path-sent", s::evaluate_l06_setup_path_sent, {"l06-setup-client-path"}},
        {"l06-setup-path-query-appended", s::evaluate_l06_setup_path_query_appended, {"l06-setup-client-path"}},
        {"l06-setup-path-absent-on-uri-binding", s::evaluate_l06_setup_path_absent_on_uri_binding,
         {"l06-setup-client-path"}},
    };
    return table;
}

// The documented NotRun cases of a conforming publisher, per binding: (evaluator id, scenario id) -> reason.
std::map<std::pair<std::string_view, std::string_view>, std::string_view> expected_not_run(LiteBinding binding) {
    std::map<std::pair<std::string_view, std::string_view>, std::string_view> out{
        // Row 152 passes only after a retraction (ANNOUNCE_END) was seen within the window; a conforming publisher
        // whose broadcast stays up the whole session never retracts, so there is nothing to judge (catalog
        // rationale: "no retraction in the window is NotRun, never a Pass").
        {{"l06-announce-retired-id-unused", "l06-announce-lifecycle"}, "no retraction within the window"},
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
    EXPECT_EQ(evaluators().size(), 30u);
    std::set<std::string_view> ids;
    std::set<std::string_view> scenarios;
    for (const auto& entry : evaluators()) {
        ids.insert(entry.id);
        scenarios.insert(entry.scenarios.begin(), entry.scenarios.end());
    }
    EXPECT_EQ(ids.size(), 30u);
    EXPECT_EQ(scenarios.size(), 19u);
    std::set<std::string> probed;
    for (const auto& probe : conformance_probes(LiteBinding::NativeQuic)) probed.insert(probe.id);
    EXPECT_EQ(probed.size(), 19u);
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
    ASSERT_EQ(by_id.size(), 19u);
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

}  // namespace
