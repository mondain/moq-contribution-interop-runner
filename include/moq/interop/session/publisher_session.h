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

enum class RequestInitiator { Peer, Local };

enum class RequestKind {
    Subscribe,
    Publish,
    Fetch,
    TrackStatus,
    PublishNamespace,
    SubscribeNamespace,
    SubscribeTracks,
};

enum class GoawayPlacement { Control, Request };

enum class RequestTerminalCause {
    PeerFin,
    LocalFin,
    PeerReset,
    PeerStopSending,
    LocalStopSending,
    ResponseError,
    SessionClosed,
};

enum class RequestPhase {
    AwaitingInitialResponse,
    Active,
    UpdateFailed,
    Terminal,
};

struct NamespaceKey {
    std::vector<std::vector<std::byte>> fields;
    bool operator==(const NamespaceKey&) const = default;
};

struct TrackKey {
    NamespaceKey name_space;
    std::vector<std::byte> track_name;
    bool operator==(const TrackKey&) const = default;
};

enum class LocalSubscriptionRole { Publisher, Subscriber };
enum class SubscriptionPhase { Pending, Established, Ending, Terminated };
enum class ReservedNamespaceCategory {
    Dot,
    SessionEmptyTrack,
    SessionUnrecognized,
};

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
    RequestObserved,
    RequestIdSequenceViolation,
    InitialResponseObserved,
    UpdateObserved,
    UpdateResponseObserved,
    GoawayObserved,
    RequestTerminal,
    ResponseViolation,
    RequestUpdateFailed,
    RequestStateViolation,
    RequestMessageObserved,
    SubscriptionCreated,
    SubscriptionPhaseChanged,
    DuplicateSubscription,
    OppositeRoleCoexistence,
    PendingSubscriptionReplaced,
    ReservedNamespaceRejected,
    ForwardStateChanged,
};

