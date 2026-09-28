#include "moq/interop/session/publisher_session.h"

#include "session/draft18_dispatch_internal.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moq::interop::session {
namespace {

constexpr std::uint64_t kInternalError = 0x1;
constexpr std::uint64_t kProtocolViolation = 0x3;
constexpr std::uint64_t kKeyValueFormattingError = 0x6;

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::vector<std::byte> reason_bytes(const char* value) {
    std::vector<std::byte> result;
    while (*value != '\0') {
        result.push_back(static_cast<std::byte>(*value));
        ++value;
    }
    return result;
}

std::size_t key_value_bytes(const wire::draft18::KeyValuePair& pair) {
    return std::visit(
        Overloaded{
            [](const wire::draft18::VarIntValue& value) {
                return value.raw_bytes.size();
            },
            [](const wire::draft18::ByteValue& value) {
                return value.bytes.size();
            }},
        pair.value);
}

std::size_t key_value_pairs_bytes(
    const wire::draft18::KeyValuePairs& pairs) {
    std::size_t result = 0;
    for (const auto& pair : pairs) result += key_value_bytes(pair);
    return result;
}

std::size_t track_namespace_bytes(
    const wire::draft18::TrackNamespace& value) {
    std::size_t result = 0;
    for (const auto& field : value.fields) result += field.size();
    return result;
}

std::size_t parameter_bytes(const wire::draft18::Parameter& parameter) {
    return std::visit(
        Overloaded{
            [](const wire::draft18::Token& value) {
                return value.token_value.size();
            },
            [](const wire::draft18::TrackNamespace& value) {
                return track_namespace_bytes(value);
            },
            [](const auto&) -> std::size_t { return 0; }},
        parameter.value);
}

std::size_t parameters_bytes(const wire::draft18::Parameters& parameters) {
    std::size_t result = 0;
    for (const auto& parameter : parameters) {
        result += parameter_bytes(parameter);
    }
    return result;
}

std::size_t message_owned_bytes(const wire::draft18::Message& message) {
    using namespace wire::draft18;
    return std::visit(
        Overloaded{
            [](const SetupMessage& value) {
                return key_value_pairs_bytes(value.options);
            },
            [](const GoawayMessage& value) {
                return value.new_session_uri.size();
            },
            [](const SubscribeOkMessage& value) {
                return parameters_bytes(value.parameters) +
                       key_value_pairs_bytes(value.track_properties.entries);
            },
            [](const RequestOkMessage& value) {
                return parameters_bytes(value.parameters) +
                       key_value_pairs_bytes(value.track_properties.entries);
            },
            [](const RequestErrorMessage& value) {
                std::size_t result = value.reason_phrase.bytes.size();
                if (value.redirect) {
                    result += value.redirect->connect_uri.size() +
                              track_namespace_bytes(
                                  value.redirect->track_namespace) +
                              value.redirect->track_name.bytes.size();
                }
                return result;
            },
            [](const RequestUpdateMessage& value) {
                return parameters_bytes(value.parameters);
            },
            [](const PublishDoneMessage& value) {
                return value.reason_phrase.bytes.size();
            },
            [](const FetchOkMessage& value) {
                return parameters_bytes(value.parameters) +
                       key_value_pairs_bytes(value.track_properties.entries);
            },
            [](const NamespaceMessage& value) {
                return track_namespace_bytes(value.track_namespace_suffix);
            },
            [](const NamespaceDoneMessage& value) {
                return track_namespace_bytes(value.track_namespace_suffix);
            },
            [](const PublishBlockedMessage& value) {
                return track_namespace_bytes(value.track_namespace_suffix) +
                       value.track_name.bytes.size();
            },
            [](const SubscribeMessage& value) {
                return track_namespace_bytes(value.track_namespace) +
                       value.track_name.bytes.size() +
                       parameters_bytes(value.parameters);
            },
            [](const PublishMessage& value) {
                return track_namespace_bytes(value.track_namespace) +
                       value.track_name.bytes.size() +
                       parameters_bytes(value.parameters) +
                       key_value_pairs_bytes(value.track_properties.entries);
            },
            [](const FetchMessage& value) {
                std::size_t result = parameters_bytes(value.parameters);
                if (const auto* standalone =
                        std::get_if<StandaloneFetch>(&value.fetch)) {
                    result += track_namespace_bytes(
                                  standalone->track_namespace) +
                              standalone->track_name.bytes.size();
                }
                return result;
            },
            [](const TrackStatusMessage& value) {
                return track_namespace_bytes(value.track_namespace) +
                       value.track_name.bytes.size() +
                       parameters_bytes(value.parameters);
            },
            [](const PublishNamespaceMessage& value) {
                return track_namespace_bytes(value.track_namespace) +
                       parameters_bytes(value.parameters);
            },
            [](const SubscribeNamespaceMessage& value) {
                return track_namespace_bytes(value.track_namespace_prefix) +
                       parameters_bytes(value.parameters);
            },
            [](const SubscribeTracksMessage& value) {
                return track_namespace_bytes(value.track_namespace_prefix) +
                       parameters_bytes(value.parameters);
            }},
        message);
}

std::size_t evidence_owned_bytes(const EvidenceData& data) {
    return std::visit(
        Overloaded{
            [](const TransportEstablishedEvidence& value) {
                return value.alpn.size() + value.local_connection_id.size() +
                       value.peer_connection_id.size();
            },
            [](const SetupEvidence& value) {
                std::size_t size = 0;
                for (const auto& option : value.setup.options) {
                    size += key_value_bytes(option);
                }
                return size;
            },
            [](const DeferredBytesEvidence& value) {
                return value.bytes.size();
            },
            [](const CloseEvidence& value) { return value.reason.size(); },
            [](const ProtocolViolationEvidence& value) {
                return value.reason.size();
            },
            [](const DraftAmbiguityEvidence& value) {
                return value.detail.size();
            },
            [](const RequestObservedEvidence& value) {
                return message_owned_bytes(value.message);
            },
            [](const InitialResponseEvidence& value) {
                return message_owned_bytes(value.message);
            },
            [](const UpdateObservedEvidence& value) {
                return parameters_bytes(value.update.parameters);
            },
            [](const UpdateResponseEvidence& value) {
                return value.candidate_update_ids.size() *
                           sizeof(std::uint64_t) +
                       message_owned_bytes(value.message);
            },
            [](const ResponseViolationEvidence& value) {
                return message_owned_bytes(value.message);
            },
            [](const GoawayEvidence& value) {
                return value.message.new_session_uri.size();
            },
            [](const RequestStateViolationEvidence& value) {
                return message_owned_bytes(value.message);
            },
            [](const RequestMessageObservedEvidence& value) {
                return message_owned_bytes(value.message);
            },
            [](const auto&) -> std::size_t { return 0; }},
        data);
}

void bound_terminal_owned_bytes(EvidenceData& data, std::size_t maximum) {
    std::visit(
        Overloaded{
            [maximum](CloseEvidence& value) {
                if (value.reason.size() > maximum) {
                    value.reason.resize(maximum);
                }
            },
            [maximum](ProtocolViolationEvidence& value) {
                if (value.reason.size() > maximum) {
                    value.reason.resize(maximum);
                }
            },
            [](auto&) {}},
        data);
}

CloseEvidence make_bounded_close_evidence(
    transport::CloseErrorSpace error_space, std::uint64_t error_code,
    std::span<const std::byte> reason, std::size_t maximum_reason_bytes) {
    const auto retained =
        reason.first(std::min(reason.size(), maximum_reason_bytes));
    return CloseEvidence{error_space, error_code,
                         {retained.begin(), retained.end()}};
}

bool valid_config(const PublisherSessionConfig& config) {
    return config.maximum_active_streams != 0 &&
           config.maximum_early_streams != 0 &&
           config.maximum_early_bytes != 0 &&
           config.maximum_partial_bytes_per_stream != 0 &&
           config.maximum_partial_bytes_per_session != 0 &&
           config.maximum_evidence_count != 0 &&
           config.maximum_evidence_bytes != 0 &&
           config.maximum_active_requests != 0 &&
           config.maximum_outstanding_updates_per_request != 0 &&
           config.maximum_request_history != 0 &&
           config.maximum_local_stream_history != 0;
}

bool peer_initiated(transport::StreamId stream_id) {
    const auto type = stream_id & 0x3u;
    return type == 0u || type == 2u;
}

bool unidirectional(transport::StreamId stream_id) {
    return (stream_id & 0x2u) != 0u;
}

std::optional<std::pair<std::uint64_t, RequestKind>> opening_request(
    const wire::draft18::Message& message) {
    return std::visit(
        Overloaded{
            [](const wire::draft18::SubscribeMessage& value)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return {{value.request_id, RequestKind::Subscribe}};
            },
            [](const wire::draft18::PublishMessage& value)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return {{value.request_id, RequestKind::Publish}};
            },
            [](const wire::draft18::FetchMessage& value)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return {{value.request_id, RequestKind::Fetch}};
            },
            [](const wire::draft18::TrackStatusMessage& value)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return {{value.request_id, RequestKind::TrackStatus}};
            },
            [](const wire::draft18::PublishNamespaceMessage& value)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return {{value.request_id, RequestKind::PublishNamespace}};
            },
            [](const wire::draft18::SubscribeNamespaceMessage& value)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return {{value.request_id, RequestKind::SubscribeNamespace}};
            },
            [](const wire::draft18::SubscribeTracksMessage& value)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return {{value.request_id, RequestKind::SubscribeTracks}};
            },
            [](const auto&)
                -> std::optional<std::pair<std::uint64_t, RequestKind>> {
                return std::nullopt;
            }},
        message);
}

