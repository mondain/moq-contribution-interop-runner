#include "moq/interop/session/publisher_session.h"

#include "session/draft18_dispatch_internal.h"

#include <algorithm>
#include <deque>
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
           config.maximum_evidence_bytes != 0;
}

bool peer_initiated(transport::StreamId stream_id) {
    const auto type = stream_id & 0x3u;
    return type == 0u || type == 2u;
}

bool unidirectional(transport::StreamId stream_id) {
    return (stream_id & 0x2u) != 0u;
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
    };

    PublisherSessionConfig config;
    SessionPhase phase{SessionPhase::AwaitingTransport};
    bool transport_established{false};
    std::optional<transport::StreamId> local_control_stream;
    bool local_setup_observed{false};
    std::optional<transport::StreamId> peer_control_stream;
    bool peer_setup_observed{false};
    std::unordered_map<transport::StreamId, StreamState> streams;
    std::size_t partial_bytes{0};
    std::size_t early_streams{0};
    std::size_t early_bytes{0};
    std::deque<EvidenceEvent> evidence;
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

    void protocol_close(SessionTransition& transition,
                        std::optional<transport::StreamId> stream_id,
                        std::uint64_t code, const char* reason) {
        auto bytes = reason_bytes(reason);
        close(transition, EvidenceKind::ProtocolViolation,
              ProtocolViolationEvidence{stream_id, code, bytes}, code,
              std::move(bytes));
    }

    void update_active() {
        if (phase == SessionPhase::AwaitingSetup && transport_established &&
            local_setup_observed && peer_setup_observed) {
            phase = SessionPhase::Active;
            early_streams = 0;
            early_bytes = 0;
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
                update_active();
            } else if (!peer_setup_observed) {
                protocol_close(transition, stream_id, kProtocolViolation,
                               "control stream did not begin with SETUP");
                return;
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
        if (!peer_initiated(event.stream_id)) {
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
            if (created && !count_new_stream(transition, event.stream_id,
                                             stream, false)) {
                return;
            }
            if (!stream.kind) {
                stream.kind = PeerStreamKind::Request;
                emit(transition, EvidenceKind::PeerStreamClassified,
                     StreamEvidence{event.stream_id,
                                    PeerStreamKind::Request});
            }
            if (!append(transition, stream, event.data, false)) {
                return;
            }
            wire::Cursor type_cursor(stream.buffered);
            const auto first_type = wire::read_vi64(type_cursor);
            if (std::holds_alternative<std::uint64_t>(first_type) &&
                std::get<std::uint64_t>(first_type) == 0x2f00) {
                protocol_close(transition, event.stream_id,
                               kProtocolViolation,
                               "SETUP on bidirectional stream");
                return;
            }
            if (event.fin) {
                const auto buffered_size = stream.buffered.size();
                emit(transition, EvidenceKind::DeferredStreamBytes,
                     DeferredBytesEvidence{event.stream_id,
                                           PeerStreamKind::Request,
                                           std::move(stream.buffered), true});
                partial_bytes -= buffered_size;
                streams.erase(position);
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
                    update_active();
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
                },
                [&](const transport::DatagramEvent& value) {
                    emit(transition, EvidenceKind::DeferredStreamBytes,
                         DeferredBytesEvidence{0, PeerStreamKind::Subgroup,
                                               value.data, false});
                },
                [&](const transport::PeerCloseEvent& value) {
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::PeerClose,
                                 make_bounded_close_evidence(
                                     value.error_space, value.error_code,
                                     value.reason,
                                     remaining_evidence_bytes()));
                },
                [&](const transport::LocalCloseEvent& value) {
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::LocalClose,
                                 make_bounded_close_evidence(
                                     value.error_space, value.error_code,
                                     value.reason,
                                     remaining_evidence_bytes()));
                },
                [&](const transport::IdleTimeoutEvent&) {
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::IdleTimeout, MarkerEvidence{});
                },
                [&](const transport::TransportErrorEvent& value) {
                    phase = SessionPhase::Closed;
                    add_reserved(EvidenceKind::TransportError,
                                 TransportErrorEvidence{value.error});
                },
                [&](const transport::EventQueueOverflowEvent&) {
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
         impl_->local_control_stream.has_value())) {
        impl_->emit(transition, EvidenceKind::LocalObservationError,
                    LocalObservationErrorEvidence{stream_id});
        return transition;
    }
    if (purpose == LocalStreamPurpose::Control) {
        impl_->local_control_stream = stream_id;
    }
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
    if (!setup || impl_->local_control_stream != stream_id ||
        impl_->local_setup_observed || fin) {
        impl_->emit(transition, EvidenceKind::LocalObservationError,
                    LocalObservationErrorEvidence{stream_id});
        return transition;
    }
    impl_->local_setup_observed = true;
    impl_->emit(transition, EvidenceKind::LocalSetupObserved,
                SetupEvidence{stream_id, *setup});
    impl_->update_active();
    return transition;
}

std::vector<EvidenceEvent> PublisherSession::take_evidence(
    std::size_t maximum) {
    std::vector<EvidenceEvent> output;
    if (!impl_ || maximum == 0) return output;
    const auto count = std::min(maximum, impl_->evidence.size());
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        impl_->evidence_bytes -= evidence_owned_bytes(impl_->evidence.front().data);
        output.push_back(std::move(impl_->evidence.front()));
        impl_->evidence.pop_front();
    }
    return output;
}

SessionPhase PublisherSession::phase() const noexcept {
    return impl_ ? impl_->phase : SessionPhase::Closed;
}

}  // namespace moq::interop::session
