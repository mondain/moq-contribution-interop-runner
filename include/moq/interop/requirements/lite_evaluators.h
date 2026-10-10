#pragma once

// The moq-lite-06 evaluators bound to requirements/moq-lite-06.json (L1d Task 8): the executable bindings, the
// evaluator registry and the outcome of every catalog row from a run's transcripts.
//
// The evaluators (src/scenarios/lite06_*.cpp) return a verdict only (nullopt = NotRun); they record no evidence
// text. The evidence a run stores (Task 9's run hook) is what every binding below declares, per scenario context:
//   raw_probe_stimulus         each runner write the probe delivered (the runner Setup stream included);
//   raw_probe_transport_event  each peer stream ending: FIN, RESET_STREAM or STOP_SENDING (with its code);
//   peer_close                 the publisher's session close (code and space);
//   lite_stream_opened         each publisher stream classified by STREAM_TYPE (session::kLiteStreamOpenedKind);
//   lite_message               each decoded publisher message (session::kLiteMessageKind);
//   lite_decode_error          each decode issue (session::kLiteDecodeErrorKind; never required for a Pass).
// A binding names only kinds its evaluator's Pass condition guarantees, so a stored Pass carries them when the run
// hook records the kinds as defined above (tests/unit/lite_evaluators_test.cpp derives the kinds from transcripts
// by this mapping and checks every Pass). The SETUP rows (014, 111, 120, 124, 125) declare no lite_message: they
// judge a lenient re-read of the Setup stream bytes, and a SETUP with a repeated Parameter ID passes 014 while the
// strict codec decodes no message from it (only a lite_decode_error).
//
// evaluate_lite drops a flagged transcript's verdicts entirely (harness_failed, event_limit_reached or timed_out,
// the same flags judgeable() refuses): it still counts as a run of its scenario (so a second, clean run of the same
// scenario cannot pass the row), but none of its evaluators is consulted, so it yields neither Pass nor Fail.
// Evidence notes the row rationales ask for, for the run hook to record in the event details: the time-bounded
// flag and the stated allowance/window of a Fail of 139, 025, 108 and 107; the first subscription's Position
// (Group Start, Frame Start) for 020 (its extension check); the session URL path, query and binding for 120 (and
// 124/125); which of the reset and STOP_SENDING came first for 108; the `refused` step's gate_expired for 107; the
// publisher SETUP's Hop parameter for 143.

#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/lite_probe.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::requirements {

inline constexpr unsigned kLiteDraft = 106;

// One binding per (row, scenario, evaluator) of every Applicable + Testable row: the product of each row's scenario
// and evaluator lists (34: 29 single-scenario rows and row 027 on its five scenarios), draft 106.
std::vector<ExecutableBinding> lite_executable_bindings();

using LiteEvaluator = std::function<std::optional<bool>(const scenarios::LiteTranscript&)>;
// Evaluator id -> function (the 30 ids pinned by tests/unit/lite_catalog_test.cpp).
const std::map<std::string, LiteEvaluator>& lite_evaluator_registry();

// Evaluator id -> "this peer cannot be judged by the row" (L2c). True when the peer's own behavior leaves the rule
// vacuous or out of reach, not when it broke the rule: it advertised a Probe capability (row 075), sent no datagram
// (105), or ended the session on the first GOAWAY (077, 186). Consulted only for an evaluator that gave no verdict
// (nullopt); a verdict of true or false is never replaced. Four evaluators have one (pinned by the unit tests).
using LiteApplicability = std::function<bool(const scenarios::LiteTranscript&)>;
const std::map<std::string, LiteApplicability>& lite_applicability_registry();

// One Outcome per catalog row, in catalog order. Unreviewed rows NotRun; not Applicable NotApplicable; NotTestable
// NotTestable. A scored row is Fail when any of its evaluators returned false on a transcript of one of its
// scenarios; Pass only when every scenario it names ran exactly once and every one of its evaluators returned true
// on each of them; otherwise NotRun. Row 027 (evaluator l06-errors-code-space) is the exception its catalog
// rationale states: it also needs all five scenarios run once each (none flagged), but passes when the
// l06-errors-code-space context saw a stream code of the right space and ANY of the five saw a session close of the
// right space (scenarios::l06_code_space_halves), so it does not depend on the SHOULD-level close of 107 or on
// every probe closing. A transcript that is harness_failed,
// event_limit_reached or timed_out is never judged (NotRun). Throws std::invalid_argument for a catalog that is not
// draft 106.
std::vector<Outcome> evaluate_lite(const RequirementCatalog& catalog,
                                   std::span<const scenarios::LiteTranscript> transcripts);

// Per-context evaluation (L1e, item I3): what evaluate_lite needs of one transcript, so a run can judge each
// context as it ends and keep this instead of the transcript. evaluate_lite(catalog, transcripts) is
// aggregate_lite(catalog, {judge_lite_context(catalog, t) for each t}), so both give identical outcomes.
struct LiteContextVerdicts {
    // The scenario the context ran: every context counts as one run of it (judged or not).
    std::string scenario_id;
    // harness_failed, event_limit_reached or timed_out: no evaluator was consulted and `verdicts` is empty.
    bool flagged{false};
    // Evaluator id -> verdict (nullopt: NotRun) for every evaluator of every scored row naming scenario_id; an
    // evaluator id the registry does not hold is recorded as nullopt.
    std::map<std::string, std::optional<bool>> verdicts;
    // Evaluator ids of `verdicts` that gave nullopt because the peer is outside the rule's reach (see
    // lite_applicability_registry). A row whose single run of every scenario was inapplicable and that nothing failed
    // is NotApplicable, which score() leaves out of its denominators.
    std::set<std::string> inapplicable;
    // Row 027's halves (scenarios::l06_code_space_halves) when the evaluator l06-errors-code-space was consulted.
    std::optional<bool> code_space_stream_half;
    std::optional<bool> code_space_session_half;
};
using LiteVerdicts = std::vector<LiteContextVerdicts>;

// Judges one context. Throws std::invalid_argument for a catalog that is not draft 106.
LiteContextVerdicts judge_lite_context(const RequirementCatalog& catalog, const scenarios::LiteTranscript& transcript);
// The outcomes of the contexts judged above, by the rules of evaluate_lite. Throws std::invalid_argument for a
// catalog that is not draft 106.
std::vector<Outcome> aggregate_lite(const RequirementCatalog& catalog, std::span<const LiteContextVerdicts> contexts);
// An upper estimate of the memory one judged context holds (its strings, map nodes and verdicts), for the run
// hook's retention bound.
std::size_t lite_retained_bytes(const LiteContextVerdicts& context);

}  // namespace moq::interop::requirements
