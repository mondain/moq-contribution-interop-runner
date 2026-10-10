#include "moq/interop/requirements/lite_evaluators.h"

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_datagram.h"
#include "moq/interop/scenarios/lite06_errors.h"
#include "moq/interop/scenarios/lite06_fetch.h"
#include "moq/interop/scenarios/lite06_goaway.h"
#include "moq/interop/scenarios/lite06_probe.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/scenarios/lite06_track.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace moq::interop::requirements {
namespace {

namespace s = scenarios;

// The evaluator of row L06-4-4-MUST-027 (its aggregation is special: code_space_settled).
constexpr std::string_view kCodeSpaceEvaluator = "l06-errors-code-space";

// Evidence kind sets, each the observation the evaluator's Pass condition guarantees (lite_evaluators.h).
// The SETUP rows judge a lenient re-read of the publisher's Setup stream bytes (lite06::peer_setup_message): a SETUP
// with a repeated Parameter ID passes row 014 although the strict codec decodes no message from it, so these rows
// can only rely on the stream itself, never on a decoded lite_message.
const std::vector<std::string> kSetupStream{"raw_probe_stimulus", "lite_stream_opened"};
const std::vector<std::string> kClose{"raw_probe_stimulus", "peer_close"};
const std::vector<std::string> kMessages{"raw_probe_stimulus", "lite_message"};
const std::vector<std::string> kGroups{"raw_probe_stimulus", "lite_stream_opened", "lite_message"};
const std::vector<std::string> kStreamEnding{"raw_probe_stimulus", "raw_probe_transport_event"};
const std::vector<std::string> kStreamEndingAndClose{"raw_probe_stimulus", "raw_probe_transport_event",
                                                     "peer_close"};
const std::vector<std::string> kStreamEndingAndAnswer{"raw_probe_stimulus", "raw_probe_transport_event",
                                                      "lite_message"};

struct BindingRow {
    std::string_view requirement;
    std::string_view scenario;
    std::string_view evaluator;
    const std::vector<std::string>* kinds;
};

// The (row, scenario, evaluator) triples of requirements/moq-lite-06.json's Applicable + Testable rows.
const std::vector<BindingRow>& binding_rows() {
    static const std::vector<BindingRow> rows{
        {"L06-3-1-MUST-014", "l06-setup-stream", "l06-setup-stream-single-setup", &kSetupStream},
        {"L06-7-3-MUST-NOT-111", "l06-setup-stream", "l06-setup-parameters-unique", &kSetupStream},
        {"L06-7-3-MUST-110", "l06-setup-unknown-parameter", "l06-setup-unknown-parameter-ignored", &kMessages},
        {"L06-7-3-MUST-112", "l06-setup-duplicate-parameter", "l06-setup-duplicate-parameter-close", &kClose},
        {"L06-6-3-1-MUST-092", "l06-setup-duplicate-stream", "l06-setup-duplicate-stream-close", &kClose},
        {"L06-7-3-2-MUST-126", "l06-setup-server-path", "l06-setup-server-path-close", &kClose},
        {"L06-7-3-3-MUST-131", "l06-setup-server-role", "l06-setup-server-role-close", &kClose},
        // Row 027 on its five scenarios. Its Pass needs all five in the run: the stream-table reset of a peer
        // stream on l06-errors-code-space, and a session close in the session table on ANY of the five
        // (aggregate_lite's code_space_settled, per the row's catalog rationale), so a close is declared evidence
        // of each context that supplied the session half, not of every context.
        {"L06-4-4-MUST-027", "l06-errors-code-space", "l06-errors-code-space",
         &kStreamEndingAndClose},
        {"L06-4-4-MUST-027", "l06-setup-duplicate-stream", "l06-errors-code-space", &kClose},
        {"L06-4-4-MUST-027", "l06-setup-duplicate-parameter", "l06-errors-code-space", &kClose},
        {"L06-4-4-MUST-027", "l06-setup-server-path", "l06-errors-code-space", &kClose},
        {"L06-4-4-MUST-027", "l06-setup-server-role", "l06-errors-code-space", &kClose},
        {"L06-7-3-2-MUST-120", "l06-setup-client-path", "l06-setup-path-query-appended", &kSetupStream},
        {"L06-7-3-2-SHOULD-124", "l06-setup-client-path", "l06-setup-path-sent", &kSetupStream},
        {"L06-7-3-2-MUST-NOT-125", "l06-setup-client-path", "l06-setup-path-absent-on-uri-binding", &kSetupStream},
        {"L06-7-4-MUST-139", "l06-announce-prefix", "l06-announce-ok-then-starts", &kMessages},
        {"L06-7-5-MUST-NOT-141", "l06-announce-prefix", "l06-announce-hop-list-excludes-own", &kMessages},
        {"L06-7-5-SHOULD-143", "l06-announce-prefix", "l06-announce-ok-hop-assigned", &kMessages},
        {"L06-7-7-MUST-NOT-152", "l06-announce-lifecycle", "l06-announce-retired-id-unused", &kMessages},
        {"L06-4-3-MUST-025", "l06-session-stream-close", "l06-session-peer-closes-send", &kStreamEnding},
        {"L06-6-3-2-MUST-093", "l06-subscribe-latest", "l06-group-starts-with-group", &kGroups},
        {"L06-6-3-2-MUST-NOT-097", "l06-subscribe-latest", "l06-group-unique-sequence", &kGroups},
        {"L06-7-19-SHOULD-190", "l06-subscribe-latest", "l06-group-sequence-increments", &kGroups},
        {"L06-5-1-2-MUST-062", "l06-subscribe-refused", "l06-subscribe-refused-reset", &kStreamEnding},
        {"L06-3-6-MUST-023", "l06-subscribe-invalid-frame-bounds", "l06-subscribe-invalid-frame-bounds-reset",
         &kStreamEnding},
        {"L06-7-9-MUST-NOT-159", "l06-subscribe-group-floor", "l06-subscribe-no-group-below-floor", &kGroups},
        {"L06-7-13-MUST-172", "l06-subscribe-group-floor", "l06-subscribe-ok-group-at-floor", &kGroups},
        {"L06-3-6-MUST-020", "l06-subscribe-abutting-frame-start", "l06-subscribe-resolved-start", &kGroups},
        {"L06-7-2-MUST-108", "l06-errors-unknown-stream-type", "l06-errors-unknown-stream-type-reset",
         &kStreamEnding},
        {"L06-7-2-MUST-NOT-109", "l06-errors-unknown-stream-type", "l06-errors-unknown-stream-type-not-fatal",
         &kMessages},
        {"L06-4-4-MUST-030", "l06-errors-unknown-reset-code", "l06-errors-unknown-code-tolerated",
         &kStreamEndingAndAnswer},
        {"L06-4-4-MUST-NOT-032", "l06-errors-unknown-reset-code", "l06-errors-no-assumed-unauthorized",
         &kStreamEnding},
        {"L06-4-4-MUST-NOT-033", "l06-errors-reserved-reset-code", "l06-errors-reserved-code-tolerated",
         &kStreamEndingAndAnswer},
        {"L06-7-1-SHOULD-107", "l06-errors-code-space", "l06-errors-message-length-close", &kClose},
        // L2a. A Pass of the TRACK_INFO rows rests on the decoded TRACK_INFO messages; the FETCH rows on the Fetch
        // Stream's ending (and, for the short-run row, the frames it carried); the Probe rows on the reports or the
        // reset; the goaway rows on the session close, or (row 077) on the absence of new streams after a GOAWAY.
        {"L06-7-12-MUST-NOT-163", "l06-track-info", "l06-track-info-immutable", &kMessages},
        {"L06-7-12-MUST-170", "l06-track-info", "l06-track-info-timescale-nonzero", &kMessages},
        {"L06-7-16-MUST-177", "l06-fetch-group", "l06-fetch-short-run", &kStreamEndingAndAnswer},
        {"L06-5-1-3-MUST-066", "l06-fetch-unknown-group", "l06-fetch-unknown-group-reset", &kStreamEnding},
        {"L06-5-1-5-MUST-072", "l06-probe-report", "l06-probe-target-continues", &kMessages},
        {"L06-5-1-5-MUST-075", "l06-probe-report", "l06-probe-none-reset", &kStreamEnding},
        {"L06-5-1-6-MUST-NOT-077", "l06-goaway-single", "l06-goaway-no-new-streams", &kGroups},
        {"L06-7-18-MUST-186", "l06-goaway-duplicate", "l06-goaway-second-closes", &kClose},
        {"L06-7-18-MUST-179", "l06-goaway-oversize", "l06-goaway-oversize-violation", &kClose},
        // L2b. A Pass rests on the datagrams that arrived (stored as raw_probe_transport_event peer-datagram).
        {"L06-6-4-MUST-NOT-105", "l06-datagram-size", "l06-datagram-size-limit", &kStreamEnding},
    };
    return rows;
}

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// A transcript the engine itself marked as cut short is never judged (Harness, never a Fail or a Pass).
bool flagged(const s::LiteTranscript& transcript) {
    return transcript.harness_failed || transcript.event_limit_reached || transcript.timed_out;
}

}  // namespace

