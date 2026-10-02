#pragma once

#include "moq/interop/transport/session_transport.h"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <vector>

namespace moq::interop::scenarios {

using RawProbeClock = std::chrono::steady_clock;
enum class RawProbeChannel { NewUni, NewBidi, Control, Datagram, PeerBidi };
enum class RawProbeOperation { Write, StopSending };
struct RawProbeGateInput;
struct RawProbeWrite {
    RawProbeChannel channel{RawProbeChannel::NewBidi};
    std::vector<std::byte> bytes;
    bool fin{false};
    // Reuse a preceding bidi write's actual stream; a response gate consumes
    // only input observed after that write was fully accepted.
    std::optional<std::size_t> reuse_write_stream{};
    std::function<bool(std::span<const std::byte>)> peer_response_ready{};
    RawProbeOperation operation{RawProbeOperation::Write};
    std::uint64_t application_error{0};
    // Evaluated again during proof: callbacks must be deterministic and must
    // not retain these borrowed spans or rely on evidence beyond their prefix.
    std::function<bool(const RawProbeGateInput&)> evidence_ready{};
    // Returns no value while awaiting evidence. The first nonempty result is
    // frozen before opening a stream, and regenerated from its prefix in proof.
    std::function<std::optional<std::vector<std::byte>>(const RawProbeGateInput&)> prepare_bytes{};
    // STOP_SENDING aimed at a peer-initiated unidirectional stream. Only valid
    // with operation StopSending and no reuse_write_stream. The callback
    // returns no value until a stream qualifies; the first stream it names is
    // frozen, and the proof calls it again on the events seen at that point.
    std::function<std::optional<transport::StreamId>(const RawProbeGateInput&)> select_peer_stream{};
};
struct RawProbeTranscript;
struct RawProbeDefinition {
    std::string id;
    std::vector<std::byte> setup_bytes;
    std::vector<RawProbeWrite> writes;
    bool start_after_peer_setup{true};
    std::function<bool(std::span<const std::byte>)> peer_setup_ready;
    std::chrono::milliseconds deadline{1000};
    std::function<bool(const RawProbeTranscript&)> response_ready{};
    std::function<bool(std::span<const std::byte>)> peer_request_ready{};
    // Publisher-initiated requests answered with a fixed reply on their own
    // streams, so a publisher that waits for its earlier requests to be
    // accepted keeps going. Every peer-opened bidirectional stream whose
    // received bytes satisfy `auto_accept_ready` gets `auto_accept_reply`
    // once, after peer SETUP, up to `auto_accept_limit` streams. A stream that
    // `peer_request_ready` selects as the stimulus target is left to the
    // explicit writes.
    std::function<bool(std::span<const std::byte>)> auto_accept_ready{};
    std::vector<std::byte> auto_accept_reply{};
    std::size_t auto_accept_limit{0};
    // The probe sends the publisher to a second listener the runner controls
    // (a GOAWAY New Session URI). The runner opens that listener, tells the
    // definition its URI, and records what arrives there as alternate events.
    bool alternate_listener{false};
    std::function<void(RawProbeDefinition&, const std::string&)> bind_alternate_uri{};
};
struct RawProbeAutoReply {
    transport::StreamId stream_id{0};
    // Number of transport events observed when the reply was fully accepted.
    std::size_t delivery_event_count{0};
};
struct RawProbeAcceptedWrite {
    RawProbeWrite write;
    std::optional<transport::StreamId> stream_id;
    std::size_t accepted{0};
    bool fin_accepted{false};
    // Number of transport events observed at full byte/FIN or STOP acceptance.
    std::optional<std::size_t> delivery_event_count{};
    // STOP acceptance is distinct from successful zero-byte writes.
    bool operation_accepted{false};
    std::optional<std::size_t> prepared_event_count{};
};
struct RawProbeGateInput {
    std::span<const RawProbeAcceptedWrite> prior_writes;
    std::span<const transport::TransportEvent> events;
};
struct RawProbeTranscript {
    std::string scenario_id;
    RawProbeAcceptedWrite setup;
    std::vector<RawProbeAcceptedWrite> writes;
    std::vector<RawProbeAutoReply> auto_replies;
    bool transport_established{false};
    std::size_t max_datagram_payload{0};
    bool peer_setup_received{false};
    bool stimulus_delivered{false};
    bool complete{false};
    bool harness_failed{false};
    bool timed_out{false};
    std::optional<std::size_t> delivery_event_count;
    std::vector<transport::TransportEvent> events;
    std::optional<std::uint64_t> unknown_auth_token_alias_compatibility_code{};
    // The moqt:// URI the runner named for the publisher's connection.
    std::optional<std::string> connection_uri{};
    // Token value the operator configured the publisher's authorization policy
    // to refuse (Section 8.9). Absent when no policy is controllable.
    std::optional<std::string> denied_authorization_token{};
    // URI of the runner-controlled second listener and the transport events
    // observed there, in order. They are evidence only: the stimulus proof
    // concerns the first connection alone.
    std::optional<std::string> alternate_uri{};
    std::vector<transport::TransportEvent> alternate_events;
};
bool raw_probe_stimulus_valid(const RawProbeTranscript& transcript,
                             const RawProbeDefinition& definition);
std::optional<bool> evaluate_raw_probe_close(
    const RawProbeTranscript& transcript, const RawProbeDefinition& definition,
    std::optional<std::uint64_t> expected_close);

class RawProbeController {
public:
    RawProbeController(transport::SessionTransport& transport,
                       RawProbeDefinition definition,
                       transport::SessionTransport* alternate = nullptr);
    // Records the URI of the second listener so completion checks can use it.
    void set_alternate_uri(std::string uri) { transcript_.alternate_uri = std::move(uri); }
    const RawProbeTranscript& poll(RawProbeClock::time_point now);
    const RawProbeTranscript& transcript() const noexcept;
private:
    bool flush(RawProbeAcceptedWrite& write);
    void send_auto_replies();
    void fail();
    transport::SessionTransport& transport_;
    transport::SessionTransport* alternate_{nullptr};
    bool primary_closed_{false};
    RawProbeDefinition definition_;
    RawProbeTranscript transcript_;
    std::map<transport::StreamId, std::vector<std::byte>> peer_setup_candidates_;
    std::size_t peer_setup_bytes_count_{0};
    std::map<transport::StreamId, std::vector<std::byte>> peer_request_candidates_;
    std::set<transport::StreamId> cancelled_peer_requests_;
    std::map<transport::StreamId, std::vector<std::byte>> auto_accept_candidates_;
    std::set<transport::StreamId> auto_accept_replied_;
    std::optional<transport::StreamId> peer_request_stream_;
    std::size_t peer_request_bytes_count_{0};
    std::optional<RawProbeClock::time_point> delivered_at_;
    std::optional<RawProbeClock::time_point> started_at_;
    std::size_t next_write_{0};
};
}  // namespace moq::interop::scenarios
