// The moq-lite-06 bindings, evaluator registry, evaluate_lite and the staged audit/score integration (L1d Task 8).
// Every expectation about rows, scenarios and evaluators is derived from requirements/moq-lite-06.json through the
// loaded catalog, never from the code under test.
#include "moq/interop/app/lite_scenarios.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/execution_audit.h"
#include "moq/interop/requirements/lite_evaluators.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/lite_conformance.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace moq::interop::requirements {
namespace {

using scenarios::LiteBinding;
using scenarios::LiteTranscript;
namespace lite = test::lite;

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

const RequirementCatalog& catalog() {
    static const RequirementCatalog loaded = [] {
        const auto source = load_draft_source(106, kRoot / "docs", kRoot / "requirements/draft-digests.json");
        return RequirementCatalog::load(source, kRoot / "requirements/moq-lite-06.json",
                                        CatalogLoadMode::AllowIncomplete);
    }();
    return loaded;
}

bool scored(const Requirement& row) {
    return row.reviewed && row.applicability == Applicability::Applicable && row.testability == Testability::Testable;
}
bool required(const Requirement& row) { return row.strength == Strength::Must || row.strength == Strength::MustNot; }

const Requirement& row(std::string_view id) {
    for (const auto& r : catalog().requirements)
        if (r.id == id) return r;
    throw std::logic_error("no row " + std::string(id));
}

std::map<std::string, OutcomeState> by_id(const std::vector<Outcome>& outcomes) {
    std::map<std::string, OutcomeState> out;
    for (const auto& outcome : outcomes) out[outcome.requirement_id] = outcome.state;
    return out;
}

// The conformance transcripts (all 19 scenarios on native QUIC), computed once.
const std::vector<LiteTranscript>& conforming() {
    static const auto transcripts = lite::conformance_transcripts(LiteBinding::NativeQuic);
    return transcripts;
}

std::vector<LiteTranscript> without(std::vector<LiteTranscript> transcripts, std::string_view scenario) {
    std::erase_if(transcripts, [&](const LiteTranscript& t) { return t.scenario_id == scenario; });
    return transcripts;
}

std::vector<LiteTranscript> replaced(std::vector<LiteTranscript> transcripts, const LiteTranscript& replacement) {
    for (auto& t : transcripts)
        if (t.scenario_id == replacement.scenario_id) t = replacement;
    return transcripts;
}

const LiteTranscript& transcript_of(std::string_view scenario) {
    for (const auto& t : conforming())
        if (t.scenario_id == scenario) return t;
    throw std::logic_error("no transcript " + std::string(scenario));
}

// A run of `scenario` against the conforming publisher with one configuration change.
LiteTranscript tweaked(std::string_view scenario,
                       const std::function<void(test::lite::ConformingLitePublisherConfig&)>& tweak) {
    for (auto& definition : lite::conformance_probes(LiteBinding::NativeQuic))
        if (definition.id == scenario)
            return lite::run_conforming(std::move(definition), LiteBinding::NativeQuic, tweak);
    throw std::logic_error("no probe " + std::string(scenario));
}

// The unknown-stream-type scenario against a publisher that neither resets nor stops the unknown stream (108 Fail).
LiteTranscript ignoring_unknown_streams() {
    return tweaked("l06-errors-unknown-stream-type",
                   [](auto& config) { config.defect = test::lite::LiteDefect::IgnoreUnknownStreams; });
}

// The rows a conforming native QUIC run leaves NotRun (justified in tests/protocol/lite_conformance_test.cpp):
// 152 (no retraction within the window) and 125 (judged on WebTransport only).
const std::set<std::string> kConformingNotRun{"L06-7-7-MUST-NOT-152", "L06-7-3-2-MUST-NOT-125"};

// --- registry and bindings ---------------------------------------------------------------------------------------

TEST(LiteEvaluators, RegistryHoldsExactlyTheCatalogEvaluators) {
    std::set<std::string> expected;
    for (const auto& r : catalog().requirements) expected.insert(r.evaluators.begin(), r.evaluators.end());
    ASSERT_EQ(expected.size(), 30u);
    std::set<std::string> registered;
    for (const auto& [id, evaluator] : lite_evaluator_registry()) {
        registered.insert(id);
        EXPECT_TRUE(static_cast<bool>(evaluator)) << id;
    }
    EXPECT_EQ(registered, expected);
}

TEST(LiteEvaluators, BindingsAreTheCatalogScenarioEvaluatorProduct) {
    using Key = std::tuple<std::string, std::string, std::string>;
    std::set<Key> expected;
    for (const auto& r : catalog().requirements) {
        if (!scored(r)) continue;
        for (const auto& scenario : r.scenarios)
            for (const auto& evaluator : r.evaluators) expected.emplace(r.id, scenario, evaluator);
    }
    // 29 single-scenario rows plus row 027 on its five scenarios.
    EXPECT_EQ(expected.size(), 34u);
    const auto bindings = lite_executable_bindings();
    std::set<Key> bound;
    const std::set<std::string> allowed{"raw_probe_stimulus", "raw_probe_transport_event", "peer_close",
                                        "lite_stream_opened", "lite_message",       "lite_decode_error"};
    for (const auto& binding : bindings) {
        EXPECT_EQ(binding.draft, 106u);
        EXPECT_TRUE(bound.emplace(binding.requirement_id, binding.scenario_id, binding.evaluator_id).second)
            << "duplicate " << binding.requirement_id;
        EXPECT_FALSE(binding.evidence_kinds.empty()) << binding.requirement_id;
        for (const auto& kind : binding.evidence_kinds) EXPECT_TRUE(allowed.contains(kind)) << kind;
        EXPECT_TRUE(lite_evaluator_registry().contains(binding.evaluator_id)) << binding.evaluator_id;
        EXPECT_TRUE(app::executable_scenario(106, binding.scenario_id)) << binding.scenario_id;
    }
    EXPECT_EQ(bindings.size(), expected.size());
    EXPECT_EQ(bound, expected);
}

TEST(LiteEvaluators, StagedAuditOfTheRealCatalogCoversEveryRequiredRow) {
    const auto bindings = lite_executable_bindings();
    const auto report = audit_completeness_staged(catalog(), bindings, app::executable_scenarios(106));
    for (const auto& finding : report.findings)
        EXPECT_FALSE(finding.blocking) << finding.code << " " << finding.requirement_id << " " << finding.detail;
    EXPECT_EQ(report.required_total, 26u);
    EXPECT_EQ(report.required_covered, 26u);
    EXPECT_EQ(report.optional_total, 4u);
    EXPECT_EQ(report.optional_covered, 4u);
    EXPECT_EQ(report.unreviewed_total, 75u);
    EXPECT_FALSE(report.complete());
    // No row is left uncovered: only the unreviewed-rows finding remains.
    ASSERT_EQ(report.findings.size(), 1u);
    EXPECT_EQ(report.findings.front().code, "unreviewed_rows");
}

TEST(LiteEvaluators, ABindingNamingAnUnreviewedRowStillBlocks) {
    const auto unreviewed = std::find_if(catalog().requirements.begin(), catalog().requirements.end(),
                                         [](const Requirement& r) { return !r.reviewed; });
    ASSERT_NE(unreviewed, catalog().requirements.end());
    auto bindings = lite_executable_bindings();
    bindings.push_back({106, unreviewed->id, "l06-setup-stream", "l06-setup-stream-single-setup", {"lite_message"}});
    const auto report = audit_completeness_staged(catalog(), bindings, app::executable_scenarios(106));
    EXPECT_TRUE(std::any_of(report.findings.begin(), report.findings.end(), [&](const CompletenessFinding& f) {
        return f.blocking && f.code == "mismatched_binding" && f.requirement_id == unreviewed->id;
    }));
}

// --- the executable scenario registry ----------------------------------------------------------------------------

TEST(LiteScenarios, TheExecutableListIsTheCatalogsPlannedScenariosWithTheBuildersTrackTraits) {
    std::set<std::string> planned;
    for (const auto& r : catalog().requirements) planned.insert(r.scenarios.begin(), r.scenarios.end());
    std::set<std::string> listed;
    for (const auto id : app::executable_scenarios(106)) listed.emplace(id);
    EXPECT_EQ(listed, planned);
    EXPECT_EQ(app::executable_scenarios(106).size(), 19u);
    std::size_t with_track = 0;
    for (const auto& definition : lite::conformance_probes(LiteBinding::NativeQuic)) {
        const auto traits = app::lite_executable_scenario(definition.id);
        ASSERT_TRUE(traits.has_value()) << definition.id;
        EXPECT_EQ(traits->requires_track, definition.requires_track) << definition.id;
        EXPECT_TRUE(app::executable_scenario(106, definition.id)) << definition.id;
        EXPECT_EQ(app::scenario_requires_track(106, definition.id), definition.requires_track) << definition.id;
        EXPECT_FALSE(app::raw_probe_scenario(106, definition.id)) << definition.id;
        for (const unsigned draft : {18u, 21u, 22u}) EXPECT_FALSE(app::executable_scenario(draft, definition.id));
        with_track += definition.requires_track ? 1 : 0;
    }
    EXPECT_EQ(with_track, 10u);
    EXPECT_FALSE(app::executable_scenario(106, "l06-unknown"));
    EXPECT_FALSE(app::lite_executable_scenario("l06-unknown").has_value());
}

// --- evaluate_lite -----------------------------------------------------------------------------------------------

TEST(LiteEvaluate, AConformingRunScoresEveryRowKind) {
    const auto outcomes = evaluate_lite(catalog(), conforming());
    ASSERT_EQ(outcomes.size(), catalog().requirements.size());
    std::size_t passed = 0;
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        const auto& r = catalog().requirements[index];
        const auto state = outcomes[index].state;
        EXPECT_EQ(outcomes[index].requirement_id, r.id);
        if (!r.reviewed) {
            EXPECT_EQ(state, OutcomeState::NotRun) << r.id;
        } else if (r.applicability != Applicability::Applicable) {
            EXPECT_EQ(state, OutcomeState::NotApplicable) << r.id;
        } else if (r.testability == Testability::NotTestable) {
            EXPECT_EQ(state, OutcomeState::NotTestable) << r.id;
        } else if (kConformingNotRun.contains(r.id)) {
            EXPECT_EQ(state, OutcomeState::NotRun) << r.id;
        } else {
            EXPECT_EQ(state, OutcomeState::Pass) << r.id;
            ++passed;
        }
    }
    EXPECT_EQ(passed, 28u);
}