std::vector<ExecutableBinding> lite_executable_bindings() {
    std::vector<ExecutableBinding> bindings;
    bindings.reserve(binding_rows().size());
    for (const auto& row : binding_rows()) {
        bindings.push_back({kLiteDraft, std::string(row.requirement), std::string(row.scenario),
                            std::string(row.evaluator), *row.kinds});
    }
    return bindings;
}

const std::map<std::string, LiteEvaluator>& lite_evaluator_registry() {
    static const std::map<std::string, LiteEvaluator> registry{
        {"l06-setup-stream-single-setup", s::evaluate_l06_setup_stream_single_setup},
        {"l06-setup-parameters-unique", s::evaluate_l06_setup_parameters_unique},
        {"l06-setup-unknown-parameter-ignored", s::evaluate_l06_setup_unknown_parameter_ignored},
        {"l06-setup-duplicate-parameter-close", s::evaluate_l06_setup_duplicate_parameter_close},
        {"l06-setup-duplicate-stream-close", s::evaluate_l06_setup_duplicate_stream_close},
        {"l06-setup-server-path-close", s::evaluate_l06_setup_server_path_close},
        {"l06-setup-server-role-close", s::evaluate_l06_setup_server_role_close},
        {"l06-errors-code-space", s::evaluate_l06_errors_code_space},
        {"l06-setup-path-sent", s::evaluate_l06_setup_path_sent},
        {"l06-setup-path-query-appended", s::evaluate_l06_setup_path_query_appended},
        {"l06-setup-path-absent-on-uri-binding", s::evaluate_l06_setup_path_absent_on_uri_binding},
        {"l06-announce-ok-then-starts", s::evaluate_l06_announce_ok_then_starts},
        {"l06-announce-ok-hop-assigned", s::evaluate_l06_announce_ok_hop_assigned},
        {"l06-announce-hop-list-excludes-own", s::evaluate_l06_announce_hop_list_excludes_own},
        {"l06-announce-retired-id-unused", s::evaluate_l06_announce_retired_id_unused},
        {"l06-session-peer-closes-send", s::evaluate_l06_session_peer_closes_send},
        {"l06-group-starts-with-group", s::evaluate_l06_group_starts_with_group},
        {"l06-group-unique-sequence", s::evaluate_l06_group_unique_sequence},
        {"l06-group-sequence-increments", s::evaluate_l06_group_sequence_increments},
        {"l06-subscribe-refused-reset", s::evaluate_l06_subscribe_refused_reset},
        {"l06-subscribe-invalid-frame-bounds-reset", s::evaluate_l06_subscribe_invalid_frame_bounds_reset},
        {"l06-subscribe-no-group-below-floor", s::evaluate_l06_subscribe_no_group_below_floor},
        {"l06-subscribe-ok-group-at-floor", s::evaluate_l06_subscribe_ok_group_at_floor},
        {"l06-subscribe-resolved-start", s::evaluate_l06_subscribe_resolved_start},
        {"l06-errors-unknown-stream-type-reset", s::evaluate_l06_errors_unknown_stream_type_reset},
        {"l06-errors-unknown-stream-type-not-fatal", s::evaluate_l06_errors_unknown_stream_type_not_fatal},
        {"l06-errors-unknown-code-tolerated", s::evaluate_l06_errors_unknown_code_tolerated},
        {"l06-errors-no-assumed-unauthorized", s::evaluate_l06_errors_no_assumed_unauthorized},
        {"l06-errors-reserved-code-tolerated", s::evaluate_l06_errors_reserved_code_tolerated},
        {"l06-errors-message-length-close", s::evaluate_l06_errors_message_length_close},
        {"l06-track-info-immutable", s::evaluate_l06_track_info_immutable},
        {"l06-track-info-timescale-nonzero", s::evaluate_l06_track_info_timescale_nonzero},
        {"l06-fetch-short-run", s::evaluate_l06_fetch_short_run},
        {"l06-fetch-unknown-group-reset", s::evaluate_l06_fetch_unknown_group_reset},
        {"l06-probe-target-continues", s::evaluate_l06_probe_target_continues},
        {"l06-probe-none-reset", s::evaluate_l06_probe_none_reset},
        {"l06-goaway-no-new-streams", s::evaluate_l06_goaway_no_new_streams},
        {"l06-goaway-second-closes", s::evaluate_l06_goaway_second_closes},
        {"l06-goaway-oversize-violation", s::evaluate_l06_goaway_oversize_violation},
        {"l06-datagram-size-limit", s::evaluate_l06_datagram_size_limit},
    };
    return registry;
}

