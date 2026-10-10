#pragma once

// The moq-lite-06 probe engine: the runner (QUIC/WebTransport server, playing the SUBSCRIBER) waits for the
// publisher's connection, sends its own Setup stream, then runs an ordered list of steps (writes on new or earlier
// streams, FINs, resets, STOP_SENDINGs, a session close) with gates and delays, while every transport event is fed
// to a session::LiteSession recorder. The result is a LiteTranscript that evaluators judge.
//
// Time comes from an injectable LiteClock (nanoseconds), so tests run on a manual clock and never sleep; the same
// script on the same clock always yields the same transcript.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "moq/interop/session/lite_session.h"
#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/subscribe.h"

namespace moq::interop::scenarios {

// The ALPN (and WebTransport subprotocol) a moq-lite-06 publisher must negotiate.
inline constexpr std::string_view kLiteAlpn = "moq-lite-06";

// Bounds on what one context records into LiteTranscript::events. Reaching either sets
// LiteTranscript::event_limit_reached; recording stops but stepping goes on, and the transcript is never judged.
// The byte bound counts the stream-data bytes KEPT in the events: the payload of the peer's Group streams is not
// kept (LiteTranscript::payload_bytes_dropped), so media volume never reaches it; every other stream's bytes
// (Setup streams, the publisher's answers on runner bidirectional streams, unknown stream types) count in full.
inline constexpr std::size_t kLiteMaximumEvents = 16384;
inline constexpr std::size_t kLiteMaximumEvidenceBytes = std::size_t{8} << 20;
// Steps (static plus dynamically appended) one probe may hold; more is a definition error (harness_failed).
inline constexpr std::size_t kLiteMaximumSteps = 1024;
// Events taken from the transport per poll.
inline constexpr std::size_t kLitePollBatch = 256;

// The engine's time source, in nanoseconds on a monotonic scale. The name avoids scenarios::Clock (engine.h), which
// is a std::chrono clock type rather than an injectable object.
class LiteClock {
public:
    virtual ~LiteClock() = default;
    virtual std::uint64_t now_ns() = 0;
};

// std::chrono::steady_clock, for live runs.
class SteadyLiteClock final : public LiteClock {
public:
    std::uint64_t now_ns() override;
};

// A clock that moves only when told to, for deterministic tests.
class ManualLiteClock final : public LiteClock {
public:
    explicit ManualLiteClock(std::uint64_t start_ns = 0) : now_(start_ns) {}
    std::uint64_t now_ns() override { return now_; }
    void advance(std::chrono::nanoseconds by) { now_ += static_cast<std::uint64_t>(by.count()); }
    void set(std::uint64_t now_ns) { now_ = now_ns; }

private:
    std::uint64_t now_;
};

// One runner action; steps run strictly in order. A step becomes current when the previous one has executed or
// was skipped. Its gate (if any) is evaluated on every poll from then on; once the gate is open the step waits
// `delay`, then executes. A gate still closed `gate_deadline` after the step became current (0: the probe deadline)
// skips the step with gate_expired set.
struct LiteStep {
    enum class Kind {
        SendUni,          // open a unidirectional stream and write `bytes` (FIN if `fin`)
        OpenBidiAndSend,  // open a bidirectional stream and write `bytes` (FIN if `fin`)
        SendOnStream,     // write `bytes` (FIN if `fin`) on the stream of step `stream_ref` or `target`
        FinStream,        // FIN the stream of step `stream_ref` or `target`
        ResetStream,      // RESET_STREAM with `code`
        StopSending,      // STOP_SENDING with `code`
        CloseSession,     // close the session with application error `code` and `reason`; ends the probe
        Wait,             // no action: only the gate and the delay
        Mark,             // no action: records a labelled instant
    } kind{Kind::Mark};
    std::vector<std::byte> bytes;
    bool fin{false};
    // Index of an earlier SendUni / OpenBidiAndSend step whose stream this step acts on.
    std::optional<std::size_t> stream_ref;
    std::uint64_t code{0};
    std::string reason;
    std::function<bool(const session::LiteSession&)> gate;
    std::chrono::milliseconds delay{0};
    std::chrono::milliseconds gate_deadline{0};
    std::string label;
    // Instead of stream_ref, a stream picked from the recorder when the step executes (for example a peer Group
    // stream to STOP_SENDING). While it returns nullopt the step waits (bounded by the probe deadline).
    std::function<std::optional<transport::StreamId>(const session::LiteSession&)> target;
};

// What happened to one step.
struct LiteStepRecord {
    std::size_t index{0};
    std::string label;
    LiteStep::Kind kind{LiteStep::Kind::Mark};
    bool dynamic{false};  // appended by LiteProbeDefinition::next_steps
    std::optional<transport::StreamId> stream_id;
    std::optional<std::uint64_t> current_at_ns;   // became the current step
    std::optional<std::uint64_t> gate_opened_ns;  // gate found open (immediately when there is none)
    std::optional<std::uint64_t> executed_at_ns;  // fully accepted by the transport (or a no-action step reached)
    bool gate_expired{false};
    // Why a step never executed although its gate was open (deadline, referenced step has no stream, ...).
    std::string skipped_reason;
    // The transport refused the action because of the peer (PeerReset, PeerStopped, ConnectionClosed): the step is
    // over (executed_at_ns set) but not delivered in full.
    std::optional<transport::TransportStatus> refused;
    std::vector<std::byte> bytes;
    bool fin{false};
    std::uint64_t code{0};
    std::string reason;
    std::size_t accepted{0};
    bool fin_accepted{false};

