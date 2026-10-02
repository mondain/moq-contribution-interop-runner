#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/draft18/messages.h"
#include <algorithm>
#include <stdexcept>
#include <set>
#include <utility>

namespace moq::interop::scenarios {
namespace {
constexpr std::size_t kMaximumEvents = 4096;
constexpr std::size_t kMaximumSetupBytes = 65546;
bool staged(const RawProbeDefinition& definition) {
    return std::any_of(definition.writes.begin(),definition.writes.end(),[](const auto& write) {
        return write.reuse_write_stream.has_value() || static_cast<bool>(write.peer_response_ready) ||
               static_cast<bool>(write.evidence_ready) || static_cast<bool>(write.prepare_bytes);
    });
}
bool valid_stages(const RawProbeDefinition& definition) {
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < definition.writes.size(); ++i) {
        const auto& write = definition.writes[i];
        if (write.prepare_bytes && (!write.bytes.empty() || write.operation != RawProbeOperation::Write ||
                                    !definition.start_after_peer_setup))
            return false;
        if (write.operation != RawProbeOperation::Write && write.operation != RawProbeOperation::StopSending)
            return false;
        if (write.operation == RawProbeOperation::StopSending &&
            (!write.reuse_write_stream || !write.bytes.empty() || write.fin ||
             write.application_error >= (std::uint64_t{1} << 62))) return false;
        if (write.peer_response_ready && !write.reuse_write_stream) return false;
        roots.push_back(i);
        if (!write.reuse_write_stream) continue;
        const auto previous = *write.reuse_write_stream;
        if (previous >= i ||
            (write.channel != RawProbeChannel::NewBidi && write.channel != RawProbeChannel::PeerBidi) ||
            write.channel != definition.writes[previous].channel) return false;
        roots.back() = roots[previous];
        for (std::size_t j = previous; j < i; ++j) {
            if (roots[j] != roots.back()) continue;
            // Local FIN closes only our sending direction. STOP cancels the
            // independently open receiving direction; further writes cannot.
            if ((definition.writes[j].fin && write.operation == RawProbeOperation::Write) ||
                (j != previous && write.peer_response_ready)) return false;
        }
    }
    return true;
}
bool stream_continues(const RawProbeTranscript& transcript, transport::StreamId id,
                      std::size_t end) {
    for (std::size_t i = 0; i < end; ++i) {
        const auto& event = transcript.events[i];
        if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
            reset && reset->stream_id == id) return false;
        if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event);
            stop && stop->stream_id == id) return false;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && data->stream_id == id && data->fin) return false;
    }
    return true;
}
enum class GateState { Pending, Ready, Cancelled, LimitExceeded };
GateState evidence_gate(std::span<const RawProbeAcceptedWrite> prior_writes,
                        std::span<const transport::TransportEvent> events,
                        const std::function<bool(const RawProbeGateInput&)>& ready) {
    if (events.size() > kMaximumEvents) return GateState::LimitExceeded;
    std::size_t bytes = 0;
    const auto fits = [&](std::size_t size) {
        if (size > kMaximumSetupBytes - bytes) return false;
        bytes += size;
        return true;
    };
    for (const auto& event : events) {
        if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&event)) {
            if (!fits(established->alpn.size()) || !fits(established->local_connection_id.size()) ||
                !fits(established->peer_connection_id.size())) return GateState::LimitExceeded;
        } else if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (!fits(data->data.size())) return GateState::LimitExceeded;
        } else if (const auto* datagram = std::get_if<transport::DatagramEvent>(&event)) {
            if (!fits(datagram->data.size())) return GateState::LimitExceeded;
        } else if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
            if (!fits(close->reason.size())) return GateState::LimitExceeded;
        } else if (const auto* close = std::get_if<transport::LocalCloseEvent>(&event)) {
            if (!fits(close->reason.size())) return GateState::LimitExceeded;
        }
    }
    return ready({prior_writes,events}) ? GateState::Ready : GateState::Pending;
}
GateState response_gate(const RawProbeTranscript& transcript, transport::StreamId id,
                        std::size_t begin, std::size_t end,
                        const std::function<bool(std::span<const std::byte>)>& ready) {
    if (begin > end || end > transcript.events.size()) return GateState::Pending;
    if (!stream_continues(transcript,id,end)) return GateState::Cancelled;
    std::vector<std::byte> response;
    for (std::size_t i = begin; i < end; ++i) {
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&transcript.events[i]);
            data && data->stream_id == id) {
            if (data->data.size() > kMaximumSetupBytes - response.size()) return GateState::LimitExceeded;
            response.insert(response.end(),data->data.begin(),data->data.end());
        }
    }
    return !response.empty() && ready(response) ? GateState::Ready : GateState::Pending;
}
bool setup_before(const RawProbeTranscript& transcript, const RawProbeDefinition& definition,
                  std::size_t end) {
    bool connection = false;
    bool setup = !definition.start_after_peer_setup;
    std::map<transport::StreamId,std::vector<std::byte>> candidates;
    std::size_t count = 0;
    for (std::size_t i = 0; i < end; ++i) {
        const auto& event = transcript.events[i];
        if (std::holds_alternative<transport::ConnectionEstablishedEvent>(event)) connection = true;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && !setup && (data->stream_id & 3u) == 2u) {
            if (!connection || (!candidates.contains(data->stream_id) && candidates.size() >= 64) ||
                data->data.size() > kMaximumSetupBytes - count) return false;
            auto& candidate = candidates[data->stream_id];
            candidate.insert(candidate.end(),data->data.begin(),data->data.end());
            count += data->data.size();
            setup = definition.peer_setup_ready && definition.peer_setup_ready(candidate);
        }
        if (std::holds_alternative<transport::PeerCloseEvent>(event)) return false;
    }
    return connection && setup;
}
bool accepted(const RawProbeAcceptedWrite& observed, const RawProbeWrite& expected) {
    return (expected.channel == RawProbeChannel::Datagram ? !observed.stream_id : observed.stream_id.has_value()) &&
           observed.write.channel == expected.channel &&
           observed.write.bytes == expected.bytes && observed.write.fin == expected.fin &&
           observed.write.reuse_write_stream == expected.reuse_write_stream &&
           static_cast<bool>(observed.write.peer_response_ready) == static_cast<bool>(expected.peer_response_ready) &&
           observed.write.operation == expected.operation &&
           observed.write.application_error == expected.application_error &&
           static_cast<bool>(observed.write.evidence_ready) == static_cast<bool>(expected.evidence_ready) &&
           static_cast<bool>(observed.write.prepare_bytes) == static_cast<bool>(expected.prepare_bytes) &&
           observed.operation_accepted == (expected.operation == RawProbeOperation::StopSending) &&
           observed.accepted == expected.bytes.size() &&
           observed.fin_accepted == expected.fin;
}
}
RawProbeController::RawProbeController(transport::SessionTransport& transport,
                                     RawProbeDefinition definition)
    : transport_(transport), definition_(std::move(definition)) {
    if (definition_.id.empty() || definition_.setup_bytes.empty() ||
        definition_.deadline.count() <= 0 || !valid_stages(definition_) ||
        (definition_.start_after_peer_setup && !definition_.peer_setup_ready) ||
        (std::any_of(definition_.writes.begin(), definition_.writes.end(), [](const auto& write) {
            return write.channel == RawProbeChannel::PeerBidi;
        }) && !definition_.peer_request_ready) ||
        (definition_.acknowledge_publisher_namespace &&
         std::any_of(definition_.writes.begin(), definition_.writes.end(), [](const auto& write) {
             return write.channel == RawProbeChannel::PeerBidi;
         })))
        throw std::invalid_argument("invalid raw probe definition");
    transcript_.scenario_id = definition_.id;
    transcript_.setup.write = {RawProbeChannel::NewUni, definition_.setup_bytes, false};
    for (const auto& write : definition_.writes)
        transcript_.writes.push_back({write, {}, 0, false});
}
void RawProbeController::fail() {
    transcript_.harness_failed = true;
    transcript_.complete = false;
}
bool RawProbeController::flush(RawProbeAcceptedWrite& pending) {
    if (pending.write.channel == RawProbeChannel::Datagram) {
        if (pending.stream_id || pending.write.fin || pending.write.bytes.empty() ||
            pending.write.bytes.size() > transcript_.max_datagram_payload) { fail(); return false; }
        if (pending.accepted == pending.write.bytes.size()) return true;
        const auto result = transport_.send_datagram(pending.write.bytes);
        if (result.status == transport::TransportStatus::WouldBlock && result.accepted == 0) return false;
        if (result.status != transport::TransportStatus::Success || result.accepted != pending.write.bytes.size()) {
            fail(); return false;
        }
        pending.accepted = result.accepted;
        return true;
    }
    if (!pending.stream_id) {
        if (pending.write.reuse_write_stream) {
            pending.stream_id = transcript_.writes[*pending.write.reuse_write_stream].stream_id;
        } else if (pending.write.channel == RawProbeChannel::PeerBidi) {
            if (!peer_request_stream_) return false;
            pending.stream_id = peer_request_stream_;
        } else if (pending.write.channel == RawProbeChannel::Control) {
            pending.stream_id = transcript_.setup.stream_id;
        } else {
            const auto opened = pending.write.channel == RawProbeChannel::NewUni
                ? transport_.open_uni() : transport_.open_bidi();
            if (opened.status == transport::TransportStatus::WouldBlock ||
                opened.status == transport::TransportStatus::StreamLimit) return false;
            if (opened.status != transport::TransportStatus::Success) { fail(); return false; }
            pending.stream_id = opened.stream_id;
        }
    }
    if (!pending.stream_id) { fail(); return false; }
    if (cancelled_peer_requests_.contains(*pending.stream_id)) return false;
    if (pending.write.operation == RawProbeOperation::StopSending) {
        if (pending.operation_accepted) return true;
        const auto result = transport_.stop_sending(*pending.stream_id,pending.write.application_error);
        if (result.status == transport::TransportStatus::WouldBlock && result.accepted == 0) return false;
        if (result.status != transport::TransportStatus::Success || result.accepted != 0) {
            fail(); return false;
        }
        pending.operation_accepted = true;
        return true;
    }
    if (pending.accepted < pending.write.bytes.size()) {
        const auto remaining = std::span<const std::byte>(pending.write.bytes).subspan(pending.accepted);
        const auto result = transport_.write(*pending.stream_id, remaining, false);
        if (result.status == transport::TransportStatus::WouldBlock && result.accepted == 0) return false;
        if ((result.status != transport::TransportStatus::Success &&
             result.status != transport::TransportStatus::Partial) ||
            result.accepted > remaining.size() ||
            (result.status == transport::TransportStatus::Success && result.accepted != remaining.size())) {
            fail(); return false;
        }
        pending.accepted += result.accepted;
        if (pending.accepted != pending.write.bytes.size()) return false;
    }
    if (pending.write.fin && !pending.fin_accepted) {
        const auto result = transport_.write(*pending.stream_id, {}, true);
        if (result.status == transport::TransportStatus::WouldBlock && result.accepted == 0) return false;
        if (result.status != transport::TransportStatus::Success || result.accepted != 0) {
            fail(); return false;
        }
        pending.fin_accepted = true;
    }
    return true;
}
void RawProbeController::acknowledge_publisher_namespaces() {
    static const std::vector<std::byte> request_ok = [] {
        wire::ByteWriter output(16);
        wire::draft18::encode_message(wire::draft18::RequestOkMessage{{}, {}}, output);
        return std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
    }();
    for (auto it = acknowledgement_pending_.begin(); it != acknowledgement_pending_.end();) {
        const auto id = *it;
        if (cancelled_peer_requests_.contains(id)) { it = acknowledgement_pending_.erase(it); continue; }
        const auto result = transport_.write(id, request_ok, false);
        if (result.status == transport::TransportStatus::WouldBlock && result.accepted == 0) { ++it; continue; }
        if (result.status != transport::TransportStatus::Success || result.accepted != request_ok.size()) {
            // A closing or reset stream cannot be answered; nothing is scored from it.
            it = acknowledgement_pending_.erase(it);
            continue;
        }
        acknowledged_.insert(id);
        transcript_.acknowledgements.push_back({id, transcript_.events.size()});
        it = acknowledgement_pending_.erase(it);
    }
}
const RawProbeTranscript& RawProbeController::poll(RawProbeClock::time_point now) {
    if (transcript_.complete || transcript_.harness_failed || transcript_.timed_out) return transcript_;
    if (!started_at_) started_at_ = now;
    const auto send = [&] {
        if (!transcript_.transport_established || transcript_.harness_failed ||
            !flush(transcript_.setup)) return;
        if (!transcript_.setup.delivery_event_count)
            transcript_.setup.delivery_event_count = transcript_.events.size();
        if (definition_.start_after_peer_setup && !transcript_.peer_setup_received) return;
        if (definition_.peer_request_ready && !peer_request_stream_) {
            for (const auto& [id, candidate] : peer_request_candidates_) {
                if (!cancelled_peer_requests_.contains(id) && definition_.peer_request_ready(candidate)) {
                    peer_request_stream_ = id;
                    break;
                }
            }
        }
        if (peer_request_stream_ && cancelled_peer_requests_.contains(*peer_request_stream_)) return;
        while (next_write_ < transcript_.writes.size()) {
            auto& pending = transcript_.writes[next_write_];
            if (pending.write.reuse_write_stream) {
                const auto& previous = transcript_.writes[*pending.write.reuse_write_stream];
                if (!previous.delivery_event_count || !previous.stream_id) return;
                if (!stream_continues(transcript_,*previous.stream_id,transcript_.events.size())) return;
                if (pending.write.peer_response_ready && !pending.prepared_event_count) {
                    const auto gate = response_gate(transcript_,*previous.stream_id,
                        *previous.delivery_event_count,transcript_.events.size(),pending.write.peer_response_ready);
                    if (gate == GateState::LimitExceeded) fail();
                    if (gate != GateState::Ready) return;
                }
            } else if (staged(definition_) && pending.write.channel == RawProbeChannel::PeerBidi &&
                       peer_request_stream_ && !stream_continues(transcript_,*peer_request_stream_,transcript_.events.size())) return;
            if (pending.write.evidence_ready && !pending.prepared_event_count) {
                const auto gate = evidence_gate(std::span<const RawProbeAcceptedWrite>(transcript_.writes).first(next_write_),
                    transcript_.events,pending.write.evidence_ready);
                if (gate == GateState::LimitExceeded) fail();
                if (gate != GateState::Ready) return;
            }
            if (pending.write.prepare_bytes && pending.write.channel == RawProbeChannel::PeerBidi &&
                !pending.write.reuse_write_stream && !peer_request_stream_) return;
            if (pending.write.prepare_bytes && !pending.prepared_event_count) {
                const auto prior = std::span<const RawProbeAcceptedWrite>(transcript_.writes).first(next_write_);
                if (evidence_gate(prior,transcript_.events,[](const auto&){return true;}) != GateState::Ready) {
                    fail(); return;
                }
                auto bytes = pending.write.prepare_bytes({prior,transcript_.events});
                if (!bytes) return;
                if (bytes->empty() || bytes->size() > kMaximumSetupBytes) { fail(); return; }
                pending.write.bytes = std::move(*bytes);
                pending.prepared_event_count = transcript_.events.size();
            }
            if (!flush(pending)) return;
            pending.delivery_event_count = transcript_.events.size();
            ++next_write_;
        }
        if (!transcript_.stimulus_delivered) {
            transcript_.stimulus_delivered = true;
            transcript_.delivery_event_count = transcript_.events.size();
            delivered_at_ = now;
        }
    };
    for (auto& event : transport_.poll(256)) {
        if (transcript_.events.size() >= kMaximumEvents) { fail(); break; }
        transcript_.events.push_back(std::move(event));
        const auto& observed = transcript_.events.back();
        if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&observed)) {
            if (transcript_.transport_established) { fail(); break; }
            transcript_.transport_established = true;
            transcript_.max_datagram_payload = established->max_datagram_payload;
        } else if (const auto* data = std::get_if<transport::StreamDataEvent>(&observed)) {
            // Client control streams are unidirectional; never mistake a request response for SETUP.
            if (!transcript_.peer_setup_received && (data->stream_id & 3u) == 2u) {
                if (!transcript_.transport_established) { fail(); break; }
                if ((!peer_setup_candidates_.contains(data->stream_id) && peer_setup_candidates_.size() >= 64) ||
                    data->data.size() > kMaximumSetupBytes - peer_setup_bytes_count_) { fail(); break; }
                auto& candidate = peer_setup_candidates_[data->stream_id];
                candidate.insert(candidate.end(), data->data.begin(), data->data.end());
                peer_setup_bytes_count_ += data->data.size();
                if (definition_.peer_setup_ready && definition_.peer_setup_ready(candidate))
                    transcript_.peer_setup_received = true;
            }
            if (definition_.acknowledge_publisher_namespace && (data->stream_id & 3u) == 0u &&
                !acknowledged_.contains(data->stream_id) &&
                !acknowledgement_pending_.contains(data->stream_id) &&
                (acknowledgement_candidates_.contains(data->stream_id) || acknowledgement_candidates_.size() < 64)) {
                auto& candidate = acknowledgement_candidates_[data->stream_id];
                if (candidate.size() + data->data.size() <= kMaximumSetupBytes) {
                    candidate.insert(candidate.end(), data->data.begin(), data->data.end());
                    wire::Cursor cursor(candidate);
                    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request, cursor, {});
                    const auto* message = std::get_if<wire::draft18::Message>(&decoded);
                    const auto* announce = message ? std::get_if<wire::draft18::PublishNamespaceMessage>(message) : nullptr;
                    if (announce && announce->parameters.empty() && !announce->track_namespace.fields.empty() &&
                        !(announce->track_namespace.fields.front().size() == 1 &&
                          announce->track_namespace.fields.front().front() == std::byte{'.'}))
                        acknowledgement_pending_.insert(data->stream_id);
                }
            }
            const bool opener_accepted = std::any_of(transcript_.writes.begin(),transcript_.writes.end(),[](const auto& write) {
                return write.write.channel == RawProbeChannel::PeerBidi && !write.write.reuse_write_stream && write.delivery_event_count;
            });
            if (definition_.peer_request_ready && !transcript_.stimulus_delivered && !opener_accepted &&
                (data->stream_id & 3u) == 0u) {
                if (!transcript_.transport_established) { fail(); break; }
                if ((!peer_request_candidates_.contains(data->stream_id) && peer_request_candidates_.size() >= 64) ||
                    data->data.size() > kMaximumSetupBytes - peer_request_bytes_count_) { fail(); break; }
                auto& candidate = peer_request_candidates_[data->stream_id];
                candidate.insert(candidate.end(), data->data.begin(), data->data.end());
                peer_request_bytes_count_ += data->data.size();
            }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&observed)) {
            if ((reset->stream_id & 2u) == 0u) cancelled_peer_requests_.insert(reset->stream_id);
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&observed)) {
            if ((stop->stream_id & 2u) == 0u) cancelled_peer_requests_.insert(stop->stream_id);
        } else if (std::holds_alternative<transport::PeerCloseEvent>(observed)) {
            transcript_.complete = transcript_.stimulus_delivered;
            if (!transcript_.complete) transcript_.timed_out = true;
            return transcript_;
        } else if (std::holds_alternative<transport::LocalCloseEvent>(observed) ||
                   std::holds_alternative<transport::IdleTimeoutEvent>(observed)) {
            transcript_.timed_out = true;
            return transcript_;
        } else if (std::holds_alternative<transport::TransportErrorEvent>(observed) ||
                   std::holds_alternative<transport::EventQueueOverflowEvent>(observed)) {
            fail(); break;
        }
    }
    // All events in this batch were already observed before any new writes.
    if (!transcript_.harness_failed && transcript_.transport_established) acknowledge_publisher_namespaces();
    if (!transcript_.harness_failed) send();
    if (transcript_.stimulus_delivered && !transcript_.harness_failed &&
        definition_.response_ready && definition_.response_ready(transcript_)) {
        transcript_.complete = true;
        return transcript_;
    }
    const auto timeout_start = delivered_at_.value_or(*started_at_);
    if (!transcript_.complete && now - timeout_start >= definition_.deadline)
        transcript_.timed_out = true;
    return transcript_;
}
const RawProbeTranscript& RawProbeController::transcript() const noexcept { return transcript_; }

