#include "moq/interop/scenarios/lite_probe.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <variant>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/setup.h"

namespace moq::interop::scenarios {

namespace {

namespace l06 = wire::moqlite06;
using transport::TransportStatus;

std::uint64_t to_ns(std::chrono::milliseconds value) {
    return value.count() <= 0 ? 0 : static_cast<std::uint64_t>(value.count()) * 1000000u;
}

bool opens_stream(LiteStep::Kind kind) {
    return kind == LiteStep::Kind::SendUni || kind == LiteStep::Kind::OpenBidiAndSend;
}

// The transport refused because of the peer or the connection's end: the step is over, not a harness fault.
bool peer_refusal(TransportStatus status) {
    return status == TransportStatus::PeerReset || status == TransportStatus::PeerStopped ||
           status == TransportStatus::ConnectionClosed;
}

bool must_wait(TransportStatus status) {
    return status == TransportStatus::WouldBlock || status == TransportStatus::StreamLimit;
}

LiteStepRecord record_for(const LiteStep& step, std::size_t index, bool dynamic) {
    LiteStepRecord record;
    record.index = index;
    record.label = step.label;
    record.kind = step.kind;
    record.dynamic = dynamic;
    record.fin = step.fin;
    record.code = step.code;
    record.reason = step.reason;
    return record;
}

template <class Encode>
std::vector<std::byte> stream_with(std::uint64_t type, Encode&& encode) {
    wire::ByteWriter output(std::size_t{1} << 21);
    if (!l06::write_stream_type(type, output) || encode(output).has_value()) return {};
    return {output.bytes().begin(), output.bytes().end()};
}

}  // namespace

std::uint64_t SteadyLiteClock::now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::optional<transport::StreamId> LiteProbeContext::stream_of(std::size_t step) const {
    if (!steps || step >= steps->size()) return std::nullopt;
    return (*steps)[step].stream_id;
}

std::vector<std::byte> lite_default_runner_setup() {
    return stream_with(static_cast<std::uint64_t>(l06::UniStreamType::Setup),
                       [](wire::ByteWriter& out) { return l06::encode_setup(l06::SetupMessage{}, out); });
}

std::vector<std::byte> lite_announce_stream_bytes(const l06::AnnounceRequest& request) {
    return stream_with(static_cast<std::uint64_t>(l06::BidiStreamType::Announce),
                       [&](wire::ByteWriter& out) { return l06::encode_announce_request(request, out); });
}

std::vector<std::byte> lite_subscribe_stream_bytes(const l06::Subscribe& subscribe) {
    return stream_with(static_cast<std::uint64_t>(l06::BidiStreamType::Subscribe),
                       [&](wire::ByteWriter& out) { return l06::encode_subscribe(subscribe, out); });
}

LiteStep lite_send_uni(std::vector<std::byte> bytes, bool fin, std::string label) {
    LiteStep step;
    step.kind = LiteStep::Kind::SendUni;
    step.bytes = std::move(bytes);
    step.fin = fin;
    step.label = std::move(label);
    return step;
}

LiteStep lite_open_bidi(std::vector<std::byte> bytes, bool fin, std::string label) {
    auto step = lite_send_uni(std::move(bytes), fin, std::move(label));
    step.kind = LiteStep::Kind::OpenBidiAndSend;
    return step;
}

LiteStep lite_send_on(std::size_t stream_ref, std::vector<std::byte> bytes, bool fin, std::string label) {
    auto step = lite_send_uni(std::move(bytes), fin, std::move(label));
    step.kind = LiteStep::Kind::SendOnStream;
    step.stream_ref = stream_ref;
    return step;
}

LiteStep lite_fin(std::size_t stream_ref, std::string label) {
    LiteStep step;
    step.kind = LiteStep::Kind::FinStream;
    step.fin = true;
    step.stream_ref = stream_ref;
    step.label = std::move(label);
    return step;
}

LiteStep lite_reset(std::size_t stream_ref, std::uint64_t code, std::string label) {
    LiteStep step;
    step.kind = LiteStep::Kind::ResetStream;
    step.stream_ref = stream_ref;
    step.code = code;
    step.label = std::move(label);
    return step;
}

LiteStep lite_stop_sending(std::size_t stream_ref, std::uint64_t code, std::string label) {
    auto step = lite_reset(stream_ref, code, std::move(label));
    step.kind = LiteStep::Kind::StopSending;
    return step;
}

LiteStep lite_close(std::uint64_t code, std::string reason, std::string label) {
    LiteStep step;
    step.kind = LiteStep::Kind::CloseSession;
    step.code = code;
    step.reason = std::move(reason);
    step.label = std::move(label);
    return step;
}

LiteStep lite_wait(std::chrono::milliseconds delay, std::string label) {
    LiteStep step;
    step.kind = LiteStep::Kind::Wait;
    step.delay = delay;
    step.label = std::move(label);
    return step;
}

LiteStep lite_mark(std::string label) {
    LiteStep step;
    step.kind = LiteStep::Kind::Mark;
    step.label = std::move(label);
    return step;
}

std::string_view to_string(LiteStep::Kind kind) {
    switch (kind) {
        case LiteStep::Kind::SendUni: return "send_uni";
        case LiteStep::Kind::OpenBidiAndSend: return "open_bidi_and_send";
        case LiteStep::Kind::SendOnStream: return "send_on_stream";
        case LiteStep::Kind::FinStream: return "fin_stream";
        case LiteStep::Kind::ResetStream: return "reset_stream";
        case LiteStep::Kind::StopSending: return "stop_sending";
        case LiteStep::Kind::CloseSession: return "close_session";
        case LiteStep::Kind::Wait: return "wait";
        case LiteStep::Kind::Mark: return "mark";
    }
    return "unknown";
}

bool judgeable(const LiteTranscript& transcript) {
    if (!transcript.established || !transcript.complete || transcript.harness_failed ||
        transcript.event_limit_reached || transcript.timed_out)
        return false;
    for (const auto& record : transcript.streams)
        if (!session::harness_issues(record).empty()) return false;
    return true;
}

bool judgeable_with_stimulus(const LiteTranscript& transcript) {
    return judgeable(transcript) && transcript.stimulus_delivered && !transcript.peer_closed_early;
}

LiteProbeController::LiteProbeController(LiteProbeDefinition definition, transport::SessionTransport& transport,
                                         LiteClock& clock)
    : definition_(std::move(definition)), transport_(transport), clock_(clock), session_(definition_.limits) {
    transcript_.scenario_id = definition_.id;
    transcript_.session_url_has_path = definition_.session_url_has_path;
    transcript_.binding = definition_.binding;
    LiteStep setup;
    setup.kind = LiteStep::Kind::SendUni;
    setup.bytes = definition_.runner_setup;
    setup.fin = definition_.runner_setup_fin;
    setup.label = "runner-setup";
    transcript_.runner_setup = record_for(setup, 0, false);
    steps_ = std::move(definition_.steps);
    definition_.steps.clear();
    if (steps_.size() > kLiteMaximumSteps) {
        steps_.resize(kLiteMaximumSteps);
        fail("the probe defines more than " + std::to_string(kLiteMaximumSteps) + " steps");
    }
    for (std::size_t i = 0; i < steps_.size(); ++i) transcript_.steps.push_back(record_for(steps_[i], i, false));
    context_.steps = &transcript_.steps;
}

void LiteProbeController::fail(std::string reason) {
    if (!transcript_.harness_failed) transcript_.harness_failure_reason = std::move(reason);
    transcript_.harness_failed = true;
    transcript_.complete = false;
}

void LiteProbeController::limit(std::string reason) {
    if (!transcript_.event_limit_reached) transcript_.event_limit_reason = std::move(reason);
    transcript_.event_limit_reached = true;
}

void LiteProbeController::finish(std::uint64_t now) {
    if (ended_) return;
    ended_ = true;
    transcript_.ended_ns = now;
    bool delivered = definition_.runner_setup.empty() || transcript_.runner_setup.delivered();
    for (const auto& record : transcript_.steps) {
        const bool no_action = record.kind == LiteStep::Kind::Wait || record.kind == LiteStep::Kind::Mark;
        if (!(no_action ? record.executed() : record.delivered())) delivered = false;
    }
    transcript_.stimulus_delivered = transcript_.established && delivered && !continuation_open();
    stale_ = true;
}

void LiteProbeController::record(const transport::TransportEvent& event, std::uint64_t now) {
    if (transcript_.event_limit_reached) return;
    if (transcript_.events.size() >= kLiteMaximumEvents) {
        limit("more than " + std::to_string(kLiteMaximumEvents) + " transport events in this context");
        return;
    }
    if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
        if (data->data.size() > kLiteMaximumEvidenceBytes - evidence_bytes_) {
            limit("more than " + std::to_string(kLiteMaximumEvidenceBytes) + " bytes of stream data in this context");
            return;
        }
        evidence_bytes_ += data->data.size();
    }
    transcript_.events.push_back(event);
    transcript_.event_times.push_back(now);
}

