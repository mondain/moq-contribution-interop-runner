#pragma once

#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/draft18/messages.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace moq::interop::session {

enum class SessionPhase {
    AwaitingTransport,
    AwaitingSetup,
    Active,
    Closing,
    Closed,
};

enum class LocalStreamPurpose { Control, Request, Data };

enum class PeerStreamKind { Control, Request, Subgroup, Fetch, Padding };

enum class EvidenceKind {
    TransportEstablished,
    LocalStreamObserved,
    LocalSetupObserved,
    PeerStreamClassified,
    PeerSetupReceived,
    SetupOptionDuplicate,
    DeferredStreamBytes,
    PeerReset,
    PeerStopSending,
    PeerClose,
    LocalClose,
    IdleTimeout,
    TransportError,
    TransportEventOverflow,
    DraftAmbiguity,
    ProtocolViolation,
    HarnessLimit,
    LocalObservationError,
};

enum class HarnessLimitKind {
    ActiveStreams,
    EarlyStreams,
    EarlyBytes,
    PartialStreamBytes,
    PartialSessionBytes,
    EvidenceCount,
    EvidenceBytes,
};

struct TransportEstablishedEvidence {
    std::vector<std::byte> alpn;
    std::vector<std::byte> local_connection_id;
    std::vector<std::byte> peer_connection_id;
    std::size_t max_datagram_payload{0};
};

struct StreamEvidence {
    transport::StreamId stream_id{0};
    PeerStreamKind stream_kind{PeerStreamKind::Request};
};

struct LocalStreamEvidence {
    transport::StreamId stream_id{0};
    LocalStreamPurpose purpose{LocalStreamPurpose::Request};
};

struct SetupEvidence {
    transport::StreamId stream_id{0};
    wire::draft18::SetupMessage setup;
};

struct SetupOptionDuplicateEvidence {
    transport::StreamId stream_id{0};
    std::uint64_t option_type{0};
};

struct DeferredBytesEvidence {
    transport::StreamId stream_id{0};
    PeerStreamKind stream_kind{PeerStreamKind::Request};
    std::vector<std::byte> bytes;
    bool fin{false};
};

struct StreamErrorEvidence {
    transport::StreamId stream_id{0};
    std::uint64_t application_error{0};
};

struct CloseEvidence {
    transport::CloseErrorSpace error_space{
        transport::CloseErrorSpace::Transport};
    std::uint64_t error_code{0};
    std::vector<std::byte> reason;
};

struct TransportErrorEvidence {
    transport::TransportError error{transport::TransportError::InternalFailure};
};

struct MarkerEvidence {};

struct DraftAmbiguityEvidence {
    std::optional<transport::StreamId> stream_id;
    std::size_t offset{0};
    std::string detail;
};

struct ProtocolViolationEvidence {
    std::optional<transport::StreamId> stream_id;
    std::uint64_t application_error{0};
    std::vector<std::byte> reason;
};

struct HarnessLimitEvidence {
    HarnessLimitKind limit{HarnessLimitKind::ActiveStreams};
    std::size_t attempted{0};
    std::size_t maximum{0};
};

struct LocalObservationErrorEvidence {
    std::optional<transport::StreamId> stream_id;
};

using EvidenceData =
    std::variant<TransportEstablishedEvidence, StreamEvidence,
                 LocalStreamEvidence, SetupEvidence,
                 SetupOptionDuplicateEvidence, DeferredBytesEvidence,
                 StreamErrorEvidence, CloseEvidence, TransportErrorEvidence,
                 MarkerEvidence, DraftAmbiguityEvidence,
                 ProtocolViolationEvidence, HarnessLimitEvidence,
                 LocalObservationErrorEvidence>;

struct EvidenceEvent {
    std::uint64_t sequence{0};
    EvidenceKind kind{EvidenceKind::LocalObservationError};
    EvidenceData data{LocalObservationErrorEvidence{}};
};

struct SendMessageAction {
    transport::StreamId stream_id{0};
    wire::draft18::Message message;
    bool fin{false};
};

struct ResetStreamAction {
    transport::StreamId stream_id{0};
    std::uint64_t application_error{0};
};

struct StopSendingAction {
    transport::StreamId stream_id{0};
    std::uint64_t application_error{0};
};

struct CloseSessionAction {
    std::uint64_t application_error{0};
    std::vector<std::byte> reason;
};

using SessionAction =
    std::variant<SendMessageAction, ResetStreamAction, StopSendingAction,
                 CloseSessionAction>;

struct SessionTransition {
    std::vector<SessionAction> actions;
};

struct PublisherSessionConfig {
    std::size_t maximum_active_streams{256};
    std::size_t maximum_early_streams{32};
    std::size_t maximum_early_bytes{1u << 16};
    std::size_t maximum_partial_bytes_per_stream{1u << 16};
    std::size_t maximum_partial_bytes_per_session{1u << 20};
    std::size_t maximum_evidence_count{512};
    std::size_t maximum_evidence_bytes{1u << 20};
    wire::draft18::Limits wire_limits{};
};

class PublisherSession {
public:
    explicit PublisherSession(PublisherSessionConfig config = {});
    ~PublisherSession();

    PublisherSession(PublisherSession&&) noexcept;
    PublisherSession& operator=(PublisherSession&&) noexcept;
    PublisherSession(const PublisherSession&) = delete;
    PublisherSession& operator=(const PublisherSession&) = delete;

    SessionTransition on_event(const transport::TransportEvent& event);
    SessionTransition observe_local_stream(transport::StreamId stream_id,
                                           LocalStreamPurpose purpose);
    SessionTransition observe_local_message(
        transport::StreamId stream_id,
        const wire::draft18::Message& message, bool fin);
    std::vector<EvidenceEvent> take_evidence(std::size_t maximum);

    [[nodiscard]] SessionPhase phase() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::session
