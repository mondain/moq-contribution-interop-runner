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
#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_errors.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/moqlite06/setup.h"
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
#include <variant>
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

// The conformance transcripts (all 27 scenarios on native QUIC), computed once.
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
// 152 (no retraction within the window). Rows 075 (judged only for a publisher that advertised no Probe capability;
// the conforming one advertises Report) and 125 (WebTransport only) are NotApplicable to a native QUIC run since L2c.
const std::set<std::string> kConformingNotRun{"L06-7-7-MUST-NOT-152"};

// --- registry and bindings ---------------------------------------------------------------------------------------

TEST(LiteEvaluators, RegistryHoldsExactlyTheCatalogEvaluators) {
    std::set<std::string> expected;
    for (const auto& r : catalog().requirements) expected.insert(r.evaluators.begin(), r.evaluators.end());
    ASSERT_EQ(expected.size(), 40u);
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
    // 29 + 10 single-scenario rows plus row 027 on its five scenarios.
    EXPECT_EQ(expected.size(), 44u);
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
    EXPECT_EQ(report.required_total, 36u);
    EXPECT_EQ(report.required_covered, 36u);
    EXPECT_EQ(report.optional_total, 4u);
    EXPECT_EQ(report.optional_covered, 4u);
    EXPECT_EQ(report.unreviewed_total, 0u);
    EXPECT_TRUE(report.complete()) << "every reviewed row is covered and none is unreviewed (the catalog flag, not the audit, keeps it staged)";
    // No row is left uncovered and none is unreviewed: nothing remains to report except that the catalog is staged.
    EXPECT_TRUE(report.findings.empty());
}

TEST(LiteEvaluators, ABindingNamingAnUnreviewedRowStillBlocks) {
    // Every real row is reviewed now, so one is un-reviewed in a copy of the catalog (the way L1 had it).
    auto copy = catalog();
    const auto target = std::find_if(copy.requirements.begin(), copy.requirements.end(),
                                     [](const Requirement& r) { return r.id == "L06-6-4-MUST-103"; });
    ASSERT_NE(target, copy.requirements.end());
    target->reviewed = false;
    auto bindings = lite_executable_bindings();
    bindings.push_back({106, target->id, "l06-setup-stream", "l06-setup-stream-single-setup", {"lite_message"}});
    const auto report = audit_completeness_staged(copy, bindings, app::executable_scenarios(106));
    EXPECT_TRUE(std::any_of(report.findings.begin(), report.findings.end(), [&](const CompletenessFinding& f) {
        return f.blocking && f.code == "mismatched_binding" && f.requirement_id == target->id;
    }));
}

// --- the executable scenario registry ----------------------------------------------------------------------------

TEST(LiteScenarios, TheExecutableListIsTheCatalogsPlannedScenariosWithTheBuildersTrackTraits) {
    std::set<std::string> planned;
    for (const auto& r : catalog().requirements) planned.insert(r.scenarios.begin(), r.scenarios.end());
    std::set<std::string> listed;
    for (const auto id : app::executable_scenarios(106)) listed.emplace(id);
    EXPECT_EQ(listed, planned);
    EXPECT_EQ(app::executable_scenarios(106).size(), 27u);
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
    EXPECT_EQ(with_track, 15u);
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
        } else if (r.id == "L06-5-1-5-MUST-075" || r.id == "L06-7-3-2-MUST-NOT-125") {
            EXPECT_EQ(state, OutcomeState::NotApplicable) << r.id;
        } else {
            EXPECT_EQ(state, OutcomeState::Pass) << r.id;
            ++passed;
        }
    }
    EXPECT_EQ(passed, 37u);
}