// False when the probe must stop processing this batch (harness failure).
bool LiteProbeController::handle(const transport::TransportEvent& event, std::uint64_t now) {
    if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&event)) {
        if (transcript_.established) {
            fail("the transport reported two connections for one context");
            return false;
        }
        transcript_.established = true;
        transcript_.established_ns = now;
        transcript_.alpn.assign(reinterpret_cast<const char*>(established->alpn.data()), established->alpn.size());
        if (transcript_.alpn != kLiteAlpn) {
            fail("the publisher negotiated ALPN '" + transcript_.alpn + "', not " + std::string(kLiteAlpn));
            return false;
        }
        session_.on_event(event, now);
        return true;
    }
    const bool stream_event = std::holds_alternative<transport::StreamDataEvent>(event) ||
                              std::holds_alternative<transport::PeerResetEvent>(event) ||
                              std::holds_alternative<transport::PeerStopSendingEvent>(event);
    if (stream_event) {
        if (!transcript_.established) {
            fail("stream data arrived before the transport was established");
            return false;
        }
        session_.on_event(event, now);
        stale_ = true;
        return true;
    }
    if (std::holds_alternative<transport::PeerCloseEvent>(event)) {
        session_.on_event(event, now);
        peer_closed_ = true;
        stale_ = true;
        return true;
    }
    if (std::holds_alternative<transport::LocalCloseEvent>(event)) {
        local_closed_ = true;
        return true;
    }
    if (std::holds_alternative<transport::IdleTimeoutEvent>(event)) {
        idle_timeout_ = true;
        return true;
    }
    if (std::holds_alternative<transport::TransportErrorEvent>(event)) {
        fail("the transport reported an error");
        return false;
    }
    if (std::holds_alternative<transport::EventQueueOverflowEvent>(event)) {
        session_.on_event(event, now);
        limit("the transport event queue overflowed; events were lost");
        return true;
    }
    return true;  // datagrams are recorded only
}