    [[nodiscard]] bool executed() const noexcept { return executed_at_ns.has_value(); }
    [[nodiscard]] bool delivered() const noexcept { return executed_at_ns.has_value() && !refused.has_value(); }
};

// Handed to LiteProbeDefinition::next_steps. `values` and `finished` persist between calls.
struct LiteProbeContext {
    std::uint64_t now_ns{0};
    std::uint64_t established_ns{0};
    const std::vector<LiteStepRecord>* steps{nullptr};  // every step so far; an index is a valid stream_ref
    bool finished{false};  // set by next_steps when it will append nothing more
    std::map<std::string, std::uint64_t> values;  // scratch state for the continuation

    [[nodiscard]] std::size_t step_count() const noexcept { return steps ? steps->size() : 0; }
    [[nodiscard]] std::optional<transport::StreamId> stream_of(std::size_t step) const;
};

// Default bytes of the runner's Setup stream: STREAM_TYPE 0x1 then a SETUP with an empty parameter list.
std::vector<std::byte> lite_default_runner_setup();

// The binding the publisher's session runs on (draft 4.2): raw QUIC (binding 1) or WebTransport (binding 2).
// Unknown when the caller did not say (unit tests); evaluators judge Unknown like either binding unless a row is
// restricted to one binding. Task 9 sets it from the transport.
enum class LiteBinding { Unknown, NativeQuic, WebTransport };

struct LiteProbeDefinition {
    std::string id;
    bool requires_track{false};
    LiteBinding binding{LiteBinding::Unknown};
    // Set when the session URL the publisher was given carried a path (Task 9 sets it; the client-path evaluators
    // read it from the transcript).
    bool session_url_has_path{false};
    // That URL's path-abempty component and its query component (without the '?'), as given to the publisher; the
    // client-path evaluators (rows 120, 124, 125) rebuild the expected SETUP Path value from them. Empty when the run
    // gave the publisher no such URL. Copied to the transcript. Task 9 sets session_url_has_path, these two and
    // binding consistently: the flag is redundant with a non-empty path but is not derived from it.
    std::string session_url_path;
    std::string session_url_query;
    // The track fixture (plan decision (d)): RunConfig::track namespace fields joined with '/' are the publisher's
    // configured broadcast path, track_name its track. Empty when the scenario needs no fixture. Copied to the
    // transcript so evaluators can rebuild the stimulus and judge coverage.
    std::string broadcast_path;
    std::string track_name;
    // The runner's Setup stream; an empty vector sends none (named deliberate violation probes only).
    std::vector<std::byte> runner_setup = lite_default_runner_setup();
    // Draft 6.3.1: the opener sends one SETUP and immediately FINs. False only for deliberate probes.
    bool runner_setup_fin{true};
    // Optional dynamic continuation, called once after establishment and then whenever the recorder changed: new
    // decoded messages, new streams, a new peer RESET_STREAM or STOP_SENDING, or the peer's close. The steps it
    // returns are appended in order. Until it sets LiteProbeContext::finished the continuation counts as pending:
    // the deadline then sets timed_out, stimulus_delivered stays false and a peer close is peer_closed_early.
    std::function<std::vector<LiteStep>(const session::LiteSession&, LiteProbeContext&)> next_steps;
    std::vector<LiteStep> steps;
    // Bounds the probe from establishment (the clock restarts when the connection is established).
    std::chrono::milliseconds deadline{5000};
    // Bounds ONLY the wait for the connection, from the first poll; nullopt: `deadline`.
    std::optional<std::chrono::milliseconds> connect_deadline;
    // Early completion. Put here only a condition WITHOUT which the transcript cannot be judged: the deadline
    // passing before it holds sets timed_out (judgeable() false). When the ABSENCE of a reaction is the verdict,
    // leave it out and use observation_window or the deadline.
    std::function<bool(const session::LiteSession&)> done;
    // When set: complete this long after the last step executed (and, with next_steps, once the context says
    // finished), if the deadline has not come first.
    std::optional<std::chrono::milliseconds> observation_window;
    session::LiteSessionLimits limits{};
};

struct LiteTranscript {
    std::string scenario_id;
    bool session_url_has_path{false};
    std::string session_url_path;               // copied from the definition
    std::string session_url_query;              // copied from the definition
    LiteBinding binding{LiteBinding::Unknown};  // copied from the definition
    std::string broadcast_path;                 // copied from the definition (the track fixture)
    std::string track_name;                     // copied from the definition (the track fixture)
    // Value copies of the recorder's state; evaluators use the session::peer_* accessors on these records.
    std::vector<session::LiteStreamRecord> streams;
    std::optional<session::PeerCloseInfo> peer_close;
    // The runner's Setup stream (label "runner-setup", kind SendUni; never executed when none was sent).
    LiteStepRecord runner_setup;
    std::vector<LiteStepRecord> steps;
    // Every transport event, bounded by kLiteMaximumEvents; stream data bytes count against
    // kLiteMaximumEvidenceBytes (datagram payloads are recorded but not counted: lite uses no datagrams and the
    // transport bounds each one to the path MTU, so they are bounded by the event count).
    //
    // A StreamDataEvent on a stream the PEER opened unidirectionally whose STREAM_TYPE is Group (0x0) is kept with
    // its stream id and FIN but WITHOUT its bytes (data empty); event_data_sizes holds how many bytes it carried.
    // The recorder (streams) decoded those bytes before they were dropped, so GROUP and FRAME messages are intact;
    // only a reader of raw StreamDataEvent bytes would miss them, and the evaluators read raw bytes of the peer's
    // Setup stream only (lite06::peer_setup_message). Setup streams, runner-opened bidirectional streams and every
    // other stream keep their bytes.
    std::vector<transport::TransportEvent> events;
    std::vector<std::uint64_t> event_times;
    // Parallel to events: the stream-data bytes each StreamDataEvent carried as received (kept or not), 0 for every
    // other event.
    std::vector<std::size_t> event_data_sizes;
    // Group payload bytes received but not kept in events (above), and how many events lost theirs.
    std::size_t payload_bytes_dropped{0};
    std::size_t payload_events_elided{0};
    bool established{false};
    std::string alpn;
    bool complete{false};
    bool harness_failed{false};
    std::string harness_failure_reason;
    // The deadline passed before the probe reached what it needed: no connection, `done` never held, or steps
    // still pending.
    bool timed_out{false};
    bool event_limit_reached{false};
    std::string event_limit_reason;
    // The peer closed the session before the runner Setup and every step had executed (or while the next_steps
    // continuation was still open).
    bool peer_closed_early{false};
    // The runner closed the session (a CloseSession step).
    bool runner_closed{false};
    // The runner Setup (if any) and every step executed and were delivered in full, and the next_steps
    // continuation (if any) finished.
    bool stimulus_delivered{false};
    std::uint64_t started_ns{0};
    std::uint64_t established_ns{0};
    std::uint64_t ended_ns{0};
};

// False when no verdict may be drawn from the transcript: never established, not complete, harness_failed,
// event_limit_reached, timed_out, or any stream with a session::harness_issues() entry.
//
// The single rule for Harness-class issues (Tasks 4-7 rely on it; session::harness_issues implements it): every
// Harness-class issue raised by the PEER's bytes makes the transcript unjudgeable, and so do the runner-side
// anomalies session::is_runner_anomaly() names (trailing_after_fin, offset_overflow, undeclared_runner_stream,
// local_bidi_mismatch, message_limit_reached, buffer_limit_reached). The only Harness-class issues ignored are
// those the runner's own deliberately malformed probe bytes raise (length_exceeds_limit,
// length_not_representable from the runner): they are the stimulus, not a harness fault. Runner-origin
// PeerProtocol issues (e.g. a malformed SUBSCRIBE probe) are likewise the stimulus and never block.
//
// Use judgeable() alone only in evaluators where the peer's close IS the observation (a close probe may end the
// session before every step ran). Every other evaluator gates on judgeable_with_stimulus().
bool judgeable(const LiteTranscript& transcript);

// The DEFAULT evaluator gate: judgeable() and stimulus_delivered and not peer_closed_early.
bool judgeable_with_stimulus(const LiteTranscript& transcript);

// Runner stream bytes: STREAM_TYPE then the request.
std::vector<std::byte> lite_announce_stream_bytes(const wire::moqlite06::AnnounceRequest& request);
std::vector<std::byte> lite_subscribe_stream_bytes(const wire::moqlite06::Subscribe& subscribe);

// Step builders.
LiteStep lite_send_uni(std::vector<std::byte> bytes, bool fin, std::string label);
LiteStep lite_open_bidi(std::vector<std::byte> bytes, bool fin, std::string label);
LiteStep lite_send_on(std::size_t stream_ref, std::vector<std::byte> bytes, bool fin, std::string label);
LiteStep lite_fin(std::size_t stream_ref, std::string label);
LiteStep lite_reset(std::size_t stream_ref, std::uint64_t code, std::string label);
LiteStep lite_stop_sending(std::size_t stream_ref, std::uint64_t code, std::string label);
LiteStep lite_close(std::uint64_t code, std::string reason, std::string label);
LiteStep lite_wait(std::chrono::milliseconds delay, std::string label);
LiteStep lite_mark(std::string label);

std::string_view to_string(LiteStep::Kind kind);

class LiteProbeController {
public:
    LiteProbeController(LiteProbeDefinition definition, transport::SessionTransport& transport, LiteClock& clock);
    LiteProbeController(const LiteProbeController&) = delete;
    LiteProbeController& operator=(const LiteProbeController&) = delete;