namespace {

void require_lite(const RequirementCatalog& catalog) {
    if (catalog.draft != kLiteDraft) throw std::invalid_argument("a moq-lite-06 (draft 106) catalog is required");
}

// The rows evaluate_lite judges from transcripts (every other row's state follows from the catalog alone).
bool scored_row(const Requirement& row) {
    return row.reviewed && row.applicability == Applicability::Applicable &&
           row.testability != Testability::NotTestable;
}

}  // namespace

LiteContextVerdicts judge_lite_context(const RequirementCatalog& catalog, const scenarios::LiteTranscript& transcript) {
    require_lite(catalog);
    LiteContextVerdicts judged;
    judged.scenario_id = transcript.scenario_id;
    judged.flagged = flagged(transcript);
    if (judged.flagged) return judged;
    const auto& registry = lite_evaluator_registry();
    for (const auto& row : catalog.requirements) {
        if (!scored_row(row) || !contains(row.scenarios, transcript.scenario_id)) continue;
        for (const auto& evaluator : row.evaluators) {
            if (judged.verdicts.contains(evaluator)) continue;  // evaluators are pure: once per context
            const auto found = registry.find(evaluator);
            judged.verdicts[evaluator] = found == registry.end() ? std::optional<bool>{} : found->second(transcript);
            if (evaluator == kCodeSpaceEvaluator) {
                const auto halves = s::l06_code_space_halves(transcript);
                judged.code_space_stream_half = halves.stream;
                judged.code_space_session_half = halves.session;
            }
        }
    }
    return judged;
}

