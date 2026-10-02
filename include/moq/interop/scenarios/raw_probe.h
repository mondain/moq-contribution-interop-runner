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
#include <utility>
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
    // Acknowledge each PUBLISH_NAMESPACE with REQUEST_OK.
    bool acknowledge_namespaces{false};
    RawProbePublishResponse publish{RawProbePublishResponse::Ignore};
    // Responses to REQUEST_UPDATE messages the publisher sends on its requests.
    RawProbeUpdateResponse update{RawProbeUpdateResponse::Ignore};
    std::chrono::milliseconds hold{300};
};
enum class RawProbeCourtesyKind { NamespaceOk, PublishOk, PublishError, UpdateOk };
struct RawProbeCourtesyWrite {
    transport::StreamId stream_id{0};
    // Transport events observed when the response was fully accepted.
    std::size_t event_count{0};
    RawProbeCourtesyKind kind{RawProbeCourtesyKind::NamespaceOk};
};
// Transport credit the runner advertises for the context, so a scenario can
// bound what the publisher may open. Unset values keep the listener defaults.
struct RawProbeListenerLimits {
    // Publisher-opened bidirectional streams available to MOQT request streams.
    std::optional<std::uint64_t> max_streams_bidi{};
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
    // Opt-in courtesy responses to requests the publisher opens on its own request
    // streams. They are not part of the scored stimulus and are never transcript
    // writes; each one is listed in RawProbeTranscript::courtesy_writes with the
    // transport event count at which it was fully accepted, so evaluators can
    // order it against what the publisher sent. Draft 21 only.
    RawProbeCourtesy courtesy{};
    RawProbeListenerLimits listener_limits{};
    // The scenario withholds or rejects what the publisher asked for, so a publisher
    // that then exits with an error is part of what was observed, not a broken run.
    bool publisher_exit_is_evidence{false};
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
    RawProbeController(transport::SessionTransport& transport,
                       RawProbeDefinition definition);
    ~RawProbeController();
    const RawProbeTranscript& poll(RawProbeClock::time_point now);
    const RawProbeTranscript& transcript() const noexcept;
private:
    bool flush(RawProbeAcceptedWrite& write);
    void fail();
    transport::SessionTransport& transport_;
    RawProbeDefinition definition_;
    RawProbeTranscript transcript_;
    std::map<transport::StreamId, std::vector<std::byte>> peer_setup_candidates_;
    std::size_t peer_setup_bytes_count_{0};
    std::unique_ptr<PublisherCourtesy> courtesy_;
    std::map<transport::StreamId, std::vector<std::byte>> peer_request_candidates_;
    std::set<transport::StreamId> cancelled_peer_requests_;
    std::optional<transport::StreamId> peer_request_stream_;
    std::size_t peer_request_bytes_count_{0};
    std::optional<RawProbeClock::time_point> delivered_at_;
    std::optional<RawProbeClock::time_point> started_at_;
    std::size_t next_write_{0};
};
}  // namespace moq::interop::scenarios