bool response_message(const wire::draft18::Message& message) {
    return std::holds_alternative<wire::draft18::SubscribeOkMessage>(message) ||
           std::holds_alternative<wire::draft18::FetchOkMessage>(message) ||
           std::holds_alternative<wire::draft18::RequestOkMessage>(message) ||
           std::holds_alternative<wire::draft18::RequestErrorMessage>(message);
}

bool valid_success_response(RequestKind kind,
                            const wire::draft18::Message& message) {
    if (kind == RequestKind::Subscribe) {
        return std::holds_alternative<wire::draft18::SubscribeOkMessage>(
            message);
    }
    if (kind == RequestKind::Fetch) {
        return std::holds_alternative<wire::draft18::FetchOkMessage>(message);
    }
    return std::holds_alternative<wire::draft18::RequestOkMessage>(message);
}

RequestInitiator opposite(RequestInitiator value) {
    return value == RequestInitiator::Peer ? RequestInitiator::Local
                                           : RequestInitiator::Peer;
}

}  // namespace

class PublisherSession::Impl {
public:
    explicit Impl(PublisherSessionConfig value) : config(std::move(value)) {
        if (!valid_config(config)) {
            throw std::invalid_argument("publisher session limits must be nonzero");
        }
    }

    struct StreamState {
        std::optional<PeerStreamKind> kind;
        std::vector<std::byte> buffered;
        bool early_counted{false};
        bool fin_received{false};
    };

    struct RequestRecord {
        struct PendingUpdate {
            std::uint64_t request_id{0};
            RequestInitiator initiator{RequestInitiator::Peer};
        };

        std::uint64_t request_id{0};
        transport::StreamId stream_id{0};
        RequestInitiator initiator{RequestInitiator::Peer};
        RequestKind kind{RequestKind::Subscribe};
        RequestPhase request_phase{RequestPhase::AwaitingInitialResponse};
        bool initial_response_observed{false};
        std::deque<PendingUpdate> outstanding_updates;
        bool peer_fin{false};
        bool local_fin{false};
        bool peer_goaway_observed{false};
        bool local_goaway_observed{false};
        bool terminal{false};
    };

    PublisherSessionConfig config;
    SessionPhase phase{SessionPhase::AwaitingTransport};
    bool transport_established{false};
    std::optional<transport::StreamId> local_control_stream;
    bool local_setup_observed{false};
    std::optional<transport::StreamId> peer_control_stream;
    bool peer_setup_observed{false};
    std::unordered_map<transport::StreamId, StreamState> streams;
    std::unordered_map<transport::StreamId, LocalStreamPurpose>
        local_stream_purposes;
    std::unordered_map<transport::StreamId, RequestRecord> requests;
    std::unordered_map<std::uint64_t, RequestInitiator> request_owners;
    std::optional<std::uint64_t> highest_peer_request_id;
    std::optional<std::uint64_t> highest_local_request_id;
    std::optional<std::uint64_t> peer_goaway_cutoff;
    bool peer_control_goaway_observed{false};
    bool local_control_goaway_observed{false};
    std::size_t active_requests{0};
    std::size_t partial_bytes{0};
    std::size_t early_streams{0};
    std::size_t early_bytes{0};
    std::deque<EvidenceEvent> evidence;
    std::deque<EvidenceEvent> reserved_request_terminal_evidence;
    std::size_t evidence_bytes{0};
    std::uint64_t next_sequence{0};
    bool reserved_terminal_evidence{false};

    bool terminal() const noexcept {
        return phase == SessionPhase::Closing || phase == SessionPhase::Closed;
    }

    std::size_t remaining_evidence_bytes() const noexcept {
        return config.maximum_evidence_bytes -
               std::min(evidence_bytes, config.maximum_evidence_bytes);
    }

    void add_reserved(EvidenceKind kind, EvidenceData data) {
        if (reserved_terminal_evidence) return;
        reserved_terminal_evidence = true;
        const auto available = remaining_evidence_bytes();
        bound_terminal_owned_bytes(data, available);
        evidence_bytes += evidence_owned_bytes(data);
        evidence.push_back(
            EvidenceEvent{next_sequence++, kind, std::move(data)});
    }

    void close(SessionTransition& transition, EvidenceKind kind,
               EvidenceData data, std::uint64_t code,
               std::vector<std::byte> reason) {
        if (terminal()) return;
        phase = SessionPhase::Closing;
        add_reserved(kind, std::move(data));
        transition.actions.emplace_back(
            CloseSessionAction{code, std::move(reason)});
    }

    void harness_limit(SessionTransition& transition, HarnessLimitKind limit,
                       std::size_t attempted, std::size_t maximum) {
        terminalize_all(transition);
        const auto reason = reason_bytes("local session evidence limit");
        close(transition, EvidenceKind::HarnessLimit,
              HarnessLimitEvidence{limit, attempted, maximum}, kInternalError,
              reason);
    }

