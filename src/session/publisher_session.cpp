#include "moq/interop/session/publisher_session.h"

#include "session/draft18_dispatch_internal.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
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

std::size_t track_key_bytes(const TrackKey& value) {
    std::size_t result = value.track_name.size();
    for (const auto& field : value.name_space.fields) result += field.size();
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
            [](const SubscriptionCreatedEvidence& value) {
                return track_key_bytes(value.track);
            },
            [](const SubscriptionPhaseEvidence& value) {
                return track_key_bytes(value.track);
            },
            [](const DuplicateSubscriptionEvidence& value) {
                return track_key_bytes(value.track);
            },
            [](const OppositeRoleCoexistenceEvidence& value) {
                return track_key_bytes(value.track);
            },
            [](const PendingSubscriptionReplacementEvidence& value) {
                return track_key_bytes(value.track);
            },
            [](const ReservedNamespaceEvidence& value) {
                return track_key_bytes(value.track);
            },
            [](const ObjectObservedEvidence& value) {
                std::size_t result = value.object.retained_payload.size();
                for (const auto& property : value.object.properties) {
                    result += key_value_bytes(property);
                }
                return result;
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
           config.maximum_local_stream_history != 0 &&
           config.maximum_active_subscriptions != 0 &&
           config.maximum_subscription_history != 0 &&
           config.maximum_subscription_key_bytes != 0 &&
           config.maximum_auth_token_cache_bytes != 0;
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

struct SubscriptionIdentity {
    TrackKey track;
    LocalSubscriptionRole local_role{LocalSubscriptionRole::Publisher};
};

struct SubscriptionIdentityLess {
    bool operator()(const SubscriptionIdentity& lhs,
                    const SubscriptionIdentity& rhs) const {
        if (lhs.track.name_space.fields != rhs.track.name_space.fields) {
            return lhs.track.name_space.fields < rhs.track.name_space.fields;
        }
        if (lhs.track.track_name != rhs.track.track_name) {
            return lhs.track.track_name < rhs.track.track_name;
        }
        return lhs.local_role < rhs.local_role;
    }
};

std::optional<std::pair<TrackKey, LocalSubscriptionRole>>
subscription_identity(const wire::draft18::Message& message,
                      RequestInitiator initiator) {
    if (const auto* subscribe =
            std::get_if<wire::draft18::SubscribeMessage>(&message)) {
        return {{TrackKey{{subscribe->track_namespace.fields},
                           subscribe->track_name.bytes},
                 initiator == RequestInitiator::Peer
                     ? LocalSubscriptionRole::Publisher
                     : LocalSubscriptionRole::Subscriber}};
    }
    if (const auto* publish =
            std::get_if<wire::draft18::PublishMessage>(&message)) {
        return {{TrackKey{{publish->track_namespace.fields},
                           publish->track_name.bytes},
                 initiator == RequestInitiator::Peer
                     ? LocalSubscriptionRole::Subscriber
                     : LocalSubscriptionRole::Publisher}};
    }
    return std::nullopt;
}

std::optional<ReservedNamespaceCategory> reserved_namespace(
    const TrackKey& track) {
    if (track.name_space.fields.empty()) return std::nullopt;
    const auto& first = track.name_space.fields.front();
    if (first == std::vector<std::byte>{std::byte{'.'}}) {
        return ReservedNamespaceCategory::Dot;
    }
    constexpr char kSession[] = ".session";
    if (first.size() == sizeof(kSession) - 1 &&
        std::equal(first.begin(), first.end(), kSession,
                   [](std::byte actual, char expected) {
                       return actual == static_cast<std::byte>(expected);
                   })) {
        return track.track_name.empty()
                   ? ReservedNamespaceCategory::SessionEmptyTrack
                   : ReservedNamespaceCategory::SessionUnrecognized;
    }
    return std::nullopt;
}

const wire::draft18::Parameters* message_parameters(
    const wire::draft18::Message& message) {
    return std::visit(
        [](const auto& value) -> const wire::draft18::Parameters* {
            if constexpr (requires { value.parameters; }) {
                return &value.parameters;
            }
            return nullptr;
        },
        message);
}

std::optional<bool> forward_parameter(
    const wire::draft18::Parameters& parameters) {
    for (const auto& parameter : parameters) {
        if (parameter.type != 0x10) continue;
        if (const auto* value =
                std::get_if<wire::draft18::Uint8ParameterValue>(
                    &parameter.value)) {
            return value->value != 0;
        }
    }
    return std::nullopt;
}

std::optional<wire::draft18::Location> largest_object_parameter(
    const wire::draft18::Parameters& parameters) {
    for (const auto& parameter : parameters) {
        if (parameter.type != 0x09) continue;
        if (const auto* location =
                std::get_if<wire::draft18::Location>(&parameter.value)) {
            return *location;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> setup_auth_cache_size(
    const wire::draft18::SetupMessage& setup) {
    for (const auto& option : setup.options) {
        if (option.type != 0x04) continue;
        const auto* value =
            std::get_if<wire::draft18::VarIntValue>(&option.value);
        if (!value || value->value > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(value->value);
    }
    return std::size_t{0};
}

std::optional<std::vector<wire::draft18::Token>> setup_tokens(
    const wire::draft18::SetupMessage& setup) {
    std::vector<wire::draft18::Token> tokens;
    for (const auto& option : setup.options) {
        if (option.type != 0x03) continue;
        const auto* bytes =
            std::get_if<wire::draft18::ByteValue>(&option.value);
        if (!bytes) return std::nullopt;
        wire::Cursor cursor(bytes->bytes);
        const auto decoded = wire::draft18::decode_token(
            cursor, bytes->bytes.size());
        const auto* token = std::get_if<wire::draft18::Token>(&decoded);
        if (!token) return std::nullopt;
        tokens.push_back(*token);
    }
    return tokens;
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
        std::optional<wire::draft18::SubgroupDecoder> subgroup_decoder;
        bool data_deferred_before_active{false};
        bool early_counted{false};
        bool fin_received{false};
    };

    struct RequestRecord {
        struct PendingUpdate {
            std::uint64_t request_id{0};
            RequestInitiator initiator{RequestInitiator::Peer};
            bool enables_forward{false};
        };

        struct SubscriptionState {
            TrackKey track;
            LocalSubscriptionRole local_role{
                LocalSubscriptionRole::Publisher};
            SubscriptionPhase phase{SubscriptionPhase::Pending};
            std::size_t retained_key_bytes{0};
            std::optional<std::uint64_t> track_alias;
            bool forward_state{true};
            std::optional<wire::draft18::Location> largest_object;
            std::optional<wire::draft18::Location> joining_location;
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
        bool token_rejected{false};
        std::optional<SubscriptionState> subscription;
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
    std::map<SubscriptionIdentity, transport::StreamId,
             SubscriptionIdentityLess>
        active_subscriptions;
    std::map<std::pair<RequestInitiator, std::uint64_t>, transport::StreamId>
        established_aliases;
    struct CachedToken {
        std::uint64_t type{0};
        std::vector<std::byte> value;
    };
    std::map<std::pair<RequestInitiator, std::uint64_t>, CachedToken>
        token_cache;
    std::size_t peer_token_cache_bytes{0};
    std::size_t local_token_cache_bytes{0};
    std::size_t peer_token_cache_limit{0};
    std::size_t local_token_cache_limit{0};
    std::vector<wire::draft18::Token> pending_local_setup_tokens;
    std::optional<std::uint64_t> highest_peer_request_id;
    std::optional<std::uint64_t> highest_local_request_id;
    std::optional<std::uint64_t> peer_goaway_cutoff;
    bool peer_control_goaway_observed{false};
    bool local_control_goaway_observed{false};
    std::size_t active_requests{0};
    std::size_t subscription_history{0};
    std::size_t subscription_key_bytes{0};
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

    bool optional_subscription_evidence_available(
        std::size_t additional_count, std::size_t additional_bytes) const {
        const auto free_count = config.maximum_evidence_count -
            std::min(evidence.size(), config.maximum_evidence_count);
        // Keep space for active request terminals and a protocol outcome.
        return active_requests + 1 < free_count &&
            additional_count <= free_count - active_requests - 2 &&
            evidence_capacity_available(additional_count + 1,
                                        additional_bytes);
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
        if (!emit(transition, EvidenceKind::RequestObserved,
                  RequestObservedEvidence{initiator, request_id, kind,
                                          stream_id, message})) {
            return false;
        }
        if (const auto* parameters = message_parameters(message)) {
            if (!process_token_parameters(transition, initiator, stream_id,
                                          *parameters, true)) {
                if (terminal()) return false;
                requests.at(stream_id).token_rejected = true;
                return true;
            }
        }
        return register_subscription(transition, requests.at(stream_id),
                                     message);
    }

    bool process_token_parameters(SessionTransition& transition,
                                  RequestInitiator sender,
                                  transport::StreamId stream_id,
                                  const wire::draft18::Parameters& parameters,
                                  bool fin_on_reject) {
        auto& occupied = sender == RequestInitiator::Peer
                             ? peer_token_cache_bytes
                             : local_token_cache_bytes;
        const auto limit = sender == RequestInitiator::Peer
                               ? peer_token_cache_limit
                               : local_token_cache_limit;
        const auto reject = [&](std::uint64_t error_code) {
            if (sender == RequestInitiator::Peer) {
                transition.actions.emplace_back(SendMessageAction{
                    stream_id,
                    wire::draft18::RequestErrorMessage{
                        error_code, 0, {}, std::nullopt},
                    fin_on_reject});
            } else {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{stream_id});
            }
            return false;
        };
        const auto close_or_local_error = [&](std::uint64_t error_code,
                                              const char* reason) {
            if (sender == RequestInitiator::Peer) {
                protocol_close(transition, stream_id, error_code, reason);
            } else {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{stream_id});
            }
            return false;
        };
        struct ResolvedToken {
            std::uint64_t type;
            std::span<const std::byte> value;
        };
        struct ResolvedTokenLess {
            bool operator()(const ResolvedToken& lhs,
                            const ResolvedToken& rhs) const {
                if (lhs.type != rhs.type) return lhs.type < rhs.type;
                return std::lexicographical_compare(
                    lhs.value.begin(), lhs.value.end(),
                    rhs.value.begin(), rhs.value.end());
            }
        };
        std::set<ResolvedToken, ResolvedTokenLess> resolved;
        std::vector<decltype(token_cache)::node_type> retired_tokens;
        const auto append_resolved = [&](std::uint64_t type,
                                         std::span<const std::byte> value) {
            return resolved.insert(ResolvedToken{type, value}).second ||
                   reject(0x4);
        };
        for (const auto& parameter : parameters) {
            if (parameter.type != 0x03) continue;
            const auto* token =
                std::get_if<wire::draft18::Token>(&parameter.value);
            if (!token) return close_or_local_error(0x6, "invalid token");
            if (token->alias_type == wire::draft18::TokenAliasType::UseValue) {
                if (!append_resolved(token->token_type.value_or(0),
                                     token->token_value)) return false;
                continue;
            }
            if (!token->alias) {
                return close_or_local_error(0x6, "missing token alias");
            }
            const auto key = std::pair{sender, *token->alias};
            const auto existing = token_cache.find(key);
            if (token->alias_type == wire::draft18::TokenAliasType::Register) {
                if (existing != token_cache.end()) {
                    return close_or_local_error(
                        0x14, "duplicate authorization token alias");
                }
                const auto bytes = std::size_t{16} + token->token_value.size();
                if (bytes > limit - std::min(occupied, limit)) {
                    return close_or_local_error(
                        0x13, "authorization token cache overflow");
                }
                token_cache.emplace(key,
                    CachedToken{token->token_type.value_or(0),
                                token->token_value});
                occupied += bytes;
                if (!append_resolved(token->token_type.value_or(0),
                                     token->token_value)) return false;
            } else if (existing == token_cache.end()) {
                return close_or_local_error(
                    0x17, "unknown authorization token alias");
            } else if (token->alias_type ==
                       wire::draft18::TokenAliasType::Delete) {
                occupied -= 16 + existing->second.value.size();
                retired_tokens.push_back(token_cache.extract(existing));
            } else {
                if (!append_resolved(existing->second.type,
                                     existing->second.value)) return false;
            }
        }
        return true;
    }

    bool process_peer_setup_tokens(
        SessionTransition& transition, transport::StreamId stream_id,
        const wire::draft18::SetupMessage& setup) {
        const auto tokens = setup_tokens(setup);
        if (!tokens) {
            protocol_close(transition, stream_id,
                           kKeyValueFormattingError,
                           "invalid setup token structure");
            return false;
        }
        for (const auto& token : *tokens) {
            if (token.alias_type == wire::draft18::TokenAliasType::Delete ||
                token.alias_type == wire::draft18::TokenAliasType::UseAlias) {
                protocol_close(transition, stream_id, kProtocolViolation,
                               "setup cannot reference token alias");
                return false;
            }
            if (token.alias_type == wire::draft18::TokenAliasType::UseValue) {
                continue;
            }
            if (!token.alias) {
                protocol_close(transition, stream_id,
                               kKeyValueFormattingError,
                               "setup token alias is missing");
                return false;
            }
            const auto key = std::pair{RequestInitiator::Peer, *token.alias};
            if (token_cache.contains(key)) {
                protocol_close(transition, stream_id, 0x14,
                               "duplicate setup token alias");
                return false;
            }
            const auto size = std::size_t{16} + token.token_value.size();
            if (size > peer_token_cache_limit -
                           std::min(peer_token_cache_bytes,
                                    peer_token_cache_limit)) {
                // SETUP registration over the advertised limit is USE_VALUE.
                continue;
            }
            token_cache.emplace(key,
                CachedToken{token.token_type.value_or(0),
                            token.token_value});
            peer_token_cache_bytes += size;
        }
        return true;
    }

    bool apply_local_setup_tokens(SessionTransition& transition,
                                  transport::StreamId stream_id) {
        std::map<std::uint64_t, CachedToken> staged;
        std::size_t staged_bytes = 0;
        for (const auto& token : pending_local_setup_tokens) {
            if (token.alias_type == wire::draft18::TokenAliasType::UseValue) {
                continue;
            }
            if (token.alias_type != wire::draft18::TokenAliasType::Register ||
                !token.alias) {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{stream_id});
                pending_local_setup_tokens.clear();
                return false;
            }
            const auto key = std::pair{RequestInitiator::Local, *token.alias};
            if (token_cache.contains(key) || staged.contains(*token.alias)) {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{stream_id});
                pending_local_setup_tokens.clear();
                return false;
            }
            const auto size = std::size_t{16} + token.token_value.size();
            if (size > local_token_cache_limit -
                           std::min(local_token_cache_bytes + staged_bytes,
                                    local_token_cache_limit)) {
                continue;
            }
            staged.emplace(*token.alias,
                           CachedToken{token.token_type.value_or(0),
                                       token.token_value});
            staged_bytes += size;
        }
        for (auto& [alias, token] : staged) {
            token_cache.emplace(
                std::pair{RequestInitiator::Local, alias},
                std::move(token));
        }
        local_token_cache_bytes += staged_bytes;
        pending_local_setup_tokens.clear();
        return true;
    }

    bool register_subscription(SessionTransition& transition,
                               RequestRecord& request,
                               const wire::draft18::Message& message) {
        const auto identity = subscription_identity(message, request.initiator);
        if (!identity) return true;
        const auto& [track, local_role] = *identity;
        const auto* publish =
            std::get_if<wire::draft18::PublishMessage>(&message);
        const auto* parameters = message_parameters(message);
        const bool initial_forward =
            parameters ? forward_parameter(*parameters).value_or(true) : true;
        // The request record and the active-role index each own a TrackKey.
        const auto key_bytes = track_key_bytes(track);
        const auto retained_bytes =
            key_bytes > std::numeric_limits<std::size_t>::max() / 2
                ? std::numeric_limits<std::size_t>::max()
                : key_bytes * 2;
        const auto reject = [&](std::uint64_t error_code) {
            if (request.initiator == RequestInitiator::Peer) {
                transition.actions.emplace_back(SendMessageAction{
                    request.stream_id,
                    wire::draft18::RequestErrorMessage{
                        error_code, 0, {}, std::nullopt},
                    true});
            } else {
                emit(transition, EvidenceKind::LocalObservationError,
                     LocalObservationErrorEvidence{request.stream_id});
            }
            return true;
        };
        if (const auto category = reserved_namespace(track)) {
            if (optional_subscription_evidence_available(1, key_bytes)) {
                emit(transition, EvidenceKind::ReservedNamespaceRejected,
                     ReservedNamespaceEvidence{track, *category,
                                               request.initiator,
                                               request.request_id,
                                               request.stream_id});
            }
            return reject(0x10);
        }
        const SubscriptionIdentity active_key{track, local_role};
        if (const auto existing = active_subscriptions.find(active_key);
            existing != active_subscriptions.end()) {
            const auto prior = requests.find(existing->second);
            const bool replace_pending_subscribe =
                request.initiator == RequestInitiator::Peer &&
                request.kind == RequestKind::Publish &&
                prior != requests.end() &&
                prior->second.initiator == RequestInitiator::Local &&
                prior->second.kind == RequestKind::Subscribe &&
                prior->second.subscription &&
                prior->second.subscription->phase == SubscriptionPhase::Pending;
            if (replace_pending_subscribe) {
                const auto cancelled_id = prior->second.request_id;
                const auto cancelled_stream = prior->second.stream_id;
                terminalize_request(transition, prior->second,
                                    RequestTerminalCause::LocalStopSending, 0x1);
                if (terminal()) return false;
                transition.actions.emplace_back(
                    StopSendingAction{cancelled_stream, 0x1});
                if (optional_subscription_evidence_available(1, key_bytes)) {
                    emit(transition, EvidenceKind::PendingSubscriptionReplaced,
                         PendingSubscriptionReplacementEvidence{
                             track, cancelled_id, cancelled_stream,
                             request.request_id, request.stream_id});
                }
                return register_subscription(transition, request, message);
            }
            if (prior != requests.end() &&
                optional_subscription_evidence_available(1, key_bytes)) {
                emit(transition, EvidenceKind::DuplicateSubscription,
                     DuplicateSubscriptionEvidence{
                         track, local_role, prior->second.request_id,
                         request.request_id, request.stream_id});
            }
            return reject(0x19);
        }
        if (publish && !validate_track_alias(
                           transition, request.initiator,
                           publish->track_alias, track, request.stream_id)) {
            return false;
        }
        if (subscription_history >= config.maximum_subscription_history) {
            harness_limit(transition, HarnessLimitKind::SubscriptionHistory,
                          subscription_history + 1,
                          config.maximum_subscription_history);
            return false;
        }
        if (active_subscriptions.size() >=
            config.maximum_active_subscriptions) {
            harness_limit(transition, HarnessLimitKind::ActiveSubscriptions,
                          active_subscriptions.size() + 1,
                          config.maximum_active_subscriptions);
            return false;
        }
        if (key_bytes > config.maximum_subscription_key_bytes / 2 ||
            retained_bytes >
            config.maximum_subscription_key_bytes -
                std::min(subscription_key_bytes,
                         config.maximum_subscription_key_bytes)) {
            harness_limit(transition, HarnessLimitKind::SubscriptionKeyBytes,
                          subscription_key_bytes + retained_bytes,
                          config.maximum_subscription_key_bytes);
            return false;
        }
        const auto other_role =
            local_role == LocalSubscriptionRole::Publisher
                ? LocalSubscriptionRole::Subscriber
                : LocalSubscriptionRole::Publisher;
        const auto opposite_subscription = active_subscriptions.find(
            SubscriptionIdentity{track, other_role});
        const auto additional_count =
            opposite_subscription == active_subscriptions.end() ? 1u : 2u;
        const auto additional_bytes = key_bytes * additional_count;
        const bool emit_optional = optional_subscription_evidence_available(
            additional_count, additional_bytes);
        request.subscription = RequestRecord::SubscriptionState{
            track, local_role, SubscriptionPhase::Pending, retained_bytes,
            publish ? std::optional<std::uint64_t>{publish->track_alias}
                    : std::nullopt,
            initial_forward,
            publish ? largest_object_parameter(publish->parameters)
                    : std::nullopt,
            std::nullopt};
        active_subscriptions.emplace(active_key, request.stream_id);
        ++subscription_history;
        subscription_key_bytes += retained_bytes;
        if (emit_optional && !emit(transition, EvidenceKind::SubscriptionCreated,
                  SubscriptionCreatedEvidence{
                      track, local_role, SubscriptionPhase::Pending,
                      request.initiator, request.request_id,
                      request.stream_id, initial_forward})) {
            return false;
        }
        if (emit_optional && opposite_subscription != active_subscriptions.end()) {
            const auto existing = requests.find(opposite_subscription->second);
            const auto existing_id =
                existing == requests.end() ? 0 : existing->second.request_id;
            return emit(transition, EvidenceKind::OppositeRoleCoexistence,
                        OppositeRoleCoexistenceEvidence{
                            track, existing_id, request.request_id});
        }
        return true;
    }

    bool validate_track_alias(SessionTransition& transition,
                              RequestInitiator publisher,
                              std::uint64_t alias, const TrackKey& track,
                              transport::StreamId stream_id) {
        const auto existing = established_aliases.find({publisher, alias});
        if (existing == established_aliases.end()) {
            return true;
        }
        const auto prior = requests.find(existing->second);
        if (prior != requests.end() && prior->second.subscription &&
            prior->second.subscription->track == track) return true;
        if (publisher == RequestInitiator::Peer) {
            protocol_close(transition, stream_id, 0x5,
                           "duplicate track alias");
        } else {
            emit(transition, EvidenceKind::LocalObservationError,
                 LocalObservationErrorEvidence{stream_id});
        }
        return false;
    }

    void establish_track_alias(RequestRecord& request,
                               RequestInitiator publisher,
                               std::uint64_t alias) {
        if (!request.subscription) return;
        request.subscription->track_alias = alias;
        established_aliases[{publisher, alias}] = request.stream_id;
    }

    void change_forward_state(SessionTransition& transition,
                              RequestRecord& request,
                              RequestInitiator actor, bool new_state,
                              bool joining_location_pending = false) {
        if (!request.subscription ||
            request.subscription->forward_state == new_state) return;
        const bool previous = request.subscription->forward_state;
        request.subscription->forward_state = new_state;
        if (!previous && new_state) {
            request.subscription->joining_location =
                joining_location_pending
                    ? std::nullopt
                    : request.subscription->largest_object;
        }
        if (optional_subscription_evidence_available(1, 0)) {
            emit(transition, EvidenceKind::ForwardStateChanged,
                 ForwardStateEvidence{request.request_id, request.stream_id,
                                      actor, previous, new_state,
                                      request.subscription->joining_location});
        }
    }

    void transition_subscription(SessionTransition& transition,
                                 RequestRecord& request,
                                 SubscriptionPhase next) {
        if (!request.subscription || request.subscription->phase == next) {
            return;
        }
        const auto previous = request.subscription->phase;
        request.subscription->phase = next;
        const auto retained_bytes = request.subscription->retained_key_bytes;
        if (next == SubscriptionPhase::Terminated) {
            if (request.subscription->track_alias) {
                const auto publisher = request.subscription->local_role ==
                        LocalSubscriptionRole::Publisher
                    ? RequestInitiator::Local
                    : RequestInitiator::Peer;
                const auto alias = established_aliases.find(
                    {publisher, *request.subscription->track_alias});
                if (alias != established_aliases.end() &&
                    alias->second == request.stream_id) {
                    established_aliases.erase(alias);
                }
            }
            active_subscriptions.erase(SubscriptionIdentity{
                request.subscription->track,
                request.subscription->local_role});
            subscription_key_bytes -= retained_bytes;
        }
        if (optional_subscription_evidence_available(
                1, track_key_bytes(request.subscription->track))) {
            emit(transition, EvidenceKind::SubscriptionPhaseChanged,
                 SubscriptionPhaseEvidence{
                     request.subscription->track,
                     request.subscription->local_role, request.request_id,
                     request.stream_id, previous, next});
        }
        if (next == SubscriptionPhase::Terminated) {
            request.subscription.reset();
        }
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
            !request.token_rejected &&
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
            if (!process_token_parameters(transition, initiator,
                                          request.stream_id,
                                          update.parameters, false)) {
                return !terminal();
            }
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
        const bool tokens_accepted = process_token_parameters(
            transition, initiator, request.stream_id,
            update.parameters, false);
        if (terminal()) return false;
        const bool subscriber_update = request.subscription &&
            ((request.subscription->local_role ==
                  LocalSubscriptionRole::Publisher &&
              initiator == RequestInitiator::Peer) ||
             (request.subscription->local_role ==
                  LocalSubscriptionRole::Subscriber &&
              initiator == RequestInitiator::Local));
        const auto forward = forward_parameter(update.parameters);
        const bool enables_forward = tokens_accepted && subscriber_update &&
            forward && *forward && !request.subscription->forward_state;
        request.outstanding_updates.push_back(
            RequestRecord::PendingUpdate{update.request_id, initiator,
                                         enables_forward});
        if (!tokens_accepted) return true;
        if (subscriber_update && forward) {
            change_forward_state(transition, request, initiator, *forward,
                                 enables_forward);
        }
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
        transition_subscription(transition, request,
                                SubscriptionPhase::Terminated);
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
        if (request.subscription) {
            if (request.subscription->track_alias) {
                const auto publisher = request.subscription->local_role ==
                        LocalSubscriptionRole::Publisher
                    ? RequestInitiator::Local
                    : RequestInitiator::Peer;
                established_aliases.erase(
                    {publisher, *request.subscription->track_alias});
            }
            reserved_request_terminal_evidence.push_back(EvidenceEvent{
                next_sequence++, EvidenceKind::SubscriptionPhaseChanged,
                SubscriptionPhaseEvidence{
                    request.subscription->track,
                    request.subscription->local_role, request.request_id,
                    request.stream_id, request.subscription->phase,
                    SubscriptionPhase::Terminated}});
            active_subscriptions.erase(SubscriptionIdentity{
                request.subscription->track,
                request.subscription->local_role});
            subscription_key_bytes -=
                request.subscription->retained_key_bytes;
            request.subscription.reset();
        }
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
            const auto* subscribe_ok =
                std::get_if<wire::draft18::SubscribeOkMessage>(&message);
            const auto alias = subscribe_ok
                ? std::optional<std::uint64_t>{subscribe_ok->track_alias}
                : request.subscription && request.kind == RequestKind::Publish
                    ? request.subscription->track_alias
                    : std::nullopt;
            const auto publisher = request.kind == RequestKind::Subscribe
                ? responder
                : request.initiator;
            if (!error && alias && request.subscription &&
                !validate_track_alias(transition, publisher, *alias,
                                      request.subscription->track,
                                      request.stream_id)) {
                return false;
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
            if (emitted && request.subscription && !error) {
                transition_subscription(transition, request,
                                        SubscriptionPhase::Established);
                if (alias) establish_track_alias(request, publisher, *alias);
                if (request.kind == RequestKind::Publish) {
                    const auto* parameters = message_parameters(message);
                    change_forward_state(
                        transition, request, responder,
                        parameters
                            ? forward_parameter(*parameters).value_or(true)
                            : true);
                }
            }
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
            message,
            std::nullopt};
        if (!error && pending->enables_forward && request.subscription) {
            const auto* ok =
                std::get_if<wire::draft18::RequestOkMessage>(&message);
            if (ok) {
                const auto location =
                    largest_object_parameter(ok->parameters);
                request.subscription->largest_object = location;
                request.subscription->joining_location = location;
                observed.joining_location = location;
            }
        }
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
                const auto advertised_cache_size =
                    setup_auth_cache_size(*setup);
                if (!advertised_cache_size) {
                    protocol_close(transition, stream_id,
                                   kKeyValueFormattingError,
                                   "invalid token cache option");
                    return;
                }
                local_token_cache_limit = std::min(
                    *advertised_cache_size,
                    config.maximum_auth_token_cache_bytes);
                if (!process_peer_setup_tokens(transition, stream_id,
                                               *setup)) return;
                if (local_setup_observed) {
                    apply_local_setup_tokens(
                        transition, *local_control_stream);
                    if (terminal()) return;
                }
                peer_setup_observed = true;
                emit(transition, EvidenceKind::PeerSetupReceived,
                     SetupEvidence{stream_id, *setup});
                if (terminal()) return;
                inspect_setup_duplicates(transition, stream_id, *setup);
                if (terminal()) return;
                if (config.webtransport) {
                    const auto has_option = [&](std::uint64_t type) {
                        return std::any_of(setup->options.begin(),
                                           setup->options.end(),
                                           [type](const auto& option) {
                                               return option.type == type;
                                           });
                    };
                    if (has_option(5u)) {
                        protocol_close(transition, stream_id, 0x19,
                                       "AUTHORITY forbidden over WebTransport");
                        return;
                    }
                    if (has_option(1u)) {
                        protocol_close(transition, stream_id, 0x8,
                                       "PATH forbidden over WebTransport");
                        return;
                    }
                }
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

    void observe_object(SessionTransition& transition,
                        wire::draft18::ObjectEvent object) {
        std::optional<std::uint64_t> request_id;
        std::optional<bool> forward_state;
        if (object.track_alias) {
            const auto alias = established_aliases.find(
                {RequestInitiator::Peer, *object.track_alias});
            if (alias != established_aliases.end()) {
                const auto request = requests.find(alias->second);
                if (request != requests.end() &&
                    request->second.subscription) {
                    request_id = request->second.request_id;
                    forward_state =
                        request->second.subscription->forward_state;
                }
            }
        }
        const bool forwarding_violation =
            request_id && forward_state && !*forward_state &&
            object.track_alias;
        const auto group_id = object.group_id;
        const auto object_id = object.object_id;
        const auto track_alias = object.track_alias;
        emit(transition, EvidenceKind::ObjectObserved,
             ObjectObservedEvidence{request_id, forward_state,
                                    std::move(object)});
        if (forwarding_violation && !terminal()) {
            emit(transition, EvidenceKind::ForwardStateViolation,
                 ForwardStateViolationEvidence{
                     *request_id, *track_alias, group_id, object_id});
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

        if (*stream.kind == PeerStreamKind::Subgroup &&
            phase == SessionPhase::Active &&
            !stream.data_deferred_before_active) {
            if (!stream.subgroup_decoder) {
                stream.subgroup_decoder.emplace(config.wire_limits);
            }
            const auto old_decoder_bytes =
                stream.subgroup_decoder->buffered_byte_count();
            auto decoded = stream.subgroup_decoder->push(
                stream.buffered, event.fin);
            partial_bytes -= old_decoder_bytes + stream.buffered.size();
            stream.buffered.clear();
            partial_bytes += stream.subgroup_decoder->buffered_byte_count();
            if (decoded.error) {
                if (decoded.error->code ==
                    wire::DecodeErrorCode::LengthExceedsLimit) {
                    harness_limit(
                        transition,
                        HarnessLimitKind::ObjectPropertiesBytes,
                        config.wire_limits.maximum_object_properties_length <
                                std::numeric_limits<std::size_t>::max()
                            ? config.wire_limits.maximum_object_properties_length + 1
                            : config.wire_limits.maximum_object_properties_length,
                        config.wire_limits.maximum_object_properties_length);
                    return;
                }
                protocol_close(transition, event.stream_id,
                               kProtocolViolation,
                               "invalid subgroup stream");
                return;
            }
            for (const auto& observation : decoded.observations) {
                if (observation.kind ==
                    wire::draft18::DecoderObservationKind::ShouldClose) {
                    protocol_close(transition, event.stream_id,
                                   kProtocolViolation,
                                   "subgroup stream ended mid-object");
                    return;
                }
                if (observation.kind ==
                    wire::draft18::DecoderObservationKind::DraftAmbiguity) {
                    emit(transition, EvidenceKind::DraftAmbiguity,
                         DraftAmbiguityEvidence{event.stream_id,
                                                observation.offset,
                                                observation.detail});
                    if (terminal()) return;
                }
            }
            for (auto& object : decoded.objects) {
                observe_object(transition, std::move(object));
                if (terminal()) return;
            }
            if (event.fin) {
                partial_bytes -=
                    stream.subgroup_decoder->buffered_byte_count();
                streams.erase(position);
            }
            return;
        }

        const auto buffered_size = stream.buffered.size();
        if (phase != SessionPhase::Active) {
            stream.data_deferred_before_active = true;
        }
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
                            if (stream->second.subgroup_decoder) {
                                partial_bytes -= stream->second
                                    .subgroup_decoder->buffered_byte_count();
                            }
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
                    if (phase != SessionPhase::Active) {
                        emit(transition, EvidenceKind::DeferredStreamBytes,
                             DeferredBytesEvidence{
                                 0, PeerStreamKind::Subgroup, value.data,
                                 false});
                        return;
                    }
                    auto decoded = wire::draft18::decode_datagram(
                        value.data, config.wire_limits);
                    if (auto* object =
                            std::get_if<wire::draft18::ObjectEvent>(&decoded)) {
                        observe_object(transition, std::move(*object));
                    } else {
                        emit(transition, EvidenceKind::DeferredStreamBytes,
                             DeferredBytesEvidence{
                                 0, PeerStreamKind::Subgroup, value.data,
                                 false});
                    }
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
        const auto advertised_cache_size = setup_auth_cache_size(*setup);
        const auto local_tokens = setup_tokens(*setup);
        const bool invalid_alias_reference =
            local_tokens && std::any_of(
                local_tokens->begin(), local_tokens->end(),
                [](const wire::draft18::Token& token) {
                    return token.alias_type ==
                               wire::draft18::TokenAliasType::Delete ||
                           token.alias_type ==
                               wire::draft18::TokenAliasType::UseAlias;
                });
        if (!advertised_cache_size || !local_tokens ||
            invalid_alias_reference ||
            *advertised_cache_size >
                impl_->config.maximum_auth_token_cache_bytes) {
            impl_->emit(transition, EvidenceKind::LocalObservationError,
                        LocalObservationErrorEvidence{stream_id});
            return transition;
        }
        impl_->peer_token_cache_limit = *advertised_cache_size;
        impl_->pending_local_setup_tokens = *local_tokens;
        if (impl_->peer_setup_observed &&
            !impl_->apply_local_setup_tokens(transition, stream_id)) {
            return transition;
        }
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