bool LiteProbeController::steps_finished() const noexcept { return next_step_ >= steps_.size(); }

bool LiteProbeController::continuation_open() const noexcept {
    return static_cast<bool>(definition_.next_steps) && !context_.finished;
}

std::optional<transport::StreamId> LiteProbeController::resolve_stream(const LiteStep& step, LiteStepRecord& record,
                                                                       bool& skip) {
    skip = false;
    if (step.stream_ref) {
        const auto ref = *step.stream_ref;
        if (ref >= record.index || !opens_stream(steps_[ref].kind)) {
            fail("step '" + step.label + "' refers to step " + std::to_string(ref) +
                 ", which is not an earlier stream-opening step");
            return std::nullopt;
        }
        const auto& target = transcript_.steps[ref];
        if (!target.stream_id || !target.executed()) {
            skip = true;
            record.skipped_reason = "the referenced step has no stream";
            return std::nullopt;
        }
        return target.stream_id;
    }
    if (step.target) return step.target(session_);
    fail("step '" + step.label + "' acts on a stream but names none");
    return std::nullopt;
}

LiteProbeController::Progress LiteProbeController::write_all(LiteStepRecord& record, std::uint64_t now) {
    const auto id = *record.stream_id;
    const bool bidirectional = (id & 2u) == 0u;
    while (record.accepted < record.bytes.size() || (record.fin && !record.fin_accepted)) {
        const auto remaining = std::span<const std::byte>(record.bytes).subspan(record.accepted);
        const auto result = transport_.write(id, remaining, record.fin);
        if (result.accepted > remaining.size()) {
            fail("the transport reported accepting more bytes than were written");
            return Progress::Failed;
        }
        const bool all = result.accepted == remaining.size();
        const bool fin_now = record.fin && all && result.status == TransportStatus::Success;
        if (result.accepted > 0 || fin_now)
            session_.note_local_write(id, bidirectional, remaining.first(result.accepted), fin_now, now);
        record.accepted += result.accepted;
        if (fin_now) record.fin_accepted = true;
        if (result.status == TransportStatus::Success) {
            if (!all) {
                fail("the transport reported success without accepting every byte");
                return Progress::Failed;
            }
            return Progress::Done;
        }
        if (result.status == TransportStatus::Partial) {
            if (result.accepted == 0) return Progress::Wait;
            continue;
        }
        if (must_wait(result.status)) return Progress::Wait;
        if (peer_refusal(result.status)) {
            record.refused = result.status;
            return Progress::Done;
        }
        fail("the transport rejected the write of step '" + record.label + "'");
        return Progress::Failed;
    }
    return Progress::Done;
}