TEST(LiteEvaluate, TheClientPathRowsFollowTheBindingOfTheirRun) {
    const auto web = lite::conformance_transcripts(LiteBinding::WebTransport);
    const auto states = by_id(evaluate_lite(catalog(), web));
    EXPECT_EQ(states.at("L06-7-3-2-MUST-NOT-125"), OutcomeState::Pass);
    // Judged on native QUIC only: not applicable to a WebTransport run (L2c), not merely unjudged.
    EXPECT_EQ(states.at("L06-7-3-2-MUST-120"), OutcomeState::NotApplicable);
    EXPECT_EQ(states.at("L06-7-3-2-SHOULD-124"), OutcomeState::NotApplicable);
    EXPECT_EQ(states.at("L06-7-3-2-MUST-126"), OutcomeState::NotApplicable);
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

// The behavior of the moq CLI in the first moq-lite-06 sweep (docs/moq-lite-punch-list.md): it ignores a server's
// SETUP Path and Role (no close) and answers the Message Length mismatch of l06-errors-code-space with a stream reset
// (CANCEL) instead of a session close.
void ignore_runner_setup(test::lite::ConformingLitePublisherConfig& config) {
    config.hooks.on_request = [](auto&, auto&, const test::lite::LiteRunnerRequest& request) {
        return !request.bidirectional && request.stream_type == 0x1;  // a runner Setup stream, decodable or not
    };
}
void reset_malformed_announce(test::lite::ConformingLitePublisherConfig& config) {
    config.hooks.on_request = [](test::lite::ConformingLitePublisher& publisher, auto& peer,
                                 const test::lite::LiteRunnerRequest& request) {
        if (!request.bidirectional || request.stream_type != 0x1) return false;  // not an Announce stream
        publisher.refuse(peer, request.stream, 0x1);
        return true;
    };
}

// Catalog rationale of L06-4-4-MUST-027: the session half "also takes evidence from the session closes provoked by
// the MUST-level probes ... so it is not tied to the SHOULD-level reaction of L06-7-1-SHOULD-107; any one close code
// from those scenarios suffices for the half". With all five scenarios run, the stream half from l06-errors-code-space
// and the PROTOCOL_VIOLATION closes of 092 and 112 settle the row, although 107, 126 and 131 got no close.
TEST(LiteEvaluate, Row027TakesItsSessionHalfFromAnyOfItsScenarios) {
    auto transcripts = replaced(conforming(), tweaked("l06-errors-code-space", reset_malformed_announce));
    transcripts = replaced(transcripts, tweaked("l06-setup-server-path", ignore_runner_setup));
    transcripts = replaced(transcripts, tweaked("l06-setup-server-role", ignore_runner_setup));
    const auto states = by_id(evaluate_lite(catalog(), transcripts));
    EXPECT_EQ(states.at("L06-7-1-SHOULD-107"), OutcomeState::Fail);
    EXPECT_EQ(states.at("L06-7-3-2-MUST-126"), OutcomeState::Fail);
    EXPECT_EQ(states.at("L06-7-3-3-MUST-131"), OutcomeState::Fail);
    EXPECT_EQ(states.at("L06-6-3-1-MUST-092"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-7-3-MUST-112"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-4-4-MUST-027"), OutcomeState::Pass);
    // One close is enough: only l06-setup-duplicate-parameter closes.
    auto one = replaced(transcripts, tweaked("l06-setup-duplicate-stream", ignore_runner_setup));
    EXPECT_EQ(by_id(evaluate_lite(catalog(), one)).at("L06-4-4-MUST-027"), OutcomeState::Pass);
    // No close in any of the five: no session half, NotRun (never a Pass on the stream half alone).
    auto none = replaced(one, tweaked("l06-setup-duplicate-parameter", ignore_runner_setup));
    EXPECT_EQ(by_id(evaluate_lite(catalog(), none)).at("L06-4-4-MUST-027"), OutcomeState::NotRun);
    // The stream half comes only from l06-errors-code-space: without its refusal the closes alone are NotRun.
    auto no_stream_half = replaced(conforming(), tweaked("l06-errors-code-space", [](auto& config) {
        config.hooks.on_request = [](auto&, auto&, const test::lite::LiteRunnerRequest& request) {
            return request.bidirectional && request.stream_type == 0x2;  // leave the unserved SUBSCRIBE pending
        };
    }));
    EXPECT_EQ(by_id(evaluate_lite(catalog(), no_stream_half)).at("L06-4-4-MUST-027"), OutcomeState::NotRun);
    // Still all five scenarios, once each: without one of them the row stays NotRun (Row027NeedsAllFiveOfItsScenarios).
    EXPECT_EQ(by_id(evaluate_lite(catalog(), without(transcripts, "l06-setup-server-role"))).at("L06-4-4-MUST-027"),
              OutcomeState::NotRun);
    // A wrong space anywhere still fails it.
    const auto wrong =
        tweaked("l06-setup-duplicate-stream", [](auto& config) { config.protocol_violation_code = 0x33; });
    EXPECT_EQ(by_id(evaluate_lite(catalog(), replaced(transcripts, wrong))).at("L06-4-4-MUST-027"), OutcomeState::Fail);
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

// --- per-context evaluation (L1e Task 1, item I3) ----------------------------------------------------------------

// The L1d evaluate_lite algorithm, copied verbatim as the reference the per-context path must reproduce (the
// production evaluate_lite is now built on aggregate_lite, so it cannot be its own reference), plus the one rule
// added since L1d by design: row 027 (evaluator l06-errors-code-space) settles when all five of its scenarios ran
// once each unflagged, the code-space transcript has the stream half and any of the five the session half (its
// catalog rationale). The reference computes that rule from the transcripts with scenarios::l06_code_space_halves,
// independently of LiteContextVerdicts.
std::vector<Outcome> reference_evaluate_lite(const RequirementCatalog& catalog,
                                             std::span<const LiteTranscript> transcripts) {
    const auto& registry = lite_evaluator_registry();
    std::vector<Outcome> outcomes;
    for (const auto& row : catalog.requirements) {
        auto state = OutcomeState::NotRun;
        if (!row.reviewed) {
            state = OutcomeState::NotRun;
        } else if (row.applicability != Applicability::Applicable) {
            state = OutcomeState::NotApplicable;
        } else if (row.testability == Testability::NotTestable) {
            state = OutcomeState::NotTestable;
        } else {
            std::map<std::string, std::size_t> runs;
            std::map<std::string, std::size_t> passed;
            std::map<std::string, std::size_t> outside;
            bool failed = false;
            for (const auto& transcript : transcripts) {
                if (std::find(row.scenarios.begin(), row.scenarios.end(), transcript.scenario_id) ==
                    row.scenarios.end())
                    continue;
                ++runs[transcript.scenario_id];
                if (transcript.harness_failed || transcript.event_limit_reached || transcript.timed_out) continue;
                bool all_true = !row.evaluators.empty();
                bool all_outside = !row.evaluators.empty();
                for (const auto& evaluator : row.evaluators) {
                    const auto found = registry.find(evaluator);
                    const auto verdict =
                        found == registry.end() ? std::optional<bool>{} : found->second(transcript);
                    if (verdict == std::optional<bool>{false}) failed = true;
                    if (verdict != std::optional<bool>{true}) all_true = false;
                    const auto gate = lite_applicability_registry().find(evaluator);
                    if (verdict || gate == lite_applicability_registry().end() || !gate->second(transcript))
                        all_outside = false;
                }
                if (all_true) ++passed[transcript.scenario_id];
                if (all_outside) ++outside[transcript.scenario_id];
            }
            bool settled = !row.scenarios.empty() &&
                std::all_of(row.scenarios.begin(), row.scenarios.end(), [&](const std::string& scenario) {
                    return runs[scenario] == 1 && passed[scenario] == 1;
                });
            if (!failed && !settled && row.evaluators == std::vector<std::string>{"l06-errors-code-space"}) {
                std::map<std::string, std::size_t> seen;
                bool flagged = false, stream_half = false, session_half = false;
                for (const auto& transcript : transcripts) {
                    if (std::find(row.scenarios.begin(), row.scenarios.end(), transcript.scenario_id) ==
                        row.scenarios.end())
                        continue;
                    ++seen[transcript.scenario_id];
                    if (transcript.harness_failed || transcript.event_limit_reached || transcript.timed_out) {
                        flagged = true;
                        continue;
                    }
                    const auto halves = scenarios::l06_code_space_halves(transcript);
                    if (transcript.scenario_id == "l06-errors-code-space" && halves.stream == true) stream_half = true;
                    if (halves.session == true) session_half = true;
                }
                settled = !flagged && stream_half && session_half &&
                          std::all_of(row.scenarios.begin(), row.scenarios.end(),
                                      [&](const std::string& scenario) { return seen[scenario] == 1; });
            }
            const bool outside_reach = !row.scenarios.empty() &&
                std::all_of(row.scenarios.begin(), row.scenarios.end(), [&](const std::string& scenario) {
                    return runs[scenario] == 1 && outside[scenario] == 1;
                });
            if (failed) state = OutcomeState::Fail;
            else if (settled) state = OutcomeState::Pass;
            else if (outside_reach) state = OutcomeState::NotApplicable;
        }
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

std::vector<Outcome> per_context(std::span<const LiteTranscript> transcripts) {
    LiteVerdicts judged;
    for (const auto& t : transcripts) judged.push_back(judge_lite_context(catalog(), t));
    return aggregate_lite(catalog(), judged);
}

void expect_identical(const std::vector<Outcome>& expected, const std::vector<Outcome>& actual,
                      const std::string& label) {
    ASSERT_EQ(expected.size(), actual.size()) << label;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i].requirement_id, actual[i].requirement_id) << label;
        EXPECT_EQ(expected[i].state, actual[i].state) << label << ": " << expected[i].requirement_id;
    }
}

// Every transcript set below: the reference, evaluate_lite and the per-context aggregation agree row by row.
TEST(LitePerContext, MatchesTheAllTranscriptsEvaluationOverTheConformanceTableAndItsDefects) {
    std::vector<std::pair<std::string, std::vector<LiteTranscript>>> sets;
    for (const auto binding : {LiteBinding::NativeQuic, LiteBinding::WebTransport, LiteBinding::Unknown})
        sets.emplace_back("conforming " + std::to_string(static_cast<int>(binding)),
                          lite::conformance_transcripts(binding));
    // Defect transcripts (each turns at least one row): one per named defect and fixture switch.
    const std::vector<std::pair<std::string, LiteTranscript>> defects{
        {"ignore-unknown-streams", ignoring_unknown_streams()},
        {"offset-group-start", tweaked("l06-subscribe-group-floor",
                                       [](auto& c) { c.defect = test::lite::LiteDefect::OffsetGroupStart; })},
        {"close-on-invalid-subscribe",
         tweaked("l06-subscribe-invalid-frame-bounds",
                 [](auto& c) { c.defect = test::lite::LiteDefect::CloseOnInvalidSubscribe; })},
        {"no-fin-echo", tweaked("l06-session-stream-close", [](auto& c) { c.echo_runner_fin = false; })},
        {"stream-code-close",
         tweaked("l06-setup-duplicate-stream", [](auto& c) { c.protocol_violation_code = 0x33; })},
        {"no-path", tweaked("l06-setup-client-path", [](auto& c) { c.session_url_path.clear(); })},
        {"silent-on-announce",
         tweaked("l06-announce-prefix", [](auto& c) { c.defect = test::lite::LiteDefect::SilentOnAnnounce; })},
        {"no-setup-stream",
         tweaked("l06-setup-stream", [](auto& c) { c.defect = test::lite::LiteDefect::NoSetupStream; })},
    };
    std::vector<LiteTranscript> all_defects = conforming();
    for (const auto& [label, defect] : defects) {
        sets.emplace_back(label, replaced(conforming(), defect));
        all_defects.push_back(defect);
    }
    sets.emplace_back("every defect beside the conforming runs", all_defects);
    // Flagged transcripts: each flag on a failing and on a passing context.
    for (const auto flag : {&LiteTranscript::harness_failed, &LiteTranscript::event_limit_reached,
                            &LiteTranscript::timed_out}) {
        auto failing = ignoring_unknown_streams();
        failing.*flag = true;
        sets.emplace_back("flagged failing", replaced(conforming(), failing));
        auto passing = transcript_of("l06-setup-stream");
        passing.*flag = true;
        sets.emplace_back("flagged passing", replaced(conforming(), passing));
        auto twice = conforming();
        twice.push_back(passing);
        sets.emplace_back("flagged second run", twice);
    }
    // Repeats, partial sets, empty and foreign transcripts.
    auto twice = conforming();
    twice.push_back(transcript_of("l06-setup-stream"));
    sets.emplace_back("twice", twice);
    sets.emplace_back("without code-space", without(conforming(), "l06-errors-code-space"));
    sets.emplace_back("nothing", std::vector<LiteTranscript>{});
    std::vector<LiteTranscript> empty;
    for (const auto id : app::executable_scenarios(106)) {
        LiteTranscript t;
        t.scenario_id = std::string(id);
        empty.push_back(t);
    }
    LiteTranscript stranger;
    stranger.scenario_id = "not-a-lite-scenario";
    empty.push_back(stranger);
    sets.emplace_back("empty and foreign", empty);
    // CLI-like (the moq CLI of the first sweep): the stream half on l06-errors-code-space, a session close on one of
    // the four close probes (duplicate parameter), no close on the others; then the same with one probe context
    // flagged, and with one 027 scenario run twice.
    auto cli = replaced(conforming(), tweaked("l06-errors-code-space", reset_malformed_announce));
    for (const auto* probe : {"l06-setup-server-path", "l06-setup-server-role", "l06-setup-duplicate-stream"})
        cli = replaced(cli, tweaked(probe, ignore_runner_setup));
    sets.emplace_back("cli-like", cli);
    // The flagged probe is one that supplies no half (server role), so only the flag keeps the row from settling.
    auto cli_flagged_probe = tweaked("l06-setup-server-role", ignore_runner_setup);
    cli_flagged_probe.timed_out = true;
    const auto cli_flagged = replaced(cli, cli_flagged_probe);
    sets.emplace_back("cli-like, a flagged close probe", cli_flagged);
    auto cli_twice = cli;
    cli_twice.push_back(tweaked("l06-setup-server-role", ignore_runner_setup));
    sets.emplace_back("cli-like, a 027 scenario twice", cli_twice);
    EXPECT_EQ(by_id(per_context(cli)).at("L06-4-4-MUST-027"), OutcomeState::Pass);
    EXPECT_EQ(by_id(per_context(cli_flagged)).at("L06-4-4-MUST-027"), OutcomeState::NotRun);
    EXPECT_EQ(by_id(per_context(cli_twice)).at("L06-4-4-MUST-027"), OutcomeState::NotRun);
    std::size_t turned = 0;
    for (const auto& [label, transcripts] : sets) {
        const auto reference = reference_evaluate_lite(catalog(), transcripts);
        expect_identical(reference, evaluate_lite(catalog(), transcripts), label + " (evaluate_lite)");
        expect_identical(reference, per_context(transcripts), label + " (per context)");
        for (const auto& outcome : reference) turned += outcome.state == OutcomeState::Fail ? 1u : 0u;
    }
    EXPECT_GT(turned, 0u) << "the defect sets exercise Fail outcomes";
}

TEST(LitePerContext, AFlaggedContextKeepsNoVerdictsAndOnlyItsOwnEvaluators) {
    auto flagged = transcript_of("l06-setup-stream");
    flagged.timed_out = true;
    const auto judged = judge_lite_context(catalog(), flagged);
    EXPECT_EQ(judged.scenario_id, "l06-setup-stream");
    EXPECT_TRUE(judged.flagged);
    EXPECT_TRUE(judged.verdicts.empty());
    const auto clean = judge_lite_context(catalog(), transcript_of("l06-setup-stream"));
    EXPECT_FALSE(clean.flagged);
    std::set<std::string> expected;
    for (const auto& r : catalog().requirements) {
        if (!scored(r) ||
            std::find(r.scenarios.begin(), r.scenarios.end(), "l06-setup-stream") == r.scenarios.end())
            continue;
        expected.insert(r.evaluators.begin(), r.evaluators.end());
    }
    std::set<std::string> held;
    for (const auto& [id, verdict] : clean.verdicts) {
        held.insert(id);
        EXPECT_EQ(verdict, std::optional<bool>{true}) << id;
    }
    EXPECT_EQ(held, expected);
    // What a context keeps is tiny next to its transcript.
    EXPECT_LT(lite_retained_bytes(clean), std::size_t{4096});
    EXPECT_THROW((void)judge_lite_context([] { auto other = catalog(); other.draft = 22; return other; }(),
                                          transcript_of("l06-setup-stream")),
                 std::invalid_argument);
    EXPECT_THROW((void)aggregate_lite([] { auto other = catalog(); other.draft = 22; return other; }(), {}),
                 std::invalid_argument);
}

// --- score (the catalog is complete since L2c) -----------------------------------------------------------------

TEST(LiteScore, AConformingRunWithRowsNothingJudgedIsIncomplete) {
    const auto summary = score(catalog(), evaluate_lite(catalog(), conforming()));
    EXPECT_EQ(summary.verdict, RunVerdict::Incomplete);
    // Rows nothing judges (not_run in a conforming run) count in the denominators and never earn.
    EXPECT_GT(summary.required.earned, 0u);
    EXPECT_GT(summary.required.possible, summary.required.earned);
}

TEST(LiteScore, AConformingRunThatJudgedEveryScoredRowPasses) {
    // The scripted table leaves row 152 (a retraction) unjudged; with a publisher that retracts the broadcast every
    // scored row is judged or not applicable (075 to a Report publisher, 125 to a native run) and the run is a Pass.
    const auto retracting = tweaked("l06-announce-lifecycle", [](auto& config) { config.retract_after_polls = 5; });
    const auto outcomes = evaluate_lite(catalog(), replaced(conforming(), retracting));
    for (const auto& outcome : outcomes) EXPECT_NE(outcome.state, OutcomeState::NotRun) << outcome.requirement_id;
    const auto summary = score(catalog(), outcomes);
    EXPECT_EQ(summary.verdict, RunVerdict::Pass);
    EXPECT_EQ(summary.required.earned, summary.required.possible);
    const auto states = by_id(outcomes);
    EXPECT_EQ(states.at("L06-5-1-5-MUST-075"), OutcomeState::NotApplicable);
    EXPECT_EQ(states.at("L06-7-3-2-MUST-NOT-125"), OutcomeState::NotApplicable);
    EXPECT_EQ(states.at("L06-7-7-MUST-NOT-152"), OutcomeState::Pass);
}

TEST(LiteScore, TheStagedScorerRefusesTheCompleteCatalog) {
    EXPECT_EQ(score_staged(catalog(), evaluate_lite(catalog(), conforming())).verdict, RunVerdict::Error);
}

TEST(LiteScore, OneFailedRequiredRowFailsTheRun) {
    const auto wrong = tweaked("l06-subscribe-invalid-frame-bounds",
                               [](auto& config) { config.defect = test::lite::LiteDefect::CloseOnInvalidSubscribe; });
    const auto outcomes = evaluate_lite(catalog(), replaced(conforming(), wrong));
    EXPECT_EQ(by_id(outcomes).at("L06-3-6-MUST-023"), OutcomeState::Fail);
    EXPECT_EQ(score(catalog(), outcomes).verdict, RunVerdict::Fail);
}

// --- declared evidence vs what a Pass really observes --------------------------------------------------------------

// The evidence kinds a transcript really holds, by the mapping the run hook records (lite_evaluators.h):
// a stimulus the runner wrote -> raw_probe_stimulus; a stream the publisher opened -> lite_stream_opened; a decoded
// publisher message (on any stream) -> lite_message; a publisher FIN / RESET_STREAM / STOP_SENDING ->
// raw_probe_transport_event; the publisher's session close -> peer_close; a decode issue on the publisher's bytes ->
// lite_decode_error.
std::set<std::string> observed_kinds(const LiteTranscript& t) {
    std::set<std::string> kinds;
    const auto wrote = [](const scenarios::LiteStepRecord& step) { return step.accepted > 0 || step.fin_accepted; };
    if (wrote(t.runner_setup) || std::any_of(t.steps.begin(), t.steps.end(), wrote)) kinds.insert("raw_probe_stimulus");
    if (!scenarios::lite06::peer_streams(t).empty()) kinds.insert("lite_stream_opened");
    for (const auto& record : t.streams) {
        if (!session::peer_messages(record).empty()) kinds.insert("lite_message");
        if (!session::peer_issues(record).empty()) kinds.insert("lite_decode_error");
    }
    for (const auto& event : t.events) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if ((data && data->fin) || std::holds_alternative<transport::PeerResetEvent>(event) ||
            std::holds_alternative<transport::PeerStopSendingEvent>(event))
            kinds.insert("raw_probe_transport_event");
    }
    if (t.peer_close) kinds.insert("peer_close");
    return kinds;
}