namespace {

// Row 027 (catalog rationale of L06-4-4-MUST-027): every scenario of the row ran exactly once and was judged
// (not flagged); the l06-errors-code-space context saw the stream half; and any context of the row saw the session
// half ("any one close code from those scenarios suffices for the half"). A false half fails the row before this.
bool code_space_settled(const Requirement& row, std::span<const LiteContextVerdicts> contexts) {
    std::map<std::string, std::size_t> runs;
    bool stream_half = false;
    bool session_half = false;
    for (const auto& context : contexts) {
        if (!contains(row.scenarios, context.scenario_id)) continue;
        ++runs[context.scenario_id];
        if (context.flagged) return false;
        if (context.scenario_id == s::kL06ErrorsCodeSpace && context.code_space_stream_half == std::optional<bool>{true})
            stream_half = true;
        if (context.code_space_session_half == std::optional<bool>{true}) session_half = true;
    }
    return stream_half && session_half &&
           std::all_of(row.scenarios.begin(), row.scenarios.end(),
                       [&](const std::string& scenario) { return runs[scenario] == 1; });
}

}  // namespace

std::vector<Outcome> aggregate_lite(const RequirementCatalog& catalog, std::span<const LiteContextVerdicts> contexts) {
    require_lite(catalog);
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& row : catalog.requirements) {
        auto state = OutcomeState::NotRun;
        if (!row.reviewed) {
            state = OutcomeState::NotRun;
        } else if (row.applicability != Applicability::Applicable) {
            state = OutcomeState::NotApplicable;
        } else if (row.testability == Testability::NotTestable) {
            state = OutcomeState::NotTestable;
        } else {
            // Per scenario of the row: how many contexts ran it, and how many of them every evaluator of the row
            // judged true.
            std::map<std::string, std::size_t> runs;
            std::map<std::string, std::size_t> passed;
            bool failed = false;
            for (const auto& context : contexts) {
                if (!contains(row.scenarios, context.scenario_id)) continue;
                ++runs[context.scenario_id];
                if (context.flagged) continue;
                bool all_true = !row.evaluators.empty();
                for (const auto& evaluator : row.evaluators) {
                    const auto found = context.verdicts.find(evaluator);
                    const auto verdict = found == context.verdicts.end() ? std::optional<bool>{} : found->second;
                    if (verdict == std::optional<bool>{false}) failed = true;
                    if (verdict != std::optional<bool>{true}) all_true = false;
                }
                if (all_true) ++passed[context.scenario_id];
            }
            bool settled = !row.scenarios.empty() &&
                std::all_of(row.scenarios.begin(), row.scenarios.end(), [&](const std::string& scenario) {
                    return runs[scenario] == 1 && passed[scenario] == 1;
                });
            if (!failed && !settled && row.evaluators == std::vector<std::string>{std::string(kCodeSpaceEvaluator)})
                settled = code_space_settled(row, contexts);
            if (failed) state = OutcomeState::Fail;
            else if (settled) state = OutcomeState::Pass;
        }
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

std::vector<Outcome> evaluate_lite(const RequirementCatalog& catalog,
                                   std::span<const scenarios::LiteTranscript> transcripts) {
    require_lite(catalog);
    LiteVerdicts contexts;
    contexts.reserve(transcripts.size());
    for (const auto& transcript : transcripts) contexts.push_back(judge_lite_context(catalog, transcript));
    return aggregate_lite(catalog, contexts);
}

std::size_t lite_retained_bytes(const LiteContextVerdicts& context) {
    // A map node: the pair plus (generously) four pointers and a colour word of tree bookkeeping.
    constexpr std::size_t kNode = sizeof(std::pair<const std::string, std::optional<bool>>) + 5 * sizeof(void*);
    std::size_t bytes = sizeof(LiteContextVerdicts) + context.scenario_id.capacity();
    for (const auto& [id, verdict] : context.verdicts) bytes += kNode + id.capacity();
    return bytes;
}

}  // namespace moq::interop::requirements