LiteProbeController::Progress LiteProbeController::execute(const LiteStep& step, LiteStepRecord& record,
                                                           std::uint64_t now) {
    using Kind = LiteStep::Kind;
    switch (step.kind) {
        case Kind::Wait:
        case Kind::Mark:
            return Progress::Done;
        case Kind::SendUni:
        case Kind::OpenBidiAndSend: {
            if (!record.stream_id) {
                const auto opened = step.kind == Kind::SendUni ? transport_.open_uni() : transport_.open_bidi();
                if (must_wait(opened.status)) return Progress::Wait;
                if (peer_refusal(opened.status)) {
                    record.refused = opened.status;
                    return Progress::Done;
                }
                if (opened.status != TransportStatus::Success) {
                    fail("the transport could not open a stream for step '" + step.label + "'");
                    return Progress::Failed;
                }
                record.stream_id = opened.stream_id;
                record.bytes = step.bytes;
            }
            return write_all(record, now);
        }
        case Kind::SendOnStream:
        case Kind::FinStream:
        case Kind::ResetStream:
        case Kind::StopSending: {
            if (!record.stream_id) {
                bool skip = false;
                const auto id = resolve_stream(step, record, skip);
                if (transcript_.harness_failed) return Progress::Failed;
                if (skip) return Progress::Done;
                if (!id) return Progress::Wait;
                record.stream_id = id;
                record.bytes = step.kind == Kind::SendOnStream ? step.bytes : std::vector<std::byte>{};
            }
            if (step.kind == Kind::SendOnStream || step.kind == Kind::FinStream) return write_all(record, now);
            const auto result = step.kind == Kind::ResetStream ? transport_.reset(*record.stream_id, step.code)
                                                               : transport_.stop_sending(*record.stream_id, step.code);
            if (result.status == TransportStatus::Success) return Progress::Done;
            if (must_wait(result.status)) return Progress::Wait;
            if (peer_refusal(result.status)) {
                record.refused = result.status;
                return Progress::Done;
            }
            fail("the transport rejected step '" + step.label + "'");
            return Progress::Failed;
        }
        case Kind::CloseSession: {
            const auto* reason = reinterpret_cast<const std::byte*>(step.reason.data());
            const auto result = transport_.close(step.code, std::span<const std::byte>(reason, step.reason.size()));
            if (result.status == TransportStatus::Success) return Progress::Done;
            if (must_wait(result.status)) return Progress::Wait;
            if (peer_refusal(result.status)) {
                record.refused = result.status;
                return Progress::Done;
            }
            fail("the transport rejected the session close of step '" + step.label + "'");
            return Progress::Failed;
        }
    }
    return Progress::Done;
}

void LiteProbeController::continue_steps(std::uint64_t now) {
    if (!definition_.next_steps || context_.finished) return;
    std::size_t stream_signals = 0;
    for (const auto& record : session_.streams())
        stream_signals +=
            static_cast<std::size_t>(record.reset_seen) + static_cast<std::size_t>(record.stop_sending_seen);
    const std::array<std::size_t, 4> seen{session_.message_count(), session_.streams().size(), stream_signals,
                                          static_cast<std::size_t>(session_.peer_close().has_value())};
    if (continuation_seen_ && *continuation_seen_ == seen) return;
    continuation_seen_ = seen;
    context_.now_ns = now;
    context_.established_ns = transcript_.established_ns;
    auto more = definition_.next_steps(session_, context_);
    for (auto& step : more) {
        if (steps_.size() >= kLiteMaximumSteps) {
            fail("the probe appended more than " + std::to_string(kLiteMaximumSteps) + " steps");
            return;
        }
        transcript_.steps.push_back(record_for(step, steps_.size(), true));
        steps_.push_back(std::move(step));
    }
}

void LiteProbeController::run_steps(std::uint64_t now) {
    while (next_step_ < steps_.size() && !transcript_.harness_failed && !ended_) {
        const auto& step = steps_[next_step_];
        auto& record = transcript_.steps[next_step_];
        if (!record.current_at_ns) record.current_at_ns = now;
        if (!record.gate_opened_ns) {
            if (!step.gate || step.gate(session_)) {
                record.gate_opened_ns = now;
            } else {
                if (step.gate_deadline.count() > 0 && now - *record.current_at_ns >= to_ns(step.gate_deadline)) {
                    record.gate_expired = true;
                    ++next_step_;
                    continue;
                }
                return;
            }
        }
        if (now - *record.gate_opened_ns < to_ns(step.delay)) return;
        const auto progress = execute(step, record, now);
        if (progress != Progress::Done) return;
        if (record.skipped_reason.empty()) {
            record.executed_at_ns = now;
            last_step_ns_ = now;
        }
        ++next_step_;
        if (step.kind == LiteStep::Kind::CloseSession && record.executed() && !record.refused) {
            transcript_.runner_closed = true;
            transcript_.complete = true;
            finish(now);
            return;
        }
    }
}