TEST(LiteEvaluate, TheClientPathRowsFollowTheBindingOfTheirRun) {
    const auto web = lite::conformance_transcripts(LiteBinding::WebTransport);
    const auto states = by_id(evaluate_lite(catalog(), web));
    EXPECT_EQ(states.at("L06-7-3-2-MUST-NOT-125"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-7-3-2-MUST-120"), OutcomeState::NotRun);
    EXPECT_EQ(states.at("L06-7-3-2-SHOULD-124"), OutcomeState::NotRun);
    EXPECT_EQ(states.at("L06-7-3-2-MUST-126"), OutcomeState::NotRun);  // native QUIC only
}

TEST(LiteEvaluate, OneFailingEvaluatorFailsItsRowOnly) {
    const auto ignoring = ignoring_unknown_streams();
    const auto states = by_id(evaluate_lite(catalog(), replaced(conforming(), ignoring)));
    EXPECT_EQ(states.at("L06-7-2-MUST-108"), OutcomeState::Fail);
    EXPECT_EQ(states.at("L06-7-2-MUST-NOT-109"), OutcomeState::Pass);
    const auto baseline = by_id(evaluate_lite(catalog(), conforming()));
    for (const auto& [id, state] : states)
        if (id != "L06-7-2-MUST-108") EXPECT_EQ(state, baseline.at(id)) << id;
}

TEST(LiteEvaluate, FlaggedTranscriptsAreNeverJudged) {
    const auto ignoring = ignoring_unknown_streams();
    ASSERT_EQ(by_id(evaluate_lite(catalog(), replaced(conforming(), ignoring))).at("L06-7-2-MUST-108"),
              OutcomeState::Fail);
    for (const auto flag : {&LiteTranscript::harness_failed, &LiteTranscript::event_limit_reached,
                            &LiteTranscript::timed_out}) {
        // A failing transcript with the flag: no Fail; a passing one: no Pass.
        auto failing = ignoring;
        failing.*flag = true;
        auto states = by_id(evaluate_lite(catalog(), replaced(conforming(), failing)));
        EXPECT_EQ(states.at("L06-7-2-MUST-108"), OutcomeState::NotRun);
        EXPECT_EQ(states.at("L06-7-2-MUST-NOT-109"), OutcomeState::NotRun);
        auto passing = transcript_of("l06-setup-stream");
        passing.*flag = true;
        states = by_id(evaluate_lite(catalog(), replaced(conforming(), passing)));
        EXPECT_EQ(states.at("L06-3-1-MUST-014"), OutcomeState::NotRun);
        EXPECT_EQ(states.at("L06-7-3-MUST-NOT-111"), OutcomeState::NotRun);
    }
}

TEST(LiteEvaluate, AScenarioRunTwiceIsNotAPass) {
    auto twice = conforming();
    twice.push_back(transcript_of("l06-setup-stream"));
    auto states = by_id(evaluate_lite(catalog(), twice));
    EXPECT_EQ(states.at("L06-3-1-MUST-014"), OutcomeState::NotRun);
    EXPECT_EQ(states.at("L06-7-3-MUST-NOT-111"), OutcomeState::NotRun);
    // ...but a false on either run still fails the row.
    auto with_failure = conforming();
    with_failure.push_back(ignoring_unknown_streams());
    states = by_id(evaluate_lite(catalog(), with_failure));
    EXPECT_EQ(states.at("L06-7-2-MUST-108"), OutcomeState::Fail);
    EXPECT_EQ(states.at("L06-7-2-MUST-NOT-109"), OutcomeState::NotRun);
}

TEST(LiteEvaluate, Row027NeedsAllFiveOfItsScenarios) {
    ASSERT_EQ(row("L06-4-4-MUST-027").scenarios.size(), 5u);
    for (const auto& scenario : row("L06-4-4-MUST-027").scenarios) {
        const auto states = by_id(evaluate_lite(catalog(), without(conforming(), scenario)));
        EXPECT_EQ(states.at("L06-4-4-MUST-027"), OutcomeState::NotRun) << "without " << scenario;
    }
    // Without the code-space scenario the four close probes still pass their own rows.
    const auto states = by_id(evaluate_lite(catalog(), without(conforming(), "l06-errors-code-space")));
    EXPECT_EQ(states.at("L06-7-3-MUST-112"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-6-3-1-MUST-092"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-7-3-2-MUST-126"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-7-3-3-MUST-131"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-7-1-SHOULD-107"), OutcomeState::NotRun);
    // A close probe with a stream-only close code fails both its own row and 027 (the documented 027 rule).
    const auto wrong =
        tweaked("l06-setup-duplicate-stream", [](auto& config) { config.protocol_violation_code = 0x33; });
    const auto failing = by_id(evaluate_lite(catalog(), replaced(conforming(), wrong)));
    EXPECT_EQ(failing.at("L06-6-3-1-MUST-092"), OutcomeState::Fail);
    EXPECT_EQ(failing.at("L06-4-4-MUST-027"), OutcomeState::Fail);
}

TEST(LiteEvaluate, NothingRunOrEmptyTranscriptsLeaveEveryScoredRowNotRun) {
    std::vector<LiteTranscript> empty;
    for (const auto id : app::executable_scenarios(106)) {
        LiteTranscript t;
        t.scenario_id = std::string(id);
        empty.push_back(t);
    }
    LiteTranscript stranger;
    stranger.scenario_id = "not-a-lite-scenario";
    empty.push_back(stranger);
    for (const auto& outcomes : {evaluate_lite(catalog(), {}), evaluate_lite(catalog(), empty)}) {
        ASSERT_EQ(outcomes.size(), catalog().requirements.size());
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            if (scored(catalog().requirements[index]))
                EXPECT_EQ(outcomes[index].state, OutcomeState::NotRun) << outcomes[index].requirement_id;
        }
    }
}

TEST(LiteEvaluate, RefusesAnotherDraftsCatalog) {
    auto other = catalog();
    other.draft = 22;
    EXPECT_THROW((void)evaluate_lite(other, conforming()), std::invalid_argument);
}

// --- staged score ------------------------------------------------------------------------------------------------

TEST(LiteScore, AConformingRunIsIncompleteNeverPass) {
    const auto summary = score_staged(catalog(), evaluate_lite(catalog(), conforming()));
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    // The 75 unreviewed rows count in the denominators and never earn.
    EXPECT_GT(summary.required.earned, 0u);
    EXPECT_GT(summary.required.possible, summary.required.earned);
}

TEST(LiteScore, OneFailedRequiredRowFailsTheRun) {
    const auto wrong = tweaked("l06-subscribe-invalid-frame-bounds",
                               [](auto& config) { config.defect = test::lite::LiteDefect::CloseOnInvalidSubscribe; });
    const auto outcomes = evaluate_lite(catalog(), replaced(conforming(), wrong));
    EXPECT_EQ(by_id(outcomes).at("L06-3-6-MUST-023"), OutcomeState::Fail);
    EXPECT_EQ(score_staged(catalog(), outcomes).verdict, RunVerdict::Fail);
}

// --- execution audit of a stored lite run ------------------------------------------------------------------------

storage::RunRecord stored_lite_run(const std::vector<Outcome>& outcomes) {
    storage::RunRecord run;
    run.id = "lite-run";
    run.config = app::RunConfig{app::DraftVersion::MoqLite06, app::TransportKind::NativeQuic, app::RunMode::Observed,
                                {}, std::chrono::milliseconds{30000},
                                app::TrackFixture{{"demo", "live"}, "video"}, {}};
    for (const auto id : app::executable_scenarios(106)) run.config.scenario_ids.emplace_back(id);
    run.build = app::BuildInfo{"test", "test", {}};
    run.state = storage::RunState::Finalized;
    run.created_at_unix_ns = 1;
    run.finalized_at_unix_ns = 2;
    run.outcomes = outcomes;
    run.score = score_staged(catalog(), outcomes);
    // What the run hook records: every declared kind of every binding, under the binding's scenario.
    std::set<std::pair<std::string, std::string>> recorded;
    for (const auto& binding : lite_executable_bindings())
        for (const auto& kind : binding.evidence_kinds) recorded.emplace(binding.scenario_id, kind);
    std::uint64_t sequence = 0;
    for (const auto& [scenario, kind] : recorded) {
        storage::EvidenceEvent event;
        event.sequence = ++sequence;
        event.kind = kind;
        event.scenario_id = scenario;
        run.events.push_back(event);
    }
    return run;
}

TEST(LiteExecutionAudit, AcceptsAStoredStagedRun) {
    const auto run = stored_lite_run(evaluate_lite(catalog(), conforming()));
    const auto bindings = lite_executable_bindings();
    const auto audit = audit_execution(catalog(), bindings, std::span(&run, 1));
    for (const auto& finding : audit.findings)
        ADD_FAILURE() << finding.code << " " << finding.requirement_id << " " << finding.detail;
    EXPECT_TRUE(audit.consistent());
    EXPECT_EQ(audit.scored_rows, 28u);
}

TEST(LiteExecutionAudit, StillCatchesAWrongScoreAndMissingEvidence) {
    auto run = stored_lite_run(evaluate_lite(catalog(), conforming()));
    run.score->verdict = RunVerdict::Pass;
    const auto bindings = lite_executable_bindings();
    auto audit = audit_execution(catalog(), bindings, std::span(&run, 1));
    EXPECT_TRUE(std::any_of(audit.findings.begin(), audit.findings.end(),
                            [](const auto& f) { return f.code == "stored_score_mismatch"; }));
    run = stored_lite_run(evaluate_lite(catalog(), conforming()));
    std::erase_if(run.events, [](const storage::EvidenceEvent& e) { return e.scenario_id == "l06-setup-stream"; });
    audit = audit_execution(catalog(), bindings, std::span(&run, 1));
    EXPECT_TRUE(std::any_of(audit.findings.begin(), audit.findings.end(), [](const auto& f) {
        return f.code == "missing_evaluator_evidence" && f.requirement_id == "L06-3-1-MUST-014";
    }));
}

}  // namespace
}  // namespace moq::interop::requirements