    bool emit(SessionTransition& transition, EvidenceKind kind,
              EvidenceData data) {
        if (terminal()) return false;
        const auto bytes = evidence_owned_bytes(data);
        if (evidence.size() >= config.maximum_evidence_count) {
            harness_limit(transition, HarnessLimitKind::EvidenceCount,
                          evidence.size() + 1,
                          config.maximum_evidence_count);
            return false;
        }
        if (bytes > config.maximum_evidence_bytes -
                        std::min(evidence_bytes,
                                 config.maximum_evidence_bytes)) {
            harness_limit(transition, HarnessLimitKind::EvidenceBytes,
                          evidence_bytes + bytes,
                          config.maximum_evidence_bytes);
            return false;
        }
        evidence_bytes += bytes;
        evidence.push_back(
            EvidenceEvent{next_sequence++, kind, std::move(data)});
        return true;
    }

    bool evidence_capacity_available(std::size_t additional_count,
                                     std::size_t additional_bytes) const {
        return additional_count <=
                   config.maximum_evidence_count -
                       std::min(evidence.size(),
                                config.maximum_evidence_count) &&
               additional_bytes <=
                   config.maximum_evidence_bytes -
                       std::min(evidence_bytes,
                                config.maximum_evidence_bytes);
    }

    bool preflight_evidence(SessionTransition& transition,
                            std::size_t additional_count,
                            std::size_t additional_bytes) {
        if (additional_count > config.maximum_evidence_count -
                                   std::min(evidence.size(),
                                            config.maximum_evidence_count)) {
            harness_limit(transition, HarnessLimitKind::EvidenceCount,
                          evidence.size() + additional_count,
                          config.maximum_evidence_count);
            return false;
        }
        if (additional_bytes > config.maximum_evidence_bytes -
                                   std::min(evidence_bytes,
                                            config.maximum_evidence_bytes)) {
            harness_limit(transition, HarnessLimitKind::EvidenceBytes,
                          evidence_bytes + additional_bytes,
                          config.maximum_evidence_bytes);
            return false;
        }
        return true;
    }

    void protocol_close(SessionTransition& transition,
                        std::optional<transport::StreamId> stream_id,
                        std::uint64_t code, const char* reason) {
        terminalize_all(transition);
        if (terminal()) return;
        auto bytes = reason_bytes(reason);
        close(transition, EvidenceKind::ProtocolViolation,
              ProtocolViolationEvidence{stream_id, code, bytes}, code,
              std::move(bytes));
    }

    void update_active(SessionTransition& transition) {
        if (phase == SessionPhase::AwaitingSetup && transport_established &&
            local_setup_observed && peer_setup_observed) {
            phase = SessionPhase::Active;
            early_streams = 0;
            early_bytes = 0;
            for (auto& [stream_id, stream] : streams) {
                if (terminal()) return;
                if (!unidirectional(stream_id) && !stream.buffered.empty()) {
                    process_request(transition, stream_id, stream,
                                    stream.fin_received);
                }
            }
        }
    }

    bool count_new_stream(SessionTransition& transition,
                          transport::StreamId stream_id, StreamState& stream,
                          bool is_control) {
        if (streams.size() > config.maximum_active_streams) {
            harness_limit(transition, HarnessLimitKind::ActiveStreams,
                          streams.size(), config.maximum_active_streams);
            return false;
        }
        if (phase == SessionPhase::AwaitingSetup && !is_control &&
            !stream.early_counted) {
            stream.early_counted = true;
            ++early_streams;
            if (early_streams > config.maximum_early_streams) {
                harness_limit(transition, HarnessLimitKind::EarlyStreams,
                              early_streams,
                              config.maximum_early_streams);
                streams.erase(stream_id);
                return false;
            }
        }
        return true;
    }