enum class HarnessLimitKind {
    ActiveStreams,
    EarlyStreams,
    EarlyBytes,
    PartialStreamBytes,
    PartialSessionBytes,
    EvidenceCount,
    EvidenceBytes,
    ActiveRequests,
    OutstandingUpdates,
    RequestHistory,
    LocalStreamHistory,
    ActiveSubscriptions,
    SubscriptionHistory,
    SubscriptionKeyBytes,
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

struct RequestObservedEvidence {
    RequestInitiator initiator{RequestInitiator::Peer};
    std::uint64_t request_id{0};
    RequestKind request_kind{RequestKind::Subscribe};
    transport::StreamId stream_id{0};
    wire::draft18::Message message;
};

struct RequestIdSequenceEvidence {
    RequestInitiator initiator{RequestInitiator::Peer};
    std::uint64_t expected{0};
    std::uint64_t observed{0};
};

struct InitialResponseEvidence {
    RequestInitiator responder{RequestInitiator::Peer};
    std::uint64_t original_request_id{0};
    RequestKind request_kind{RequestKind::Subscribe};
    transport::StreamId stream_id{0};
    wire::draft18::Message message;
};

struct UpdateObservedEvidence {
    RequestInitiator initiator{RequestInitiator::Peer};
    std::uint64_t update_request_id{0};
    std::uint64_t original_request_id{0};
    transport::StreamId stream_id{0};
    wire::draft18::RequestUpdateMessage update;
};

struct UpdateResponseEvidence {
    RequestInitiator responder{RequestInitiator::Peer};
    std::uint64_t original_request_id{0};
    std::optional<std::uint64_t> update_request_id;
    std::vector<std::uint64_t> candidate_update_ids;
    transport::StreamId stream_id{0};
    wire::draft18::Message message;
};

struct ResponseViolationEvidence {
    RequestInitiator responder{RequestInitiator::Peer};
    std::uint64_t original_request_id{0};
    transport::StreamId stream_id{0};
    wire::draft18::Message message;
};

struct GoawayEvidence {
    RequestInitiator sender{RequestInitiator::Peer};
    GoawayPlacement placement{GoawayPlacement::Control};
    transport::StreamId stream_id{0};
    std::optional<std::uint64_t> cutoff_request_id;
    wire::draft18::GoawayMessage message;
};

struct RequestTerminalEvidence {
    std::uint64_t original_request_id{0};
    transport::StreamId stream_id{0};
    RequestTerminalCause cause{RequestTerminalCause::SessionClosed};
    std::optional<std::uint64_t> application_error;
};

struct RequestUpdateFailedEvidence {
    std::uint64_t original_request_id{0};
    transport::StreamId stream_id{0};
    RequestKind request_kind{RequestKind::Subscribe};
    RequestInitiator update_initiator{RequestInitiator::Peer};
};

struct RequestStateViolationEvidence {
    RequestInitiator initiator{RequestInitiator::Peer};
    std::uint64_t original_request_id{0};
    transport::StreamId stream_id{0};
    wire::draft18::Message message;
};

struct RequestMessageObservedEvidence {
    RequestInitiator sender{RequestInitiator::Peer};
    std::uint64_t original_request_id{0};
    transport::StreamId stream_id{0};
    wire::draft18::Message message;
};

struct SubscriptionCreatedEvidence {
    TrackKey track;
    LocalSubscriptionRole local_role{LocalSubscriptionRole::Publisher};
    SubscriptionPhase phase{SubscriptionPhase::Pending};
    RequestInitiator initiator{RequestInitiator::Peer};
    std::uint64_t request_id{0};
    transport::StreamId stream_id{0};
    bool forward_state{true};
};

struct ForwardStateEvidence {
    std::uint64_t request_id{0};
    transport::StreamId stream_id{0};
    RequestInitiator actor{RequestInitiator::Peer};
    bool old_state{true};
    bool new_state{true};
};

struct SubscriptionPhaseEvidence {
    TrackKey track;
    LocalSubscriptionRole local_role{LocalSubscriptionRole::Publisher};
    std::uint64_t request_id{0};
    transport::StreamId stream_id{0};
    SubscriptionPhase old_phase{SubscriptionPhase::Pending};
    SubscriptionPhase new_phase{SubscriptionPhase::Terminated};
};

struct DuplicateSubscriptionEvidence {
    TrackKey track;
    LocalSubscriptionRole local_role{LocalSubscriptionRole::Publisher};
    std::uint64_t existing_request_id{0};
    std::uint64_t rejected_request_id{0};
    transport::StreamId rejected_stream_id{0};
};

struct OppositeRoleCoexistenceEvidence {
    TrackKey track;
    std::uint64_t existing_request_id{0};
    std::uint64_t admitted_request_id{0};
};

struct PendingSubscriptionReplacementEvidence {
    TrackKey track;
    std::uint64_t cancelled_request_id{0};
    transport::StreamId cancelled_stream_id{0};
    std::uint64_t admitted_request_id{0};
    transport::StreamId admitted_stream_id{0};
};

struct ReservedNamespaceEvidence {
    TrackKey track;
    ReservedNamespaceCategory category{ReservedNamespaceCategory::Dot};
    RequestInitiator initiator{RequestInitiator::Peer};
    std::uint64_t request_id{0};
    transport::StreamId stream_id{0};
};

using EvidenceData =
    std::variant<TransportEstablishedEvidence, StreamEvidence,
                 LocalStreamEvidence, SetupEvidence,
                 SetupOptionDuplicateEvidence, DeferredBytesEvidence,
                 StreamErrorEvidence, CloseEvidence, TransportErrorEvidence,
                 MarkerEvidence, DraftAmbiguityEvidence,
                 ProtocolViolationEvidence, HarnessLimitEvidence,
                 LocalObservationErrorEvidence, RequestObservedEvidence,
                 RequestIdSequenceEvidence, InitialResponseEvidence,
                 UpdateObservedEvidence, UpdateResponseEvidence,
                 ResponseViolationEvidence, GoawayEvidence,
                 RequestTerminalEvidence, RequestUpdateFailedEvidence,
                 RequestStateViolationEvidence,
                 RequestMessageObservedEvidence,
                 SubscriptionCreatedEvidence, SubscriptionPhaseEvidence,
                 DuplicateSubscriptionEvidence,
                 OppositeRoleCoexistenceEvidence,
                 PendingSubscriptionReplacementEvidence,
                 ReservedNamespaceEvidence, ForwardStateEvidence>;

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
    std::size_t maximum_active_requests{256};
    std::size_t maximum_outstanding_updates_per_request{64};
    std::size_t maximum_request_history{4'096};
    std::size_t maximum_local_stream_history{4'096};
    std::size_t maximum_active_subscriptions{256};
    std::size_t maximum_subscription_history{4'096};
    std::size_t maximum_subscription_key_bytes{1u << 20};
    std::size_t maximum_auth_token_cache_bytes{1u << 20};
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
    SessionTransition observe_local_stop_sending(
        transport::StreamId stream_id, std::uint64_t application_error);
    SessionTransition observe_local_fin(transport::StreamId stream_id);
    std::vector<EvidenceEvent> take_evidence(std::size_t maximum);

    [[nodiscard]] SessionPhase phase() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::session