    // True while running; drives events, steps and deadlines. Never blocks, never throws on peer input.
    bool poll();
    // The transcript; the stream records are refreshed from the recorder on access. NOT thread-safe: although
    // const, it writes the cached copy, so call it only from the thread that polls (or after polling stopped).
    [[nodiscard]] const LiteTranscript& transcript() const;
    [[nodiscard]] const session::LiteSession& session() const noexcept { return session_; }
    [[nodiscard]] bool running() const noexcept { return !ended_; }

private:
    enum class Progress { Done, Wait, Failed };

    void fail(std::string reason);
    void limit(std::string reason);
    void finish(std::uint64_t now);
    void record(const transport::TransportEvent& event, std::uint64_t now);
    // True for data of a peer unidirectional stream whose STREAM_TYPE is (or, on its first bytes, reads as) Group.
    [[nodiscard]] bool is_peer_group_data(const transport::StreamDataEvent& data) const;
    bool handle(const transport::TransportEvent& event, std::uint64_t now);
    void run_steps(std::uint64_t now);
    void continue_steps(std::uint64_t now);
    Progress execute(const LiteStep& step, LiteStepRecord& record, std::uint64_t now);
    Progress write_all(LiteStepRecord& record, std::uint64_t now);
    std::optional<transport::StreamId> resolve_stream(const LiteStep& step, LiteStepRecord& record, bool& skip);
    [[nodiscard]] bool steps_finished() const noexcept;
    [[nodiscard]] bool continuation_open() const noexcept;
    void on_deadline(std::uint64_t now);

    LiteProbeDefinition definition_;
    transport::SessionTransport& transport_;
    LiteClock& clock_;
    session::LiteSession session_;
    mutable LiteTranscript transcript_;
    mutable bool stale_{true};
    std::vector<LiteStep> steps_;
    LiteProbeContext context_;
    std::size_t next_step_{0};
    std::size_t evidence_bytes_{0};
    bool started_{false};
    bool ended_{false};
    bool peer_closed_{false};
    bool local_closed_{false};
    bool idle_timeout_{false};
    std::optional<std::uint64_t> last_step_ns_;
    // (decoded messages, streams, peer resets + stop-sendings, peer close) when next_steps was last called.
    std::optional<std::array<std::size_t, 4>> continuation_seen_;
};

}  // namespace moq::interop::scenarios