    bool register_request(SessionTransition& transition,
                          transport::StreamId stream_id,
                          RequestInitiator initiator,
                          const wire::draft18::Message& message,
                          bool peer_fault) {
        const auto opening = opening_request(message);
        if (!opening) {
            if (peer_fault) {
                protocol_close(transition, stream_id, kProtocolViolation,
                               "request stream did not begin with request");
            } else {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{stream_id});
            }
            return false;
        }
        const auto [request_id, kind] = *opening;
        if (!validate_request_id(transition, stream_id, initiator, request_id,
                                 peer_fault)) {
            return false;
        }
        if (request_owners.size() >= config.maximum_request_history) {
            harness_limit(transition, HarnessLimitKind::RequestHistory,
                          request_owners.size() + 1,
                          config.maximum_request_history);
            return false;
        }
        if (active_requests >= config.maximum_active_requests) {
            harness_limit(transition, HarnessLimitKind::ActiveRequests,
                          active_requests + 1,
                          config.maximum_active_requests);
            return false;
        }
        if (initiator == RequestInitiator::Local && peer_goaway_cutoff &&
            request_id >= *peer_goaway_cutoff) {
            emit(transition, EvidenceKind::LocalObservationError,
                 LocalObservationErrorEvidence{stream_id});
            return false;
        }
        const auto expected = expected_request_id(initiator);
        const bool gap = request_id != expected;
        if (!preflight_evidence(transition, gap ? 2 : 1,
                                message_owned_bytes(message))) {
            return false;
        }
        if (gap) {
            emit(transition, EvidenceKind::RequestIdSequenceViolation,
                 RequestIdSequenceEvidence{initiator, expected, request_id});
        }
        claim_request_id(initiator, request_id);
        RequestRecord record;
        record.request_id = request_id;
        record.stream_id = stream_id;
        record.initiator = initiator;
        record.kind = kind;
        requests.emplace(stream_id, std::move(record));
        ++active_requests;
        return emit(transition, EvidenceKind::RequestObserved,
                    RequestObservedEvidence{initiator, request_id, kind,
                                            stream_id, message});
    }

    bool validate_request_id(SessionTransition& transition,
                             transport::StreamId stream_id,
                             RequestInitiator initiator,
                             std::uint64_t request_id, bool peer_fault) {
        const auto required_parity =
            initiator == RequestInitiator::Peer ? 0u : 1u;
        if ((request_id & 1u) != required_parity ||
            request_owners.contains(request_id)) {
            if (peer_fault) {
                protocol_close(transition, stream_id, 0x4,
                               "invalid request ID");
            } else {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{stream_id});
            }
            return false;
        }
        return true;
    }

    std::uint64_t expected_request_id(RequestInitiator initiator) const {
        const auto required_parity =
            initiator == RequestInitiator::Peer ? 0u : 1u;
        const auto& highest = initiator == RequestInitiator::Peer
                                  ? highest_peer_request_id
                                  : highest_local_request_id;
        return highest ? (*highest >
                                   std::numeric_limits<std::uint64_t>::max() - 2
                               ? *highest
                               : *highest + 2)
                       : required_parity;
    }

    void claim_request_id(RequestInitiator initiator,
                          std::uint64_t request_id) {
        auto& highest = initiator == RequestInitiator::Peer
                            ? highest_peer_request_id
                            : highest_local_request_id;
        if (!highest || request_id > *highest) highest = request_id;
        request_owners.emplace(request_id, initiator);
    }

    bool observe_update(SessionTransition& transition, RequestRecord& request,
                        RequestInitiator initiator,
                        const wire::draft18::RequestUpdateMessage& update,
                        bool peer_fault) {
        const bool cross_publish_update =
            request.kind == RequestKind::Publish &&
            initiator != request.initiator;
        if (!validate_request_id(transition, request.stream_id, initiator,
                                 update.request_id, peer_fault)) {
            return false;
        }
        if (request_owners.size() >= config.maximum_request_history) {
            harness_limit(transition, HarnessLimitKind::RequestHistory,
                          request_owners.size() + 1,
                          config.maximum_request_history);
            return false;
        }
        const auto expected = expected_request_id(initiator);
        const bool gap = update.request_id != expected;
        const bool initiator_allowed =
            initiator == request.initiator || cross_publish_update;
        const bool context_valid =
            !request.terminal &&
            request.request_phase != RequestPhase::UpdateFailed &&
            initiator_allowed &&
            (!cross_publish_update ||
             request.request_phase == RequestPhase::Active);
        const auto source_bytes = parameters_bytes(update.parameters);
        if (!context_valid) {
            if (!preflight_evidence(transition, gap ? 2 : 1,
                                    source_bytes)) {
                return false;
            }
            if (gap) {
                emit(transition, EvidenceKind::RequestIdSequenceViolation,
                     RequestIdSequenceEvidence{initiator, expected,
                                               update.request_id});
            }
            claim_request_id(initiator, update.request_id);
            const wire::draft18::Message message = update;
            return emit(transition, EvidenceKind::RequestStateViolation,
                        RequestStateViolationEvidence{
                            initiator, request.request_id,
                            request.stream_id, message});
        }
        if (request.outstanding_updates.size() >=
            config.maximum_outstanding_updates_per_request) {
            harness_limit(transition, HarnessLimitKind::OutstandingUpdates,
                          request.outstanding_updates.size() + 1,
                          config.maximum_outstanding_updates_per_request);
            return false;
        }
        if (!preflight_evidence(transition, gap ? 2 : 1,
                                source_bytes)) {
            return false;
        }
        if (gap) {
            emit(transition, EvidenceKind::RequestIdSequenceViolation,
                 RequestIdSequenceEvidence{initiator, expected,
                                           update.request_id});
        }
        claim_request_id(initiator, update.request_id);
        request.outstanding_updates.push_back(
            RequestRecord::PendingUpdate{update.request_id, initiator});
        return emit(transition, EvidenceKind::UpdateObserved,
                    UpdateObservedEvidence{initiator, update.request_id,
                                           request.request_id,
                                           request.stream_id, update});
    }

    void terminalize_request(SessionTransition& transition,
                             RequestRecord& request,
                             RequestTerminalCause cause,
                             std::optional<std::uint64_t> application_error =
                                 std::nullopt) {
        if (request.terminal) return;
        if (!preflight_evidence(transition, 1, 0)) return;
        request.terminal = true;
        request.request_phase = RequestPhase::Terminal;
        std::deque<RequestRecord::PendingUpdate>{}.swap(
            request.outstanding_updates);
        if (active_requests != 0) --active_requests;
        if (terminal()) return;
        emit(transition, EvidenceKind::RequestTerminal,
             RequestTerminalEvidence{request.request_id, request.stream_id,
                                     cause, application_error});
    }

    void observe_request_fin(SessionTransition& transition,
                             RequestRecord& request,
                             RequestInitiator sender) {
        auto& observed = sender == RequestInitiator::Peer
                             ? request.peer_fin
                             : request.local_fin;
        observed = true;
        if (request.peer_fin && request.local_fin) {
            terminalize_request(
                transition, request,
                sender == RequestInitiator::Peer
                    ? RequestTerminalCause::PeerFin
                    : RequestTerminalCause::LocalFin);
        }
    }

    void cleanup_terminal_request_stream(transport::StreamId stream_id) {
        const auto request = requests.find(stream_id);
        if (request == requests.end() || !request->second.terminal) return;
        const auto stream = streams.find(stream_id);
        if (stream == streams.end()) return;
        partial_bytes -= stream->second.buffered.size();
        streams.erase(stream);
    }

    void terminalize_request_for_session(RequestRecord& request) {
        if (request.terminal) return;
        request.terminal = true;
        request.request_phase = RequestPhase::Terminal;
        std::deque<RequestRecord::PendingUpdate>{}.swap(
            request.outstanding_updates);
        if (active_requests != 0) --active_requests;
        reserved_request_terminal_evidence.push_back(EvidenceEvent{
            next_sequence++, EvidenceKind::RequestTerminal,
            RequestTerminalEvidence{request.request_id, request.stream_id,
                                    RequestTerminalCause::SessionClosed,
                                    std::nullopt}});
    }

    void terminalize_all(SessionTransition& transition) {
        static_cast<void>(transition);
        for (auto& [stream_id, request] : requests) {
            static_cast<void>(stream_id);
            terminalize_request_for_session(request);
        }
    }

    bool observe_goaway(SessionTransition& transition,
                        transport::StreamId stream_id,
                        RequestInitiator sender, GoawayPlacement placement,
                        const wire::draft18::GoawayMessage& goaway,
                        bool peer_fault) {
        bool* observed = nullptr;
        RequestRecord* request = nullptr;
        if (placement == GoawayPlacement::Control) {
            observed = sender == RequestInitiator::Peer
                           ? &peer_control_goaway_observed
                           : &local_control_goaway_observed;
        } else {
            const auto position = requests.find(stream_id);
            if (position == requests.end()) {
                if (peer_fault) {
                    protocol_close(transition, stream_id, kProtocolViolation,
                                   "GOAWAY on unknown request stream");
                } else {
                    emit(transition, EvidenceKind::LocalObservationError,
                         LocalObservationErrorEvidence{stream_id});
                }
                return false;
            }
            request = &position->second;
            observed = sender == RequestInitiator::Peer
                           ? &request->peer_goaway_observed
                           : &request->local_goaway_observed;
        }
        const bool context_valid =
            placement == GoawayPlacement::Control
                ? goaway.request_id.has_value()
                : !goaway.request_id.has_value();
        const bool peer_uri_valid =
            !peer_fault || goaway.new_session_uri.empty();
        const bool cutoff_valid =
            placement != GoawayPlacement::Control ||
            ((goaway.request_id.value_or(0) & 1u) ==
             (sender == RequestInitiator::Peer ? 1u : 0u));
        if (*observed || !context_valid || !peer_uri_valid || !cutoff_valid) {
            if (peer_fault) {
                protocol_close(transition, stream_id,
                               !cutoff_valid ? 0x4 : kProtocolViolation,
                               "invalid GOAWAY");
            } else {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{stream_id});
            }
            return false;
        }
        if (!preflight_evidence(transition, 1,
                                goaway.new_session_uri.size())) {
            return false;
        }
        *observed = true;
        if (sender == RequestInitiator::Peer &&
            placement == GoawayPlacement::Control) {
            peer_goaway_cutoff = goaway.request_id;
        }
        return emit(transition, EvidenceKind::GoawayObserved,
                    GoawayEvidence{sender, placement, stream_id,
                                   goaway.request_id, goaway});
    }

    bool response_violation(SessionTransition& transition,
                            RequestRecord& request,
                            RequestInitiator responder,
                            const wire::draft18::Message& message,
                            bool peer_fault) {
        static_cast<void>(peer_fault);
        if (!preflight_evidence(transition, 1,
                                message_owned_bytes(message))) {
            return false;
        }
        return emit(transition, EvidenceKind::ResponseViolation,
                    ResponseViolationEvidence{responder, request.request_id,
                                              request.stream_id, message});
    }

    bool observe_request_message(SessionTransition& transition,
                                 const RequestRecord& request,
                                 RequestInitiator sender,
                                 const wire::draft18::Message& message) {
        if (!preflight_evidence(transition, 1,
                                message_owned_bytes(message))) {
            return false;
        }
        return emit(transition, EvidenceKind::RequestMessageObserved,
                    RequestMessageObservedEvidence{
                        sender, request.request_id, request.stream_id,
                        message});
    }

    bool observe_response(SessionTransition& transition, RequestRecord& request,
                          RequestInitiator responder,
                          const wire::draft18::Message& message,
                          bool peer_fault) {
        if (!response_message(message)) {
            return response_violation(transition, request, responder, message,
                                      peer_fault);
        }
        const bool error =
            std::holds_alternative<wire::draft18::RequestErrorMessage>(message);
        if (!request.initial_response_observed) {
            if (responder != opposite(request.initiator) ||
                (!error && !valid_success_response(request.kind, message))) {
                const bool namespace_first_response =
                    request.kind == RequestKind::SubscribeNamespace ||
                    request.kind == RequestKind::SubscribeTracks;
                if (peer_fault && namespace_first_response) {
                    if (evidence_capacity_available(
                            1, message_owned_bytes(message))) {
                        emit(transition, EvidenceKind::ResponseViolation,
                             ResponseViolationEvidence{
                                 responder, request.request_id,
                                 request.stream_id, message});
                    }
                    protocol_close(transition, request.stream_id,
                                   kProtocolViolation,
                                   "invalid namespace first response");
                    return false;
                }
                return response_violation(transition, request, responder,
                                          message, peer_fault);
            }
            if (!preflight_evidence(transition, error ? 2 : 1,
                                    message_owned_bytes(message))) {
                return false;
            }
            request.initial_response_observed = true;
            request.request_phase = RequestPhase::Active;
            const auto emitted =
                emit(transition, EvidenceKind::InitialResponseObserved,
                     InitialResponseEvidence{responder, request.request_id,
                                             request.kind, request.stream_id,
                                             message});
            if (emitted && error) {
                terminalize_request(transition, request,
                                    RequestTerminalCause::ResponseError);
            }
            return emitted;
        }
        const auto pending = std::find_if(
            request.outstanding_updates.begin(),
            request.outstanding_updates.end(),
            [responder](const RequestRecord::PendingUpdate& value) {
                return responder == opposite(value.initiator);
            });
        if (pending == request.outstanding_updates.end() ||
            (!error && !std::holds_alternative<wire::draft18::RequestOkMessage>(
                           message))) {
            return response_violation(transition, request, responder, message,
                                      peer_fault);
        }
        const auto candidate_count = static_cast<std::size_t>(std::count_if(
            request.outstanding_updates.begin(),
            request.outstanding_updates.end(),
            [initiator = pending->initiator](const auto& outstanding) {
                return outstanding.initiator == initiator;
            }));
        const bool ambiguous = error && candidate_count > 1;
        const auto candidate_bytes =
            ambiguous ? candidate_count * sizeof(std::uint64_t) : 0;
        if (!preflight_evidence(transition, error ? 2 : 1,
                                message_owned_bytes(message) +
                                    candidate_bytes)) {
            return false;
        }
        std::vector<std::uint64_t> candidates;
        if (ambiguous) {
            candidates.reserve(candidate_count);
            for (const auto& outstanding : request.outstanding_updates) {
                if (outstanding.initiator == pending->initiator) {
                    candidates.push_back(outstanding.request_id);
                }
            }
        }
        UpdateResponseEvidence observed{
            responder,
            request.request_id,
            ambiguous ? std::nullopt
                      : std::optional<std::uint64_t>{pending->request_id},
            ambiguous ? std::move(candidates) : std::vector<std::uint64_t>{},
            request.stream_id,
            message};
        const auto update_initiator = pending->initiator;
        if (error) {
            std::erase_if(request.outstanding_updates,
                          [initiator = pending->initiator](const auto& value) {
                              return value.initiator == initiator;
                          });
        } else {
            request.outstanding_updates.erase(pending);
        }
        if (!emit(transition, EvidenceKind::UpdateResponseObserved,
                  std::move(observed))) {
            return false;
        }
        if (error) {
            request.request_phase = RequestPhase::UpdateFailed;
            return emit(
                transition, EvidenceKind::RequestUpdateFailed,
                RequestUpdateFailedEvidence{
                    request.request_id, request.stream_id, request.kind,
                    update_initiator});
        }
        return true;
    }

    void process_request(SessionTransition& transition,
                         transport::StreamId stream_id, StreamState& stream,
                         bool fin) {
        while (!stream.buffered.empty() && !terminal()) {
            wire::Cursor cursor(stream.buffered);
            auto decoded = wire::draft18::decode_message(
                wire::draft18::StreamRole::Request, cursor,
                config.wire_limits);
            if (std::holds_alternative<wire::NeedMore>(decoded)) {
                if (fin) {
                    protocol_close(transition, stream_id, kProtocolViolation,
                                   "truncated request message");
                }
                return;
            }
            if (const auto* error = std::get_if<wire::DecodeError>(&decoded)) {
                if (error->code == wire::DecodeErrorCode::LengthExceedsLimit) {
                    harness_limit(transition,
                                  HarnessLimitKind::PartialStreamBytes,
                                  stream.buffered.size(),
                                  config.maximum_partial_bytes_per_stream);
                    return;
                }
                const auto code =
                    error->code == wire::DecodeErrorCode::KeyValueFormattingError
                        ? kKeyValueFormattingError
                        : kProtocolViolation;
                protocol_close(transition, stream_id, code,
                               "invalid request message");
                return;
            }
            if (const auto* ambiguity =
                    std::get_if<wire::draft18::DraftAmbiguity>(&decoded)) {
                emit(transition, EvidenceKind::DraftAmbiguity,
                     DraftAmbiguityEvidence{stream_id, ambiguity->offset,
                                            ambiguity->detail});
                return;
            }
            auto message =
                std::get<wire::draft18::Message>(std::move(decoded));
            if (!requests.contains(stream_id)) {
                if (!register_request(transition, stream_id,
                                      RequestInitiator::Peer, message, true)) {
                    return;
                }
            } else {
                auto& request = requests.at(stream_id);
                if (const auto* update =
                        std::get_if<wire::draft18::RequestUpdateMessage>(
                            &message)) {
                    if (!observe_update(transition, request,
                                        RequestInitiator::Peer, *update,
                                        true)) {
                        return;
                    }
                } else if (response_message(message)) {
                    if (!observe_response(transition, request,
                                          RequestInitiator::Peer, message,
                                          true)) {
                        return;
                    }
                } else if (const auto* goaway =
                               std::get_if<wire::draft18::GoawayMessage>(
                                   &message)) {
                    if (!observe_goaway(transition, stream_id,
                                        RequestInitiator::Peer,
                                        GoawayPlacement::Request, *goaway,
                                        true)) {
                        return;
                    }
                } else if (std::holds_alternative<
                               wire::draft18::PublishDoneMessage>(message)) {
                    if (!observe_request_message(
                            transition, request, RequestInitiator::Peer,
                            message)) {
                        return;
                    }
                } else {
                    protocol_close(transition, stream_id, kProtocolViolation,
                                   "unexpected request stream message");
                    return;
                }
            }
            const auto consumed = cursor.offset();
            partial_bytes -= consumed;
            stream.buffered.erase(
                stream.buffered.begin(),
                stream.buffered.begin() +
                    static_cast<std::ptrdiff_t>(consumed));
        }
        if (fin && !terminal()) {
            auto position = requests.find(stream_id);
            if (position != requests.end()) {
                observe_request_fin(transition, position->second,
                                    RequestInitiator::Peer);
            }
        }
    }

    bool account_early_buffer(SessionTransition& transition,
                              StreamState& stream) {
        if (phase != SessionPhase::AwaitingSetup ||
            stream.kind == PeerStreamKind::Control) {
            return true;
        }
        if (stream.buffered.size() >
            config.maximum_early_bytes -
                std::min(early_bytes, config.maximum_early_bytes)) {
            harness_limit(transition, HarnessLimitKind::EarlyBytes,
                          early_bytes + stream.buffered.size(),
                          config.maximum_early_bytes);
            return false;
        }
        early_bytes += stream.buffered.size();
        return true;
    }

    bool append(SessionTransition& transition, StreamState& stream,
                std::span<const std::byte> bytes,
                bool is_control) {
        if (bytes.size() > config.maximum_partial_bytes_per_stream -
                               std::min(stream.buffered.size(),
                                        config.maximum_partial_bytes_per_stream)) {
            harness_limit(transition, HarnessLimitKind::PartialStreamBytes,
                          stream.buffered.size() + bytes.size(),
                          config.maximum_partial_bytes_per_stream);
            return false;
        }
        if (bytes.size() > config.maximum_partial_bytes_per_session -
                               std::min(partial_bytes,
                                        config.maximum_partial_bytes_per_session)) {
            harness_limit(transition, HarnessLimitKind::PartialSessionBytes,
                          partial_bytes + bytes.size(),
                          config.maximum_partial_bytes_per_session);
            return false;
        }
        if (phase == SessionPhase::AwaitingSetup && !is_control) {
            if (bytes.size() > config.maximum_early_bytes -
                                   std::min(early_bytes,
                                            config.maximum_early_bytes)) {
                harness_limit(transition, HarnessLimitKind::EarlyBytes,
                              early_bytes + bytes.size(),
                              config.maximum_early_bytes);
                return false;
            }
            early_bytes += bytes.size();
        }
        stream.buffered.insert(stream.buffered.end(), bytes.begin(), bytes.end());
        partial_bytes += bytes.size();
        return true;
    }

    void discard_buffer(StreamState& stream) {
        partial_bytes -= stream.buffered.size();
        stream.buffered.clear();
    }

    void inspect_setup_duplicates(SessionTransition& transition,
                                  transport::StreamId stream_id,
                                  const wire::draft18::SetupMessage& setup) {
        std::unordered_set<std::uint64_t> seen;
        for (const auto& option : setup.options) {
            if (!seen.insert(option.type).second &&
                detail::is_known_nonrepeatable_setup_option(option.type)) {
                if (!emit(transition, EvidenceKind::SetupOptionDuplicate,
                          SetupOptionDuplicateEvidence{stream_id,
                                                       option.type})) {
                    return;
                }
            }
        }
    }

    void process_control(SessionTransition& transition,
                         transport::StreamId stream_id, StreamState& stream) {
        while (!stream.buffered.empty() && !terminal()) {
            wire::Cursor cursor(stream.buffered);
            auto decoded = wire::draft18::decode_message(
                wire::draft18::StreamRole::Control, cursor,
                config.wire_limits);
            if (std::holds_alternative<wire::NeedMore>(decoded)) return;
            if (const auto* error = std::get_if<wire::DecodeError>(&decoded)) {
                if (error->code == wire::DecodeErrorCode::LengthExceedsLimit) {
                    harness_limit(transition,
                                  HarnessLimitKind::PartialStreamBytes,
                                  stream.buffered.size(),
                                  config.maximum_partial_bytes_per_stream);
                    return;
                }
                const auto code =
                    error->code == wire::DecodeErrorCode::KeyValueFormattingError
                        ? kKeyValueFormattingError
                        : kProtocolViolation;
                protocol_close(transition, stream_id, code,
                               "invalid control message");
                return;
            }
            if (const auto* ambiguity =
                    std::get_if<wire::draft18::DraftAmbiguity>(&decoded)) {
                emit(transition, EvidenceKind::DraftAmbiguity,
                     DraftAmbiguityEvidence{stream_id, ambiguity->offset,
                                            ambiguity->detail});
                return;
            }

            const auto consumed = cursor.offset();
            auto message = std::get<wire::draft18::Message>(std::move(decoded));
            if (const auto* setup =
                    std::get_if<wire::draft18::SetupMessage>(&message)) {
                if (peer_setup_observed) {
                    protocol_close(transition, stream_id, kProtocolViolation,
                                   "duplicate peer SETUP");
                    return;
                }
                peer_setup_observed = true;
                emit(transition, EvidenceKind::PeerSetupReceived,
                     SetupEvidence{stream_id, *setup});
                if (terminal()) return;
                inspect_setup_duplicates(transition, stream_id, *setup);
                update_active(transition);
            } else if (!peer_setup_observed) {
                protocol_close(transition, stream_id, kProtocolViolation,
                               "control stream did not begin with SETUP");
                return;
            } else if (const auto* goaway =
                           std::get_if<wire::draft18::GoawayMessage>(
                               &message)) {
                if (!observe_goaway(transition, stream_id,
                                    RequestInitiator::Peer,
                                    GoawayPlacement::Control, *goaway,
                                    true)) {
                    return;
                }
            } else {
                std::vector<std::byte> frame(stream.buffered.begin(),
                                             stream.buffered.begin() +
                                                 static_cast<std::ptrdiff_t>(
                                                     consumed));
                emit(transition, EvidenceKind::DeferredStreamBytes,
                     DeferredBytesEvidence{stream_id, PeerStreamKind::Control,
                                           std::move(frame), false});
            }
            if (terminal()) return;
            partial_bytes -= consumed;
            stream.buffered.erase(
                stream.buffered.begin(),
                stream.buffered.begin() +
                    static_cast<std::ptrdiff_t>(consumed));
        }
    }

    void process_stream_data(SessionTransition& transition,
                             const transport::StreamDataEvent& event) {
        if (!transport_established) {
            emit(transition, EvidenceKind::LocalObservationError,
                 LocalObservationErrorEvidence{event.stream_id});
            return;
        }
        const auto local_purpose =
            local_stream_purposes.find(event.stream_id);
        const bool local_request_stream =
            local_purpose != local_stream_purposes.end() &&
            local_purpose->second == LocalStreamPurpose::Request &&
            requests.contains(event.stream_id);
        if (!peer_initiated(event.stream_id) && !local_request_stream) {
            emit(transition, EvidenceKind::LocalObservationError,
                 LocalObservationErrorEvidence{event.stream_id});
            return;
        }

        const bool created = streams.find(event.stream_id) == streams.end();
        if (created && streams.size() >= config.maximum_active_streams) {
            harness_limit(transition, HarnessLimitKind::ActiveStreams,
                          streams.size() + 1,
                          config.maximum_active_streams);
            return;
        }
        auto [position, unused_inserted] = streams.try_emplace(event.stream_id);
        static_cast<void>(unused_inserted);
        auto& stream = position->second;
        if (!unidirectional(event.stream_id)) {
            if (created && peer_initiated(event.stream_id) &&
                !count_new_stream(transition, event.stream_id, stream,
                                  false)) {
                return;
            }
            if (!stream.kind) {
                stream.kind = PeerStreamKind::Request;
                if (peer_initiated(event.stream_id)) {
                    emit(transition, EvidenceKind::PeerStreamClassified,
                         StreamEvidence{event.stream_id,
                                        PeerStreamKind::Request});
                }
            }
            if (!append(transition, stream, event.data, false)) {
                return;
            }
            stream.fin_received = stream.fin_received || event.fin;
            wire::Cursor type_cursor(stream.buffered);
            const auto first_type = wire::read_vi64(type_cursor);
            if (std::holds_alternative<std::uint64_t>(first_type) &&
                std::get<std::uint64_t>(first_type) == 0x2f00) {
                protocol_close(transition, event.stream_id,
                               kProtocolViolation,
                               "SETUP on bidirectional stream");
                return;
            }
            if (phase == SessionPhase::Active) {
                process_request(transition, event.stream_id, stream,
                                event.fin);
                if (!terminal()) {
                    cleanup_terminal_request_stream(event.stream_id);
                }
                return;
            }
            return;
        }

        if (!append(transition, stream, event.data,
                    !stream.kind || stream.kind == PeerStreamKind::Control)) {
            return;
        }
        if (!stream.kind) {
            const auto classified =
                detail::classify_unidirectional_stream(stream.buffered);
            if (classified.need_more) {
                if (event.fin) {
                    protocol_close(transition, event.stream_id,
                                   kProtocolViolation,
                                   "truncated unidirectional stream type");
                }
                return;
            }
            if (!classified.kind) {
                protocol_close(transition, event.stream_id,
                               kProtocolViolation,
                               "unknown unidirectional stream type");
                return;
            }
            stream.kind = classified.kind;
            if (*stream.kind != PeerStreamKind::Control &&
                (!count_new_stream(transition, event.stream_id, stream,
                                   false) ||
                 !account_early_buffer(transition, stream))) {
                return;
            }
            if (*stream.kind == PeerStreamKind::Control) {
                if (peer_control_stream &&
                    *peer_control_stream != event.stream_id) {
                    protocol_close(transition, event.stream_id,
                                   kProtocolViolation,
                                   "second peer control stream");
                    return;
                }
                peer_control_stream = event.stream_id;
            }
            emit(transition, EvidenceKind::PeerStreamClassified,
                 StreamEvidence{event.stream_id, *stream.kind});
            if (terminal()) return;
        }

        if (*stream.kind == PeerStreamKind::Control) {
            process_control(transition, event.stream_id, stream);
            if (event.fin && !terminal()) {
                protocol_close(transition, event.stream_id,
                               kProtocolViolation,
                               "peer control stream closed");
            }
            return;
        }

        const auto buffered_size = stream.buffered.size();
        emit(transition, EvidenceKind::DeferredStreamBytes,
             DeferredBytesEvidence{event.stream_id, *stream.kind,
                                   std::move(stream.buffered), event.fin});
        partial_bytes -= buffered_size;
        stream.buffered.clear();
        if (event.fin) streams.erase(position);
    }

    SessionTransition event(const transport::TransportEvent& input) {
        SessionTransition transition;
        if (terminal()) return transition;
        std::visit(
            Overloaded{
                [&](const transport::ConnectionEstablishedEvent& value) {
                    if (transport_established) {
                        emit(transition, EvidenceKind::LocalObservationError,
                             LocalObservationErrorEvidence{std::nullopt});
                        return;
                    }
                    transport_established = true;
                    phase = SessionPhase::AwaitingSetup;
                    emit(transition, EvidenceKind::TransportEstablished,
                         TransportEstablishedEvidence{
                             value.alpn, value.local_connection_id,
                             value.peer_connection_id,
                             value.max_datagram_payload});
                    update_active(transition);
                },
                [&](const transport::StreamDataEvent& value) {
                    process_stream_data(transition, value);
                },
                [&](const transport::PeerResetEvent& value) {
                    emit(transition, EvidenceKind::PeerReset,
                         StreamErrorEvidence{value.stream_id,
                                             value.application_error});
                    if (peer_control_stream == value.stream_id && !terminal()) {
                        protocol_close(transition, value.stream_id,
                                       kProtocolViolation,
                                       "peer reset control stream");
                    } else if (!terminal()) {
                        const auto request = requests.find(value.stream_id);
                        if (request != requests.end()) {
                            terminalize_request(
                                transition, request->second,
                                RequestTerminalCause::PeerReset,
                                value.application_error);
                        }
                        const auto stream = streams.find(value.stream_id);
                        if (stream != streams.end()) {
                            partial_bytes -= stream->second.buffered.size();
                            streams.erase(stream);
                        }
                    }
                },
                [&](const transport::PeerStopSendingEvent& value) {
                    emit(transition, EvidenceKind::PeerStopSending,
                         StreamErrorEvidence{value.stream_id,
                                             value.application_error});
                    const auto request = requests.find(value.stream_id);
                    if (request != requests.end()) {
                        terminalize_request(
                            transition, request->second,
                            RequestTerminalCause::PeerStopSending,
                            value.application_error);
                    }
                },
                [&](const transport::DatagramEvent& value) {
                    emit(transition, EvidenceKind::DeferredStreamBytes,
                         DeferredBytesEvidence{0, PeerStreamKind::Subgroup,
                                               value.data, false});
                },
                [&](const transport::PeerCloseEvent& value) {
                    terminalize_all(transition);
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::PeerClose,
                                 make_bounded_close_evidence(
                                     value.error_space, value.error_code,
                                     value.reason,
                                     remaining_evidence_bytes()));
                },
                [&](const transport::LocalCloseEvent& value) {
                    terminalize_all(transition);
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::LocalClose,
                                 make_bounded_close_evidence(
                                     value.error_space, value.error_code,
                                     value.reason,
                                     remaining_evidence_bytes()));
                },
                [&](const transport::IdleTimeoutEvent&) {
                    terminalize_all(transition);
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::IdleTimeout, MarkerEvidence{});
                },
                [&](const transport::TransportErrorEvent& value) {
                    terminalize_all(transition);
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::TransportError,
                                 TransportErrorEvidence{value.error});
                },
                [&](const transport::EventQueueOverflowEvent&) {
                    terminalize_all(transition);
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::TransportEventOverflow,
                                 MarkerEvidence{});
                }},
            input);
        return transition;
    }
};