void LiteProbeController::on_deadline(std::uint64_t now) {
    bool pending = false;
    if (!definition_.runner_setup.empty() && !transcript_.runner_setup.executed()) {
        pending = true;
        transcript_.runner_setup.skipped_reason = "deadline";
    }
    for (std::size_t i = next_step_; i < transcript_.steps.size(); ++i) {
        auto& record = transcript_.steps[i];
        pending = true;
        if (i != next_step_) continue;
        if (!record.gate_opened_ns && record.current_at_ns) {
            record.gate_expired = true;
        } else if (record.current_at_ns) {
            record.skipped_reason = "deadline";
        }
    }
    if (continuation_open()) pending = true;
    const bool done_missing = definition_.done && !definition_.done(session_);
    transcript_.timed_out = !transcript_.established || pending || done_missing;
    transcript_.complete = transcript_.established;
    finish(now);
}

bool LiteProbeController::poll() {
    if (ended_) return false;
    if (transcript_.harness_failed) {
        finish(clock_.now_ns());
        return false;
    }
    const auto now = clock_.now_ns();
    stale_ = true;  // the recorder may change below (peer events, runner writes)
    if (!started_) {
        started_ = true;
        transcript_.started_ns = now;
    }
    for (auto& event : transport_.poll(kLitePollBatch)) {
        record(event, now);
        if (!handle(event, now)) break;
    }
    if (transcript_.harness_failed) {
        finish(now);
        return false;
    }
    if (session_.limit_reached()) limit("the lite session recorder reached a limit");
    if (peer_closed_) {
        // The continuation sees the close (it may finish on it); steps it appends now can no longer run.
        if (transcript_.established && (definition_.runner_setup.empty() || transcript_.runner_setup.executed()))
            continue_steps(now);
        const bool setup_pending = !definition_.runner_setup.empty() && !transcript_.runner_setup.executed();
        transcript_.peer_closed_early = setup_pending || !steps_finished() || continuation_open();
        transcript_.complete = !transcript_.harness_failed;
        finish(now);
        return false;
    }
    if (local_closed_ || idle_timeout_) {
        // The connection ended without a peer close or a CloseSession step: nothing more can be observed.
        if (idle_timeout_) {
            transcript_.timed_out = true;
            transcript_.complete = true;
        } else {
            fail("the local transport closed the connection");
        }
        finish(now);
        return false;
    }
    if (transcript_.established) {
        auto& setup = transcript_.runner_setup;
        bool setup_ready = definition_.runner_setup.empty() || setup.executed();
        if (!setup_ready) {
            if (!setup.current_at_ns) setup.current_at_ns = setup.gate_opened_ns = now;
            LiteStep step;
            step.kind = LiteStep::Kind::SendUni;
            step.bytes = definition_.runner_setup;
            step.fin = setup.fin;
            step.label = setup.label;
            const auto progress = execute(step, setup, now);
            if (progress == Progress::Failed) {
                finish(now);
                return false;
            }
            if (progress == Progress::Done) {
                setup.executed_at_ns = now;
                setup_ready = true;
            }
        }
        if (setup_ready) {
            continue_steps(now);
            run_steps(now);
            if (ended_) return false;
            if (transcript_.harness_failed) {
                finish(now);
                return false;
            }
            continue_steps(now);
            if (definition_.done && definition_.done(session_)) {
                transcript_.complete = true;
                finish(now);
                return false;
            }
            if (definition_.observation_window && steps_finished() && !continuation_open()) {
                const auto since = last_step_ns_.value_or(transcript_.established_ns);
                if (now - since >= to_ns(*definition_.observation_window)) {
                    transcript_.complete = true;
                    finish(now);
                    return false;
                }
            }
        }
    }
    const auto base = transcript_.established ? transcript_.established_ns : transcript_.started_ns;
    const auto limit_ms = transcript_.established ? definition_.deadline
                                                  : definition_.connect_deadline.value_or(definition_.deadline);
    if (now - base >= to_ns(limit_ms)) {
        on_deadline(now);
        return false;
    }
    return true;
}

const LiteTranscript& LiteProbeController::transcript() const {
    if (stale_) {
        transcript_.streams = session_.streams();
        transcript_.peer_close = session_.peer_close();
        stale_ = false;
    }
    return transcript_;
}

}  // namespace moq::interop::scenarios
