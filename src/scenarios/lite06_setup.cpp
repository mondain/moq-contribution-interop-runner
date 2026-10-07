#include "moq/interop/scenarios/lite06_setup.h"

#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/session/lite_session.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/setup.h"

namespace moq::interop::scenarios {
namespace {

namespace l06 = wire::moqlite06;
using lite06::proved_stimulus;

std::vector<std::byte> text(std::string_view value) {
    std::vector<std::byte> out;
    for (const auto c : value) out.push_back(static_cast<std::byte>(c));
    return out;
}

void check(std::chrono::milliseconds deadline, std::chrono::milliseconds allowance) {
    if (allowance.count() <= 0 || deadline <= allowance)
        throw std::invalid_argument("a moq-lite-06 setup probe needs a deadline beyond its allowance");
}

LiteStep allowance_step(std::chrono::milliseconds allowance) {
    return lite_wait(allowance, std::string(kL06AllowanceLabel));
}

LiteProbeDefinition base(std::string_view id, std::chrono::milliseconds deadline, std::chrono::milliseconds allowance) {
    check(deadline, allowance);
    LiteProbeDefinition definition;
    definition.id = std::string(id);
    definition.deadline = deadline;
    // The probe ends as soon as its allowance step executed (or expired, for l06-setup-stream).
    definition.observation_window = std::chrono::milliseconds{0};
    return definition;
}

// Whether the publisher's Setup stream has ended (FIN or reset), read from the recorder's peer streams.
bool peer_setup_ended(const session::LiteSession& session) {
    for (const auto* record : session::peer_streams(session)) {
        if (record->bidirectional || record->stream_type != std::optional<std::uint64_t>{0x1}) continue;
        if (record->fin_seen || record->reset_seen) return true;
        if (!session::peer_issues(*record).empty()) return true;  // decoding stopped: nothing more to wait for
    }
    return false;
}

}  // namespace

std::vector<std::byte> l06_unknown_parameter_runner_setup() {
    return lite06::raw_setup_stream({{kL06UnknownParameterId, text(kL06UnknownParameterValue)}});
}

std::vector<std::byte> l06_duplicate_parameter_runner_setup() {
    const std::vector<std::byte> one{std::byte{0x01}};
    return lite06::raw_setup_stream({{l06::kParamCost, one}, {l06::kParamCost, one}});
}

std::vector<std::byte> l06_server_path_runner_setup() {
    return lite06::raw_setup_stream({{l06::kParamPath, text(kL06ServerPathValue)}});
}

std::vector<std::byte> l06_server_role_runner_setup() {
    return lite06::raw_setup_stream({{l06::kParamRole, {std::byte{0x00}}}});
}

LiteProbeDefinition l06_setup_stream_probe(std::chrono::milliseconds deadline, std::chrono::milliseconds allowance) {
    auto definition = base(kL06SetupStream, deadline, allowance);
    auto step = lite_wait(std::chrono::milliseconds{0}, std::string(kL06AllowanceLabel));
    step.gate = peer_setup_ended;
    step.gate_deadline = allowance;
    definition.steps.push_back(std::move(step));
    return definition;
}

LiteProbeDefinition l06_setup_unknown_parameter_probe(std::chrono::milliseconds deadline,
                                                      std::chrono::milliseconds allowance) {
    auto definition = base(kL06SetupUnknownParameter, deadline, allowance);
    definition.runner_setup = l06_unknown_parameter_runner_setup();
    // The empty prefix covers every broadcast, so no track fixture is needed.
    definition.steps.push_back(
        lite_open_bidi(lite_announce_stream_bytes(l06::AnnounceRequest{""}), false, std::string(kL06AnnounceLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_setup_duplicate_parameter_probe(std::chrono::milliseconds deadline,
                                                        std::chrono::milliseconds allowance) {
    auto definition = base(kL06SetupDuplicateParameter, deadline, allowance);
    definition.runner_setup = l06_duplicate_parameter_runner_setup();
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_setup_duplicate_stream_probe(std::chrono::milliseconds deadline,
                                                     std::chrono::milliseconds allowance) {
    auto definition = base(kL06SetupDuplicateStream, deadline, allowance);
    definition.steps.push_back(lite_send_uni(lite_default_runner_setup(), true, std::string(kL06SecondSetupLabel)));
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_setup_server_path_probe(std::chrono::milliseconds deadline,
                                                std::chrono::milliseconds allowance) {
    auto definition = base(kL06SetupServerPath, deadline, allowance);
    definition.runner_setup = l06_server_path_runner_setup();
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

LiteProbeDefinition l06_setup_server_role_probe(std::chrono::milliseconds deadline,
                                                std::chrono::milliseconds allowance) {
    auto definition = base(kL06SetupServerRole, deadline, allowance);
    definition.runner_setup = l06_server_role_runner_setup();
    definition.steps.push_back(allowance_step(allowance));
    return definition;
}

namespace {

const LiteStepRecord* step_labelled(const LiteTranscript& transcript, std::string_view label) {
    for (const auto& step : transcript.steps)
        if (step.label == label) return &step;
    return nullptr;
}

// The common gate of rows 014 and 111: the l06-setup-stream transcript, judgeable, and the runner's ordinary Setup
// stream delivered. The allowance step is an observation timer, not a stimulus, so judgeable_with_stimulus() (which
// counts it) is not used: it ends unexecuted, by design, when the publisher's Setup stream never ends.
bool setup_stream_gate(const LiteTranscript& transcript) {
    return transcript.scenario_id == kL06SetupStream && judgeable(transcript) &&
           proved_stimulus(transcript, lite06::kRunnerSetupLabel, lite_default_runner_setup()) != nullptr;
}

}  // namespace

std::optional<bool> evaluate_l06_setup_stream_single_setup(const LiteTranscript& transcript) {
    if (!setup_stream_gate(transcript)) return std::nullopt;
    // The observation is over: the publisher's Setup stream ended (the gate opened), the allowance elapsed (the gate
    // expired), or the peer closed the session (nothing more can arrive).
    const auto* allowance = step_labelled(transcript, kL06AllowanceLabel);
    const bool over = transcript.peer_close.has_value() ||
                      (allowance && (allowance->executed() || allowance->gate_expired));
    const auto setup = lite06::peer_setup_message(transcript);
    if (!setup) return over ? std::optional<bool>{false} : std::nullopt;
    if (setup->setup_streams > 1 || setup->reset || setup->malformed || setup->trailing) return false;
    // A SETUP whose only defect is a repeated Parameter ID is decoded here (row 111 judges the repetition).
    if (!setup->message || !setup->fin) return over ? std::optional<bool>{false} : std::nullopt;
    return true;
}

std::optional<bool> evaluate_l06_setup_parameters_unique(const LiteTranscript& transcript) {
    if (!setup_stream_gate(transcript)) return std::nullopt;
    const auto setup = lite06::peer_setup_message(transcript);
    // No decodable SETUP: row 014's matter, not a uniqueness verdict.
    if (!setup || !setup->message) return std::nullopt;
    const auto& parameters = setup->message->parameters;
    // A duplicate needs at least two parameters: fewer is NotRun, never a vacuous Pass.
    if (parameters.size() < 2) return std::nullopt;
    std::set<std::uint64_t> seen;
    for (const auto& parameter : parameters)
        if (!seen.insert(parameter.id).second) return false;
    return true;
}

std::optional<bool> evaluate_l06_setup_unknown_parameter_ignored(const LiteTranscript& transcript) {
    // A close is a Fail here, so the gate is judgeable() plus the proof of the runner's SETUP with the unknown id.
    if (transcript.scenario_id != kL06SetupUnknownParameter || !judgeable(transcript)) return std::nullopt;
    if (!proved_stimulus(transcript, lite06::kRunnerSetupLabel, l06_unknown_parameter_runner_setup()))
        return std::nullopt;
    if (transcript.runner_closed) return std::nullopt;
    if (transcript.peer_close) return false;  // any code, any space, after the unknown parameter was delivered
    const auto* announce = step_labelled(transcript, kL06AnnounceLabel);
    if (!announce) return std::nullopt;
    if (announce->refused == transport::TransportStatus::PeerReset ||
        announce->refused == transport::TransportStatus::PeerStopped)
        return false;  // the request was refused
    if (!proved_stimulus(transcript, kL06AnnounceLabel, lite_announce_stream_bytes(l06::AnnounceRequest{""})) ||
        !announce->stream_id)
        return std::nullopt;
    const auto* stream = lite06::find_stream(transcript, *announce->stream_id);
    if (!stream) return std::nullopt;
    if (stream->reset_seen || stream->stop_sending_seen) return false;  // the request was refused
    // The session must have stayed open for the whole allowance.
    const auto* allowance = step_labelled(transcript, kL06AllowanceLabel);
    if (!allowance || !allowance->executed()) return std::nullopt;
    // The observable consequences of ignoring: the publisher's SETUP and an ANNOUNCE_OK. Their absence without a
    // close or a refusal belongs to other rows (014, the announce rows), so it is NotRun here, not a Fail.
    const auto setup = lite06::peer_setup_message(transcript);
    if (!setup || !setup->message) return std::nullopt;
    for (const auto* decoded : session::peer_messages(*stream))
        if (std::holds_alternative<l06::AnnounceOk>(decoded->message)) return true;
    return std::nullopt;
}

std::optional<bool> evaluate_l06_setup_duplicate_parameter_close(const LiteTranscript& transcript) {
    return lite06::judge_close_probe(
        transcript, kL06SetupDuplicateParameter,
        proved_stimulus(transcript, lite06::kRunnerSetupLabel, l06_duplicate_parameter_runner_setup()));
}

std::optional<bool> evaluate_l06_setup_duplicate_stream_close(const LiteTranscript& transcript) {
    const auto default_setup = lite_default_runner_setup();
    const LiteStepRecord* stimulus = nullptr;
    if (proved_stimulus(transcript, lite06::kRunnerSetupLabel, default_setup))
        stimulus = proved_stimulus(transcript, kL06SecondSetupLabel, default_setup);
    return lite06::judge_close_probe(transcript, kL06SetupDuplicateStream, stimulus);
}

std::optional<bool> evaluate_l06_setup_server_path_close(const LiteTranscript& transcript) {
    return lite06::judge_close_probe(
        transcript, kL06SetupServerPath,
        proved_stimulus(transcript, lite06::kRunnerSetupLabel, l06_server_path_runner_setup()));
}

std::optional<bool> evaluate_l06_setup_server_role_close(const LiteTranscript& transcript) {
    return lite06::judge_close_probe(
        transcript, kL06SetupServerRole,
        proved_stimulus(transcript, lite06::kRunnerSetupLabel, l06_server_role_runner_setup()));
}

}  // namespace moq::interop::scenarios