PublisherSession::PublisherSession(PublisherSessionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
PublisherSession::~PublisherSession() = default;
PublisherSession::PublisherSession(PublisherSession&&) noexcept = default;
PublisherSession& PublisherSession::operator=(PublisherSession&&) noexcept =
    default;

SessionTransition PublisherSession::on_event(
    const transport::TransportEvent& event) {
    if (!impl_) return {};
    return impl_->event(event);
}

SessionTransition PublisherSession::observe_local_stream(
    transport::StreamId stream_id, LocalStreamPurpose purpose) {
    SessionTransition transition;
    if (!impl_ || impl_->terminal()) return transition;
    const bool local = !peer_initiated(stream_id);
    const bool correct_direction =
        local && ((purpose == LocalStreamPurpose::Request &&
                   !unidirectional(stream_id)) ||
                  (purpose != LocalStreamPurpose::Request &&
                   unidirectional(stream_id)));
    if (!impl_->transport_established || !correct_direction ||
        (purpose == LocalStreamPurpose::Control &&
         impl_->local_control_stream.has_value()) ||
        impl_->local_stream_purposes.contains(stream_id)) {
        impl_->emit(transition, EvidenceKind::LocalObservationError,
                    LocalObservationErrorEvidence{stream_id});
        return transition;
    }
    if (impl_->local_stream_purposes.size() >=
        impl_->config.maximum_local_stream_history) {
        impl_->harness_limit(
            transition, HarnessLimitKind::LocalStreamHistory,
            impl_->local_stream_purposes.size() + 1,
            impl_->config.maximum_local_stream_history);
        return transition;
    }
    if (purpose == LocalStreamPurpose::Control) {
        impl_->local_control_stream = stream_id;
    }
    impl_->local_stream_purposes.emplace(stream_id, purpose);
    impl_->emit(transition, EvidenceKind::LocalStreamObserved,
                LocalStreamEvidence{stream_id, purpose});
    return transition;
}

SessionTransition PublisherSession::observe_local_message(
    transport::StreamId stream_id,
    const wire::draft18::Message& message, bool fin) {
    SessionTransition transition;
    if (!impl_ || impl_->terminal()) return transition;
    const auto* setup = std::get_if<wire::draft18::SetupMessage>(&message);
    if (setup && impl_->local_control_stream == stream_id &&
        !impl_->local_setup_observed && !fin) {
        impl_->local_setup_observed = true;
        impl_->emit(transition, EvidenceKind::LocalSetupObserved,
                    SetupEvidence{stream_id, *setup});
        impl_->update_active(transition);
        return transition;
    }
    if (const auto* goaway =
            std::get_if<wire::draft18::GoawayMessage>(&message)) {
        if (impl_->local_control_stream == stream_id) {
            impl_->observe_goaway(transition, stream_id,
                                  RequestInitiator::Local,
                                  GoawayPlacement::Control, *goaway, false);
            return transition;
        }
        if (impl_->requests.contains(stream_id)) {
            impl_->observe_goaway(transition, stream_id,
                                  RequestInitiator::Local,
                                  GoawayPlacement::Request, *goaway, false);
            return transition;
        }
    }
    const auto purpose = impl_->local_stream_purposes.find(stream_id);
    const auto existing_request = impl_->requests.find(stream_id);
    if (existing_request != impl_->requests.end()) {
        if (const auto* update =
                std::get_if<wire::draft18::RequestUpdateMessage>(&message)) {
            impl_->observe_update(transition, existing_request->second,
                                  RequestInitiator::Local, *update, false);
            if (fin && !impl_->terminal()) {
                impl_->observe_request_fin(transition,
                                           existing_request->second,
                                           RequestInitiator::Local);
                impl_->cleanup_terminal_request_stream(stream_id);
            }
            return transition;
        }
        if (response_message(message)) {
            impl_->observe_response(transition, existing_request->second,
                                    RequestInitiator::Local, message, false);
            if (fin && !impl_->terminal()) {
                impl_->observe_request_fin(transition,
                                           existing_request->second,
                                           RequestInitiator::Local);
                impl_->cleanup_terminal_request_stream(stream_id);
            }
            return transition;
        }
        if (std::holds_alternative<wire::draft18::PublishDoneMessage>(
                message)) {
            impl_->observe_request_message(
                transition, existing_request->second,
                RequestInitiator::Local, message);
            if (fin && !impl_->terminal()) {
                impl_->observe_request_fin(transition,
                                           existing_request->second,
                                           RequestInitiator::Local);
                impl_->cleanup_terminal_request_stream(stream_id);
            }
            return transition;
        }
    }
    if (purpose != impl_->local_stream_purposes.end() &&
        purpose->second == LocalStreamPurpose::Request) {
        if (existing_request == impl_->requests.end()) {
            if (impl_->register_request(transition, stream_id,
                                        RequestInitiator::Local, message,
                                        false) &&
                fin) {
                auto& request = impl_->requests.at(stream_id);
                impl_->observe_request_fin(transition, request,
                                           RequestInitiator::Local);
                impl_->cleanup_terminal_request_stream(stream_id);
            }
            return transition;
        }
    }
    impl_->emit(transition, EvidenceKind::LocalObservationError,
                LocalObservationErrorEvidence{stream_id});
    return transition;
}

SessionTransition PublisherSession::observe_local_stop_sending(
    transport::StreamId stream_id, std::uint64_t application_error) {
    SessionTransition transition;
    if (!impl_ || impl_->terminal()) return transition;
    const auto request = impl_->requests.find(stream_id);
    if (request == impl_->requests.end()) {
        impl_->emit(transition, EvidenceKind::LocalObservationError,
                    LocalObservationErrorEvidence{stream_id});
        return transition;
    }
    impl_->terminalize_request(transition, request->second,
                               RequestTerminalCause::LocalStopSending,
                               application_error);
    return transition;
}

SessionTransition PublisherSession::observe_local_fin(
    transport::StreamId stream_id) {
    SessionTransition transition;
    if (!impl_ || impl_->terminal()) return transition;
    const auto request = impl_->requests.find(stream_id);
    if (request == impl_->requests.end()) {
        impl_->emit(transition, EvidenceKind::LocalObservationError,
                    LocalObservationErrorEvidence{stream_id});
        return transition;
    }
    impl_->observe_request_fin(transition, request->second,
                               RequestInitiator::Local);
    impl_->cleanup_terminal_request_stream(stream_id);
    return transition;
}

std::vector<EvidenceEvent> PublisherSession::take_evidence(
    std::size_t maximum) {
    std::vector<EvidenceEvent> output;
    if (!impl_ || maximum == 0) return output;
    const auto available = impl_->evidence.size() +
                           impl_->reserved_request_terminal_evidence.size();
    const auto count = std::min(maximum, available);
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const bool take_reserved =
            impl_->evidence.empty() ||
            (!impl_->reserved_request_terminal_evidence.empty() &&
             impl_->reserved_request_terminal_evidence.front().sequence <
                 impl_->evidence.front().sequence);
        if (take_reserved) {
            output.push_back(
                std::move(impl_->reserved_request_terminal_evidence.front()));
            impl_->reserved_request_terminal_evidence.pop_front();
        } else {
            impl_->evidence_bytes -=
                evidence_owned_bytes(impl_->evidence.front().data);
            output.push_back(std::move(impl_->evidence.front()));
            impl_->evidence.pop_front();
        }
    }
    return output;
}

SessionPhase PublisherSession::phase() const noexcept {
    return impl_ ? impl_->phase : SessionPhase::Closed;
}

}  // namespace moq::interop::session
