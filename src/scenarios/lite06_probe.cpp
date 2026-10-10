#include "moq/interop/scenarios/lite06_probe.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "moq/interop/session/lite_session.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::scenarios {
namespace {

namespace l06 = wire::moqlite06;
using lite06::allowance_step;
using lite06::proved_stimulus;
using session::LiteStreamRecord;

bool elapsed(std::uint64_t now_ns, std::uint64_t since_ns, std::chrono::milliseconds allowance) {
    return now_ns >= since_ns && now_ns - since_ns >= static_cast<std::uint64_t>(
                                                          std::chrono::duration_cast<std::chrono::nanoseconds>(allowance)
                                                              .count());
}

// The Probe Stream once its first target reached the publisher exactly as built, and the second target's step when
// the runner sent it.
struct ProbeRun {
    const LiteStreamRecord* record{nullptr};
    const LiteStepRecord* second{nullptr};  // nullptr: the publisher ended the stream before the second target
};

std::optional<ProbeRun> probe_run(const LiteTranscript& transcript) {
    // The publisher's close can be the observation (row 072 fails on a session close once a target was written), so
    // plain judgeable(): the first target is proven below, the second only when the runner got to send it.
    if (transcript.scenario_id != kL06ProbeReport || !judgeable(transcript)) return std::nullopt;
    const auto* open = proved_stimulus(transcript, kL06ProbeOpenLabel, l06_probe_open_bytes(kL06ProbeFirstTarget));
    if (!open || !open->stream_id) return std::nullopt;
    ProbeRun run;
    run.record = lite06::find_stream(transcript, *open->stream_id);
    if (!run.record || run.record->kind != session::LiteStreamKind::Probe) return std::nullopt;
    if (lite06::step_labelled(transcript, kL06ProbeSecondLabel)) {
        run.second = proved_stimulus(transcript, kL06ProbeSecondLabel, l06_probe_message_bytes(kL06ProbeSecondTarget));
        if (!run.second) return std::nullopt;  // sent, but not as built
    }
    return run;
}

}  // namespace

std::vector<std::byte> l06_probe_message_bytes(std::uint64_t target_bps) {
    wire::ByteWriter out(std::size_t{1} << 10);
    if (l06::encode_probe(l06::ProbeMessage{target_bps, 0}, out)) return {};
    return {out.bytes().begin(), out.bytes().end()};
}

std::vector<std::byte> l06_probe_open_bytes(std::uint64_t target_bps) {
    wire::ByteWriter out(std::size_t{1} << 10);
    if (!l06::write_stream_type(static_cast<std::uint64_t>(l06::BidiStreamType::Probe), out)) return {};
    for (const auto byte : l06_probe_message_bytes(target_bps)) {
        if (!out.append_byte(byte)) return {};
    }
    return {out.bytes().begin(), out.bytes().end()};
}

std::optional<std::uint64_t> l06_publisher_probe_level(const LiteTranscript& transcript) {
    const auto setup = lite06::peer_setup_message(transcript);
    if (!setup || !setup->message) return std::nullopt;
    for (const auto& parameter : setup->message->parameters) {
        if (parameter.id != l06::kParamProbe) continue;
        wire::Cursor cursor(parameter.value);
        const auto level = l06::read_varint(cursor);
        const auto* value = std::get_if<std::uint64_t>(&level);
        if (!value || cursor.remaining() != 0) return std::nullopt;
        return *value;
    }
    return 0;  // absent equals None
}

LiteProbeDefinition l06_probe_report_probe(std::chrono::milliseconds deadline, std::chrono::milliseconds allowance,
                                           std::chrono::milliseconds answer_allowance) {
    if (answer_allowance.count() <= 0)
        throw std::invalid_argument("a moq-lite-06 probe scenario needs a positive answer allowance");
    if (deadline <= answer_allowance + allowance)
        throw std::invalid_argument("a moq-lite-06 probe scenario needs a deadline beyond its windows");
    auto definition = lite06::allowance_probe(kL06ProbeReport, deadline, allowance);
    definition.steps.push_back(
        lite_open_bidi(l06_probe_open_bytes(kL06ProbeFirstTarget), false, std::string(kL06ProbeOpenLabel)));
    definition.next_steps = [allowance, answer_allowance](const session::LiteSession& session,
                                                          LiteProbeContext& context) -> std::vector<LiteStep> {
        const auto stream = context.stream_of(0);
        const auto* open = context.steps && !context.steps->empty() ? &context.steps->front() : nullptr;
        if (!stream || !open || !open->executed_at_ns) return {};
        const auto* record = session.find(*stream);
        if (!record) return {};
        // The publisher ended the stream (a reset is what a level None publisher does): no second target, which
        // the reset would refuse; the window is still observed.
        if (record->fin_seen || record->reset_seen || session.peer_close()) {
            context.finished = true;
            return {allowance_step(allowance)};
        }
        if (!session::peer_probe_reports(*record).empty() ||
            elapsed(context.now_ns, *open->executed_at_ns, answer_allowance)) {
            context.finished = true;
            return {lite_send_on(0, l06_probe_message_bytes(kL06ProbeSecondTarget), false,
                                 std::string(kL06ProbeSecondLabel)),
                    allowance_step(allowance)};
        }
        return {};
    };
    return definition;
}

std::optional<bool> evaluate_l06_probe_target_continues(const LiteTranscript& transcript) {
    const auto run = probe_run(transcript);
    if (!run) return std::nullopt;
    const auto level = l06_publisher_probe_level(transcript);
    if (!level || *level == 0) return std::nullopt;  // level None: the stream reset is row 075
    if (!session::peer_protocol_issues(*run->record).empty()) return std::nullopt;  // the stream is unreadable
    // A reset of the Probe Stream after a target is a reset the publisher had no right to (draft 5.1.5: it resets only
    // when it advertised no capability), and so is a session close once the target was written.
    if (run->record->reset_seen) return false;
    if (transcript.peer_close) return false;
    if (!run->second || !run->second->executed_at_ns) return std::nullopt;  // no second target reached it
    // The stream stays usable: a report arrived after the second target (a peer event stamped at the step's time
    // arrived before it).
    for (const auto* report : session::peer_probe_reports(*run->record)) {
        if (report->at_ns > *run->second->executed_at_ns) return true;
    }
    return std::nullopt;
}

bool l06_probe_none_inapplicable(const LiteTranscript& transcript) {
    if (!probe_run(transcript)) return false;
    const auto level = l06_publisher_probe_level(transcript);
    return level && *level != 0;
}

std::optional<bool> evaluate_l06_probe_none_reset(const LiteTranscript& transcript) {
    const auto run = probe_run(transcript);
    if (!run) return std::nullopt;
    const auto level = l06_publisher_probe_level(transcript);
    if (!level || *level != 0) return std::nullopt;  // the publisher advertised a capability: row 072
    if (!session::peer_protocol_issues(*run->record).empty()) return std::nullopt;
    // A report answered instead of the reset is the Fail, whether or not a reset follows.
    if (!session::peer_probe_reports(*run->record).empty()) return false;
    if (run->record->reset_seen) return true;
    if (run->record->fin_seen || transcript.peer_close) return false;  // not a reset of the stream
    return std::nullopt;  // no reaction inside the window
}

}  // namespace moq::interop::scenarios
