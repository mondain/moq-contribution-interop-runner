#pragma once

#include "moq/interop/transport/session_transport.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moq::interop::scenarios {

using RawProbeClock = std::chrono::steady_clock;
// Credit, UniCredit, DropInbound and ResumeInbound are transport steps rather
// than streams and carry no bytes. Credit and UniCredit raise the peer's
// bidirectional or unidirectional stream limit by `application_error` (reused
// as the count). DropInbound discards everything the peer sends from then on
// (nothing is acknowledged) and ResumeInbound ends that.
enum class RawProbeChannel { NewUni, NewBidi, Control, Datagram, PeerBidi, Credit, UniCredit,
                             DropInbound, ResumeInbound };
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
    // The write waits until this long after the previous write (or SETUP, for
    // the first) was accepted; the proof checks the recorded acceptance times.
    std::chrono::milliseconds delay_after_previous{0};
    // STOP_SENDING aimed at a peer-initiated unidirectional stream. Only valid
    // with operation StopSending and no reuse_write_stream. The callback
    // returns no value until a stream qualifies; the first stream it names is
    // frozen, and the proof calls it again on the events seen at that point.
    std::function<std::optional<transport::StreamId>(const RawProbeGateInput&)> select_peer_stream{};
};
struct RawProbeTranscript;
// How the runner answers requests a publisher opens (draft 21, Sections 9.3, 9.4
// and 9.5). A REQUEST_OK without parameters accepts; REQUEST_ERROR UNINTERESTED
// followed by a FIN rejects (Section 6.4.2.3).
enum class RawProbePublishResponse {
    Ignore,
    Accept,
    Reject,
    // Reject only once an Object for the PUBLISH's Track Alias has been received,
    // so the publisher is known to have been producing.
    RejectAfterObject,
};
enum class RawProbeUpdateResponse {
    Ignore,
    Accept,
    // Accept at once, except that a message carrying an AUTHORIZATION TOKEN with
    // Alias Type USE_ALIAS is answered only after `hold`.
    HoldAliasUses,
};
struct RawProbeCourtesy {
    RawProbePublishResponse publish{RawProbePublishResponse::Ignore};
    // Responses to REQUEST_UPDATE messages the publisher sends on its requests.
    RawProbeUpdateResponse update{RawProbeUpdateResponse::Ignore};
    std::chrono::milliseconds hold{300};
};
enum class RawProbeCourtesyKind { PublishOk, PublishError, UpdateOk };
struct RawProbeCourtesyWrite {
    transport::StreamId stream_id{0};
    // Transport events observed when the response was fully accepted.
    std::size_t event_count{0};
    RawProbeCourtesyKind kind{RawProbeCourtesyKind::PublishOk};
};
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
    // Answers each parameter-free PUBLISH_NAMESPACE the publisher opens with
    // REQUEST_OK, as a subscriber must for the publisher to proceed (draft 18
    // Section 10.15). The answers are recorded in the transcript, are not part
    // of the stimulus, and cannot be combined with PeerBidi writes.
    bool acknowledge_publisher_namespace{false};
    // Initial QUIC credit for peer-initiated bidirectional streams. The
    // harness adds the WebTransport CONNECT stream where it applies.
    std::optional<std::uint64_t> initial_peer_bidi_streams{};
    // Initial credit for peer-initiated unidirectional streams beyond those the
    // session itself needs (MOQT control stream; WebTransport adds its three
    // HTTP/3 streams).
    std::optional<std::uint64_t> initial_peer_uni_streams{};
    // Bytes the publisher may write on a unidirectional data stream. With
    // `hold_uni_stream_credit` the runner never raises it, so a longer stream
    // stays open and unfinished.
    std::optional<std::uint64_t> initial_peer_uni_stream_data{};
    bool hold_uni_stream_credit{false};
    // The harness runs a second listener whose URI a write can name (through
    // RawProbeGateInput::replacement_uri) and records what connects to it in
    // RawProbeTranscript::replacement_events. Used for GOAWAY migration.
    bool offer_replacement_session{false};
    // Called by the run with the replacement listener's URI so a definition
    // can embed it in its writes (used with offer_replacement_session).
    std::function<void(RawProbeDefinition&, const std::string&)> bind_alternate_uri{};
    // Opt-in courtesy responses to requests the publisher opens on its own request
    // streams. They are not part of the scored stimulus and are never transcript
    // writes; each one is listed in RawProbeTranscript::courtesy_writes with the
    // transport event count at which it was fully accepted, so evaluators can
    // order it against what the publisher sent. Draft 21 only. PUBLISH_NAMESPACE
    // is acknowledged by `auto_accept_*`, not here.
    RawProbeCourtesy courtesy{};
    // The scenario withholds or rejects what the publisher asked for, so a publisher
    // that then exits with an error is part of what was observed, not a broken run.
    bool publisher_exit_is_evidence{false};
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
    // When the controller accepted the write, on the polling clock.
    std::optional<RawProbeClock::time_point> accepted_at{};
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
    // Set when the definition offers a replacement session: its URI and the
    // transport events of whatever connected there.
    std::optional<std::string> replacement_uri{};
    std::vector<transport::TransportEvent> replacement_events;
    // Token value the operator configured the publisher's authorization policy
    // to refuse (Section 8.9). Absent when no policy is controllable.
    std::optional<std::string> denied_authorization_token{};
    // Responses sent by RawProbeDefinition::courtesy, in the order accepted.
    std::vector<RawProbeCourtesyWrite> courtesy_writes;
};
bool raw_probe_stimulus_valid(const RawProbeTranscript& transcript,
                             const RawProbeDefinition& definition);
std::optional<bool> evaluate_raw_probe_close(
    const RawProbeTranscript& transcript, const RawProbeDefinition& definition,
    std::optional<std::uint64_t> expected_close);

class PublisherCourtesy;
class RawProbeController {
public:
    // `replacement` is the second listener of a replacement-session definition.
    RawProbeController(transport::SessionTransport& transport,
                       RawProbeDefinition definition,
                       transport::SessionTransport* replacement = nullptr,
                       std::string replacement_uri = {});
    ~RawProbeController();
    const RawProbeTranscript& poll(RawProbeClock::time_point now);
    const RawProbeTranscript& transcript() const noexcept;
private:
    bool flush(RawProbeAcceptedWrite& write);
    void send_auto_replies();
    void fail();
    transport::SessionTransport& transport_;
    transport::SessionTransport* replacement_;
    bool replacement_setup_sent_{false};
    bool session_closed_{false};
    RawProbeDefinition definition_;
    RawProbeTranscript transcript_;
    std::map<transport::StreamId, std::vector<std::byte>> peer_setup_candidates_;
    std::size_t peer_setup_bytes_count_{0};
    std::unique_ptr<PublisherCourtesy> courtesy_;
    std::map<transport::StreamId, std::vector<std::byte>> peer_request_candidates_;
    std::set<transport::StreamId> cancelled_peer_requests_;
    std::map<transport::StreamId, std::vector<std::byte>> auto_accept_candidates_;
    std::set<transport::StreamId> auto_accept_replied_;
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