bool raw_probe_stimulus_valid(const RawProbeTranscript& transcript,
                             const RawProbeDefinition& definition) {
    if (!valid_stages(definition) || transcript.events.size() > kMaximumEvents ||
        transcript.scenario_id != definition.id || !transcript.complete ||
        transcript.harness_failed || transcript.timed_out || !transcript.stimulus_delivered ||
        !transcript.delivery_event_count || *transcript.delivery_event_count > transcript.events.size() ||
        transcript.setup.prepared_event_count ||
        !accepted(transcript.setup, {RawProbeChannel::NewUni, definition.setup_bytes, false}) ||
        transcript.writes.size() != definition.writes.size()) return false;
    std::size_t datagram_capacity = 0;
    bool established_seen = false;
    for (std::size_t i = 0; i < transcript.events.size(); ++i) {
        if (const auto* established = std::get_if<transport::ConnectionEstablishedEvent>(&transcript.events[i])) {
            if (established_seen || i >= *transcript.delivery_event_count) return false;
            established_seen = true;
            datagram_capacity = established->max_datagram_payload;
        }
    }
    if (transcript.setup.delivery_event_count) {
        const auto setup_marker = *transcript.setup.delivery_event_count;
        if (setup_marker > *transcript.delivery_event_count) return false;
        const auto setup_events = std::span<const transport::TransportEvent>(transcript.events).first(setup_marker);
        if (!std::any_of(setup_events.begin(),setup_events.end(),
                [](const auto& event){return std::holds_alternative<transport::ConnectionEstablishedEvent>(event);}))
            return false;
    }
    if ((*transcript.setup.stream_id & 3u) != 3u) return false;
    std::set<transport::StreamId> opened{*transcript.setup.stream_id};
    std::optional<transport::StreamId> peer_target;
    const bool marked = staged(definition) || std::any_of(transcript.writes.begin(),transcript.writes.end(),[](const auto& write) {
        return write.delivery_event_count.has_value();
    });
    std::size_t previous_marker = 0;
    for (std::size_t i = 0; i < definition.writes.size(); ++i) {
        const auto& write = transcript.writes[i];
        auto expected = definition.writes[i];
        const auto marker = write.delivery_event_count.value_or(*transcript.delivery_event_count);
        const auto gate_marker = write.prepared_event_count.value_or(marker);
        if (expected.prepare_bytes) {
            if (!write.prepared_event_count || !transcript.setup.delivery_event_count ||
                *transcript.setup.delivery_event_count > gate_marker ||
                gate_marker < previous_marker || gate_marker > marker ||
                marker > *transcript.delivery_event_count) return false;
            const auto prior = std::span<const RawProbeAcceptedWrite>(transcript.writes).first(i);
            const auto events = std::span<const transport::TransportEvent>(transcript.events).first(gate_marker);
            if (evidence_gate(prior,events,[](const auto&){return true;}) != GateState::Ready ||
                !setup_before(transcript,definition,gate_marker)) return false;
            // Validate transport metadata before gates can inspect stream IDs.
            auto metadata = expected;
            metadata.bytes = write.write.bytes;
            if (!accepted(write,metadata)) return false;
        } else if (write.prepared_event_count) return false;
        if (!expected.prepare_bytes && !accepted(write, expected)) return false;
        if (marked && !write.delivery_event_count) return false;
        if (marker < previous_marker || marker > *transcript.delivery_event_count ||
            !setup_before(transcript,definition,marker)) return false;
        previous_marker = marker;
        if (definition.writes[i].reuse_write_stream) {
            const auto& previous = transcript.writes[*definition.writes[i].reuse_write_stream];
            if (write.stream_id != previous.stream_id || !previous.delivery_event_count ||
                !stream_continues(transcript,*write.stream_id,marker)) return false;
            if (definition.writes[i].peer_response_ready &&
                response_gate(transcript,*write.stream_id,*previous.delivery_event_count,gate_marker,
                    definition.writes[i].peer_response_ready) != GateState::Ready) return false;
        } else if (write.write.channel == RawProbeChannel::Datagram) {
            if (write.write.fin || write.write.bytes.empty() ||
                write.write.bytes.size() > datagram_capacity ||
                transcript.max_datagram_payload != datagram_capacity) return false;
        } else if (write.write.channel == RawProbeChannel::PeerBidi) {
            if (!definition.peer_request_ready || (*write.stream_id & 3u) != 0u) return false;
            if (peer_target && peer_target != write.stream_id) return false;
            if (staged(definition) && !stream_continues(transcript,*write.stream_id,marker)) return false;
            peer_target = write.stream_id;
            std::vector<std::byte> request;
            bool connection_seen = false;
            std::size_t all_request_bytes = 0;
            std::set<transport::StreamId> candidates;
            for (std::size_t event_index = 0; event_index < gate_marker; ++event_index) {
                const auto& event = transcript.events[event_index];
                if (std::holds_alternative<transport::ConnectionEstablishedEvent>(event)) connection_seen = true;
                if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
                    data && (data->stream_id & 3u) == 0u) {
                    if (!connection_seen || data->data.size() > kMaximumSetupBytes - all_request_bytes) return false;
                    all_request_bytes += data->data.size();
                    candidates.insert(data->stream_id);
                    if (candidates.size() > 64) return false;
                }
                if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
                    data && data->stream_id == *write.stream_id) {
                    if (data->data.size() > kMaximumSetupBytes - request.size()) return false;
                    request.insert(request.end(), data->data.begin(), data->data.end());
                } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
                           reset && reset->stream_id == *write.stream_id) return false;
                else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event);
                         stop && stop->stream_id == *write.stream_id) return false;
            }
            if (!definition.peer_request_ready(request)) return false;
        } else if (write.write.channel == RawProbeChannel::Control) {
            if (write.stream_id != transcript.setup.stream_id) return false;
        } else {
            const auto kind = write.write.channel == RawProbeChannel::NewUni ? 3u : 1u;
            if ((*write.stream_id & 3u) != kind || !opened.insert(*write.stream_id).second)
                return false;
        }
        if (definition.writes[i].evidence_ready &&
            evidence_gate(std::span<const RawProbeAcceptedWrite>(transcript.writes).first(i),
                std::span<const transport::TransportEvent>(transcript.events).first(gate_marker),
                definition.writes[i].evidence_ready) != GateState::Ready) return false;
        if (expected.prepare_bytes) {
            const auto prior = std::span<const RawProbeAcceptedWrite>(transcript.writes).first(i);
            const auto events = std::span<const transport::TransportEvent>(transcript.events).first(gate_marker);
            const auto bytes = expected.prepare_bytes({prior,events});
            if (!bytes || bytes->empty() || bytes->size() > kMaximumSetupBytes) return false;
            expected.bytes = *bytes;
            if (!accepted(write,expected)) return false;
        }
    }
    if (marked && !transcript.writes.empty() && previous_marker != *transcript.delivery_event_count) return false;
    bool terminated = false;
    for (const auto& event : transcript.events) {
        if (terminated) return false;
        terminated = std::holds_alternative<transport::PeerCloseEvent>(event);
        if (std::holds_alternative<transport::EventQueueOverflowEvent>(event) ||
            std::holds_alternative<transport::TransportErrorEvent>(event) ||
            std::holds_alternative<transport::LocalCloseEvent>(event) ||
            std::holds_alternative<transport::IdleTimeoutEvent>(event)) return false;
    }
    return setup_before(transcript,definition,*transcript.delivery_event_count);
}

std::optional<bool> evaluate_raw_probe_close(const RawProbeTranscript& transcript,
                                           const RawProbeDefinition& definition,
                                           std::optional<std::uint64_t> expected_close) {
    if (!raw_probe_stimulus_valid(transcript, definition)) return std::nullopt;
    for (const auto& event : transcript.events) {
        if (const auto* close = std::get_if<transport::PeerCloseEvent>(&event)) {
            if (close->error_space != transport::CloseErrorSpace::Application) return std::nullopt;
            return !expected_close || close->error_code == *expected_close;
        }
    }
    return std::nullopt;
}
}  // namespace moq::interop::scenarios