// Every Pass evaluate_lite gives over `transcripts` carries, in the transcript of each bound scenario, every kind
// its binding declares. Returns the number of Pass rows checked.
std::size_t check_declared_kinds_observed(const std::vector<LiteTranscript>& transcripts, const std::string& label) {
    std::size_t checked = 0;
    const auto bindings = lite_executable_bindings();
    for (const auto& outcome : evaluate_lite(catalog(), transcripts)) {
        if (outcome.state != OutcomeState::Pass) continue;
        ++checked;
        for (const auto& binding : bindings) {
            if (binding.requirement_id != outcome.requirement_id) continue;
            const auto t = std::find_if(transcripts.begin(), transcripts.end(),
                                        [&](const LiteTranscript& x) { return x.scenario_id == binding.scenario_id; });
            if (t == transcripts.end()) {
                ADD_FAILURE() << label << ": " << outcome.requirement_id << " passed without " << binding.scenario_id;
                continue;
            }
            const auto observed = observed_kinds(*t);
            for (const auto& kind : binding.evidence_kinds)
                EXPECT_TRUE(observed.contains(kind)) << label << ": " << outcome.requirement_id << " on "
                                                     << binding.scenario_id << " declares " << kind;
        }
    }
    return checked;
}

std::vector<LiteTranscript> run_all(LiteBinding binding,
                                    const std::function<void(test::lite::ConformingLitePublisherConfig&)>& tweak) {
    std::vector<LiteTranscript> out;
    for (auto& definition : lite::conformance_probes(binding))
        out.push_back(lite::run_conforming(std::move(definition), binding, tweak));
    return out;
}

