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

using lite06::allowance_step;

LiteProbeDefinition base(std::string_view id, std::chrono::milliseconds deadline, std::chrono::milliseconds allowance) {
    return lite06::allowance_probe(id, deadline, allowance);
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
    // Ungated: the whole allowance is observed, so a FIN in a later frame or a second Setup stream arriving after
    // the first one finished is still seen.
    definition.steps.push_back(allowance_step(allowance));
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

using lite06::step_labelled;

// The common gate of rows 014 and 111: the l06-setup-stream transcript, judgeable, and the runner's ordinary Setup
// stream delivered. judgeable() rather than judgeable_with_stimulus(): a peer close before the allowance step ran
// (peer_closed_early) still ends the observation, and nothing more can arrive after it.
bool setup_stream_gate(const LiteTranscript& transcript) {
    return transcript.scenario_id == kL06SetupStream && judgeable(transcript) &&
           proved_stimulus(transcript, lite06::kRunnerSetupLabel, lite_default_runner_setup()) != nullptr;
}

}  // namespace

std::optional<bool> evaluate_l06_setup_stream_single_setup(const LiteTranscript& transcript) {
    if (!setup_stream_gate(transcript)) return std::nullopt;
    // Judged only once the observation is over: the whole allowance elapsed (the ungated allowance step executed)
    // or the peer closed the session. So a FIN in a later frame and a later second Setup stream are both seen.
    const auto* allowance = step_labelled(transcript, kL06AllowanceLabel);
    const bool over = transcript.peer_close.has_value() || (allowance && allowance->executed());
    if (!over) return std::nullopt;
    const auto setup = lite06::peer_setup_message(transcript);
    if (!setup) return false;
    if (setup->setup_streams > 1 || setup->reset || setup->malformed || setup->trailing) return false;
    // A SETUP whose only defect is a repeated Parameter ID is decoded here (row 111 judges the repetition).
    return setup->message.has_value() && setup->fin;
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
    // Row 126 tests the "only the client sends Path" half on binding 1 only; on WebTransport a Path is also a
    // URI-binding violation, so the reaction cannot be attributed to this half. Unknown is judged.
    if (transcript.binding == LiteBinding::WebTransport) return std::nullopt;
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
