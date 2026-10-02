#pragma once

#include "moq/interop/transport/session_transport.h"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {

using RawProbeClock = std::chrono::steady_clock;
// Credit is a stream-credit step rather than a stream: it raises the peer's
// bidirectional stream limit by `application_error` (reused as the count) and
// carries no bytes.
enum class RawProbeChannel { NewUni, NewBidi, Control, Datagram, PeerBidi, Credit };
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
    // Answers each parameter-free PUBLISH_NAMESPACE the publisher opens with
    // REQUEST_OK, as a subscriber must for the publisher to proceed (draft 18
    // Section 10.15). The answers are recorded in the transcript, are not part
    // of the stimulus, and cannot be combined with PeerBidi writes.
    bool acknowledge_publisher_namespace{false};
    // Initial QUIC credit for peer-initiated bidirectional streams. The
    // harness adds the WebTransport CONNECT stream where it applies.
    std::optional<std::uint64_t> initial_peer_bidi_streams{};
    // The harness runs a second listener whose URI a write can name (through
    // RawProbeGateInput::replacement_uri) and records what connects to it in
    // RawProbeTranscript::replacement_events. Used for GOAWAY migration.
    bool offer_replacement_session{false};
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
    // The runner's replacement-session URI (see offer_replacement_session);
    // empty when the definition offers none.
    std::string_view replacement_uri{};
};
// A REQUEST_OK the controller wrote on a publisher-opened request stream.
struct RawProbeAcknowledgement {
    transport::StreamId stream_id{0};
    // Transport events observed when the answer was fully accepted.
    std::size_t event_count{0};
};
struct RawProbeTranscript {
    std::string scenario_id;
    std::vector<RawProbeAcknowledgement> acknowledgements;
    RawProbeAcceptedWrite setup;
    std::vector<RawProbeAcceptedWrite> writes;
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
    // Set when the definition offers a replacement session: its URI and the
    // transport events of whatever connected there.
    std::optional<std::string> replacement_uri{};
    std::vector<transport::TransportEvent> replacement_events;
};
bool raw_probe_stimulus_valid(const RawProbeTranscript& transcript,
                             const RawProbeDefinition& definition);
std::optional<bool> evaluate_raw_probe_close(
    const RawProbeTranscript& transcript, const RawProbeDefinition& definition,
    std::optional<std::uint64_t> expected_close);

class RawProbeController {
public:
    // `replacement` is the second listener of a replacement-session definition.
    RawProbeController(transport::SessionTransport& transport,
                       RawProbeDefinition definition,
                       transport::SessionTransport* replacement = nullptr,
                       std::string replacement_uri = {});
    const RawProbeTranscript& poll(RawProbeClock::time_point now);
    const RawProbeTranscript& transcript() const noexcept;
private:
    bool flush(RawProbeAcceptedWrite& write);
    void fail();
    transport::SessionTransport& transport_;
    transport::SessionTransport* replacement_;
    bool replacement_setup_sent_{false};
    bool session_closed_{false};
    RawProbeDefinition definition_;
    RawProbeTranscript transcript_;
    std::map<transport::StreamId, std::vector<std::byte>> peer_setup_candidates_;
    std::size_t peer_setup_bytes_count_{0};
    std::map<transport::StreamId, std::vector<std::byte>> peer_request_candidates_;
    std::set<transport::StreamId> cancelled_peer_requests_;
    std::optional<transport::StreamId> peer_request_stream_;
    std::size_t peer_request_bytes_count_{0};
    std::map<transport::StreamId, std::vector<std::byte>> acknowledgement_candidates_;
    std::set<transport::StreamId> acknowledgement_pending_;
    std::set<transport::StreamId> acknowledged_;
    void acknowledge_publisher_namespaces();
    std::optional<RawProbeClock::time_point> delivered_at_;
    std::optional<RawProbeClock::time_point> started_at_;
    std::size_t next_write_{0};
};
}  // namespace moq::interop::scenarios