// The publisher's own Setup stream written raw with `parameters` (a repeated Parameter ID allowed).
std::function<void(test::lite::ConformingLitePublisherConfig&)> raw_setup(
    std::vector<wire::moqlite06::SetupParameter> parameters) {
    return [parameters](test::lite::ConformingLitePublisherConfig& config) {
        config.hooks.on_start = [parameters](test::lite::ConformingLitePublisher&, test::lite::ScriptedLitePeer& peer) {
            peer.data(peer.open_peer_uni(), scenarios::lite06::raw_setup_stream(parameters), true);
            return true;
        };
    };
}

TEST(LiteEvidence, EveryPassCarriesItsDeclaredKinds) {
    namespace l06 = wire::moqlite06;
    const auto bytes = [](std::string_view text) {
        std::vector<std::byte> out;
        for (const char c : text) out.push_back(static_cast<std::byte>(c));
        return out;
    };
    const l06::SetupParameter hop{l06::kParamHop, {std::byte{0x07}}};
    const l06::SetupParameter path{l06::kParamPath, bytes("/moq?token=l1d")};
    // The conformance runs on every binding.
    for (const auto binding : {LiteBinding::NativeQuic, LiteBinding::WebTransport, LiteBinding::Unknown})
        EXPECT_GT(check_declared_kinds_observed(lite::conformance_transcripts(binding), "conforming"), 0u);
    // A publisher SETUP with a repeated Parameter ID: 014 (and 120/124 on native QUIC, 125 on WebTransport) still
    // pass on the lenient re-read, while the strict codec decodes no SETUP message (a decode issue only).
    const auto native = run_all(LiteBinding::NativeQuic, raw_setup({path, hop, hop}));
    const auto states = by_id(evaluate_lite(catalog(), native));
    EXPECT_EQ(states.at("L06-3-1-MUST-014"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-7-3-MUST-NOT-111"), OutcomeState::Fail);
    EXPECT_EQ(states.at("L06-7-3-2-SHOULD-124"), OutcomeState::Pass);
    EXPECT_EQ(states.at("L06-7-3-2-MUST-120"), OutcomeState::Pass);
    for (const auto& t : native) {
        if (t.scenario_id != "l06-setup-stream") continue;
        const auto kinds = observed_kinds(t);
        EXPECT_FALSE(kinds.contains("lite_message")) << "the strict codec decoded the repeated-id SETUP";
        EXPECT_TRUE(kinds.contains("lite_decode_error"));
    }
    EXPECT_GT(check_declared_kinds_observed(native, "repeated-id native"), 0u);
    const auto web = run_all(LiteBinding::WebTransport, raw_setup({hop, hop}));
    EXPECT_EQ(by_id(evaluate_lite(catalog(), web)).at("L06-7-3-2-MUST-NOT-125"), OutcomeState::Pass);
    EXPECT_GT(check_declared_kinds_observed(web, "repeated-id webtransport"), 0u);
    // The unknown stream type answered by STOP_SENDING alone (108 passes on either reaction).
    const auto stop_only = run_all(LiteBinding::NativeQuic, [](test::lite::ConformingLitePublisherConfig& config) {
        config.hooks.on_request = [](test::lite::ConformingLitePublisher&, test::lite::ScriptedLitePeer& peer,
                                     const test::lite::LiteRunnerRequest& request) {
            if (!request.bidirectional || request.stream_type != scenarios::kL06UnregisteredStreamType) return false;
            peer.peer_stop_sending(request.stream, 0x0);
            return true;
        };
    });
    EXPECT_EQ(by_id(evaluate_lite(catalog(), stop_only)).at("L06-7-2-MUST-108"), OutcomeState::Pass);
    EXPECT_GT(check_declared_kinds_observed(stop_only, "stop-only"), 0u);
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
    run.score = score(catalog(), outcomes);
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
    EXPECT_EQ(audit.scored_rows, 37u);
    // Row 075 is stored as NotApplicable (the conforming publisher advertises Report): a scored row the audit accepts
    // without evidence and leaves out of the scored count (L2c).
    const auto stored = std::find_if(run.outcomes.begin(), run.outcomes.end(),
                                     [](const auto& o) { return o.requirement_id == "L06-5-1-5-MUST-075"; });
    ASSERT_NE(stored, run.outcomes.end());
    EXPECT_EQ(stored->state, OutcomeState::NotApplicable);
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

// --- L2c: the lite-only inapplicable verdict for capability-gated rows ---------------------------------------------

namespace moq::interop::requirements {
namespace {

TEST(LiteApplicability, AReportAdvertisingPublisherMakesRow075Inapplicable) {
    const auto context = judge_lite_context(catalog(), transcript_of("l06-probe-report"));
    EXPECT_TRUE(context.inapplicable.contains("l06-probe-none-reset"));
    const auto outcomes = by_id(aggregate_lite(catalog(), std::vector{context}));
    EXPECT_EQ(outcomes.at("L06-5-1-5-MUST-075"), OutcomeState::NotApplicable);
}

TEST(LiteApplicability, ANoneAdvertisingPublisherIsJudgedNotInapplicable) {
    const auto t = tweaked("l06-probe-report", [](auto& config) { config.setup_parameters.pop_back(); });
    const auto context = judge_lite_context(catalog(), t);
    EXPECT_FALSE(context.inapplicable.contains("l06-probe-none-reset"));
    EXPECT_EQ(context.verdicts.at("l06-probe-none-reset"), std::optional<bool>{true});
}

TEST(LiteApplicability, AFailBeatsInapplicable) {
    LiteContextVerdicts na;
    na.scenario_id = "l06-probe-report";
    na.inapplicable = {"l06-probe-none-reset"};
    LiteContextVerdicts bad;
    bad.scenario_id = "l06-probe-report";
    bad.verdicts["l06-probe-none-reset"] = false;
    EXPECT_EQ(by_id(aggregate_lite(catalog(), std::vector{na, bad})).at("L06-5-1-5-MUST-075"), OutcomeState::Fail);
    // One context alone is the only way to be inapplicable: a scenario run twice is not.
    EXPECT_EQ(by_id(aggregate_lite(catalog(), std::vector{na, na})).at("L06-5-1-5-MUST-075"), OutcomeState::NotRun);
}

TEST(LiteApplicability, TheBindingRestrictedPathRowsAreInapplicableOnTheOtherBinding) {
    const auto native = judge_lite_context(catalog(), transcript_of("l06-setup-client-path"));
    EXPECT_TRUE(native.inapplicable.contains("l06-setup-path-absent-on-uri-binding"));
    EXPECT_FALSE(native.inapplicable.contains("l06-setup-path-sent"));
    EXPECT_FALSE(native.inapplicable.contains("l06-setup-path-query-appended"));
    const auto wt_transcripts = lite::conformance_transcripts(LiteBinding::WebTransport);
    const auto wt = std::find_if(wt_transcripts.begin(), wt_transcripts.end(),
                                 [](const auto& t) { return t.scenario_id == "l06-setup-client-path"; });
    ASSERT_NE(wt, wt_transcripts.end());
    const auto context = judge_lite_context(catalog(), *wt);
    EXPECT_TRUE(context.inapplicable.contains("l06-setup-path-sent"));
    EXPECT_TRUE(context.inapplicable.contains("l06-setup-path-query-appended"));
    EXPECT_FALSE(context.inapplicable.contains("l06-setup-path-absent-on-uri-binding"));
}

// Row 152 needs a publisher that retracts the broadcast: with one, the conforming scenario judges the row.
TEST(LiteEvaluate, ARetractingPublisherPassesRow152) {
    const auto t = tweaked("l06-announce-lifecycle", [](auto& config) { config.retract_after_polls = 5; });
    const auto outcomes = by_id(aggregate_lite(catalog(), std::vector{judge_lite_context(catalog(), t)}));
    EXPECT_EQ(outcomes.at("L06-7-7-MUST-NOT-152"), OutcomeState::Pass);
    // Without the knob the row stays unjudged.
    const auto quiet = by_id(aggregate_lite(
        catalog(), std::vector{judge_lite_context(catalog(), transcript_of("l06-announce-lifecycle"))}));
    EXPECT_EQ(quiet.at("L06-7-7-MUST-NOT-152"), OutcomeState::NotRun);
}

TEST(LiteApplicability, OnlyTheEightGatedEvaluatorsHavePredicates) {
    std::set<std::string> ids;
    for (const auto& [id, predicate] : lite_applicability_registry()) ids.insert(id);
    EXPECT_EQ(ids, (std::set<std::string>{"l06-probe-none-reset", "l06-datagram-size-limit",
                                          "l06-goaway-no-new-streams", "l06-goaway-second-closes",
                                          "l06-setup-server-path-close", "l06-setup-path-sent",
                                          "l06-setup-path-query-appended",
                                          "l06-setup-path-absent-on-uri-binding"}));
}

TEST(LiteApplicability, ADatagramlessPublisherMakesRow105InapplicableAndADatagramSenderDoesNot) {
    const auto none = tweaked("l06-datagram-size", [](auto& config) { config.datagrams = false; });
    EXPECT_TRUE(judge_lite_context(catalog(), none).inapplicable.contains("l06-datagram-size-limit"));
    const auto sender = judge_lite_context(catalog(), transcript_of("l06-datagram-size"));
    EXPECT_FALSE(sender.inapplicable.contains("l06-datagram-size-limit"));
    EXPECT_EQ(sender.verdicts.at("l06-datagram-size-limit"), std::optional<bool>{true});
}

TEST(LiteApplicability, APublisherThatEndsTheSessionOnTheFirstGoawayMakesRows077And186Inapplicable) {
    const auto close = [](auto& config) { config.defect = test::lite::LiteDefect::GoawayClosesSessionOnFirst; };
    EXPECT_TRUE(judge_lite_context(catalog(), tweaked("l06-goaway-single", close))
                    .inapplicable.contains("l06-goaway-no-new-streams"));
    EXPECT_TRUE(judge_lite_context(catalog(), tweaked("l06-goaway-duplicate", close))
                    .inapplicable.contains("l06-goaway-second-closes"));
    // A publisher that carries on is judged.
    EXPECT_FALSE(judge_lite_context(catalog(), transcript_of("l06-goaway-single"))
                     .inapplicable.contains("l06-goaway-no-new-streams"));
    EXPECT_FALSE(judge_lite_context(catalog(), transcript_of("l06-goaway-duplicate"))
                     .inapplicable.contains("l06-goaway-second-closes"));
}

TEST(LiteApplicability, AVerdictIsNeverMarkedInapplicable) {
    // An inapplicable id only ever stands for a nullopt verdict.
    for (const auto& t : conforming()) {
        const auto context = judge_lite_context(catalog(), t);
        for (const auto& id : context.inapplicable)
            EXPECT_EQ(context.verdicts.at(id), std::nullopt) << t.scenario_id << " " << id;
    }
}

}  // namespace
}  // namespace moq::interop::requirements
