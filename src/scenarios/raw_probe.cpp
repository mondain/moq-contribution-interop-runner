#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/draft18/messages.h"
#include "moq/interop/wire/draft21/request_ok.h"
#include "moq/interop/scenarios/raw_probe_liveness.h"
#include "raw_probe_courtesy.h"
#include <algorithm>
#include <stdexcept>
#include <set>
#include <utility>

namespace moq::interop::scenarios {
namespace {
constexpr std::size_t kMaximumEvents = 4096;
constexpr std::size_t kMaximumSetupBytes = 65546;
// A complete draft 21 PUBLISH_NAMESPACE (Section 9.14: Request ID, Track Namespace,
// Parameters) that is answerable by a bare REQUEST_OK: no Parameters and not the
// reserved "." namespace. Anything malformed or incomplete is left unanswered.
bool answerable_draft21_announcement(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto vi = [&cursor]() -> std::optional<std::uint64_t> {
        auto value = wire::read_vi64(cursor);
        if (const auto* v = std::get_if<std::uint64_t>(&value)) return *v;
        return std::nullopt;
    };
    const auto take = [&cursor](std::uint64_t length) -> std::optional<std::span<const std::byte>> {
        if (length > 65535) return std::nullopt;
        auto value = wire::read_bytes(cursor, static_cast<std::size_t>(length));
        if (const auto* v = std::get_if<std::span<const std::byte>>(&value)) return *v;
        return std::nullopt;
    };
    const auto type = vi();
    if (!type || *type != 0x6) return false;
    const auto frame = take(2);
    if (!frame) return false;
    const std::size_t size = (std::to_integer<std::size_t>((*frame)[0]) << 8u) | std::to_integer<std::size_t>((*frame)[1]);
    const auto body = take(size);
    if (!body) return false;
    wire::Cursor inner(*body);
    const auto inner_vi = [&inner]() -> std::optional<std::uint64_t> {
        auto value = wire::read_vi64(inner);
        if (const auto* v = std::get_if<std::uint64_t>(&value)) return *v;
        return std::nullopt;
    };
    if (!inner_vi()) return false;  // Request ID
    const auto fields = inner_vi();
    if (!fields || *fields == 0 || *fields > 32) return false;
    std::optional<std::span<const std::byte>> first;
    for (std::uint64_t i = 0; i < *fields; ++i) {
        const auto length = inner_vi();
        if (!length || *length > inner.remaining()) return false;
        auto field = wire::read_bytes(inner, static_cast<std::size_t>(*length));
        const auto* value = std::get_if<std::span<const std::byte>>(&field);
        if (!value) return false;
        if (i == 0) first = *value;
    }
    const auto parameters = inner_vi();
    if (!parameters || *parameters != 0 || inner.remaining() != 0) return false;
    return !(first->size() == 1 && (*first)[0] == std::byte{'.'});
}
bool stream_less(RawProbeChannel channel) {
    return channel == RawProbeChannel::Datagram || channel == RawProbeChannel::Credit ||
           channel == RawProbeChannel::UniCredit || channel == RawProbeChannel::DropInbound ||
           channel == RawProbeChannel::ResumeInbound;
}
bool transport_step(RawProbeChannel channel) {
    return channel == RawProbeChannel::Credit || channel == RawProbeChannel::UniCredit ||
           channel == RawProbeChannel::DropInbound || channel == RawProbeChannel::ResumeInbound;
}
bool staged(const RawProbeDefinition& definition) {
    return std::any_of(definition.writes.begin(),definition.writes.end(),[](const auto& write) {
        return write.reuse_write_stream.has_value() || static_cast<bool>(write.peer_response_ready) ||
               static_cast<bool>(write.evidence_ready) || static_cast<bool>(write.prepare_bytes) ||
               static_cast<bool>(write.select_peer_stream);
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
        if (transport_step(write.channel)) {
            const bool counted = write.channel == RawProbeChannel::Credit ||
                                 write.channel == RawProbeChannel::UniCredit;
            if (!write.bytes.empty() || write.fin || write.prepare_bytes || write.reuse_write_stream ||
                write.operation != RawProbeOperation::Write ||
                (counted ? (write.application_error == 0 || write.application_error > (std::uint64_t{1} << 40))
                         : write.application_error != 0)) return false;
        }
        if (write.delay_after_previous.count() < 0 || write.delay_after_previous.count() > 600000) return false;
        if (write.select_peer_stream &&
            (write.operation != RawProbeOperation::StopSending || write.reuse_write_stream ||
             write.peer_response_ready || write.prepare_bytes || !definition.start_after_peer_setup))
            return false;
        if (write.operation == RawProbeOperation::StopSending &&
            ((!write.reuse_write_stream && !write.select_peer_stream) || !write.bytes.empty() || write.fin ||
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
                        const std::function<bool(const RawProbeGateInput&)>& ready,
                        std::string_view replacement_uri = {}) {
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
    return ready({prior_writes,events,replacement_uri}) ? GateState::Ready : GateState::Pending;
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
    return (stream_less(expected.channel) ? !observed.stream_id : observed.stream_id.has_value()) &&
           observed.write.delay_after_previous == expected.delay_after_previous &&
           observed.write.channel == expected.channel &&
           observed.write.bytes == expected.bytes && observed.write.fin == expected.fin &&
           observed.write.reuse_write_stream == expected.reuse_write_stream &&
           static_cast<bool>(observed.write.peer_response_ready) == static_cast<bool>(expected.peer_response_ready) &&
           observed.write.operation == expected.operation &&
           observed.write.application_error == expected.application_error &&
           static_cast<bool>(observed.write.evidence_ready) == static_cast<bool>(expected.evidence_ready) &&
           static_cast<bool>(observed.write.prepare_bytes) == static_cast<bool>(expected.prepare_bytes) &&
           static_cast<bool>(observed.write.select_peer_stream) == static_cast<bool>(expected.select_peer_stream) &&
           observed.operation_accepted == (expected.operation == RawProbeOperation::StopSending) &&
           observed.accepted == expected.bytes.size() &&
           observed.fin_accepted == expected.fin;
}
}
RawProbeController::RawProbeController(transport::SessionTransport& transport,
                                     RawProbeDefinition definition,
                                     transport::SessionTransport* replacement,
                                     std::string replacement_uri)
    : transport_(transport), replacement_(replacement), definition_(std::move(definition)) {
    if (definition_.offer_replacement_session != (replacement != nullptr && !replacement_uri.empty()) ||
        (replacement == nullptr) != replacement_uri.empty())
        throw std::invalid_argument("invalid raw probe replacement session");
    if (!replacement_uri.empty()) transcript_.replacement_uri = std::move(replacement_uri);
    if (definition_.id.empty() || definition_.setup_bytes.empty() ||
        definition_.deadline.count() <= 0 || !valid_stages(definition_) ||
        (definition_.start_after_peer_setup && !definition_.peer_setup_ready) ||
        (std::any_of(definition_.writes.begin(), definition_.writes.end(), [](const auto& write) {
            return write.channel == RawProbeChannel::PeerBidi;
        }) && !definition_.peer_request_ready) ||
        (definition_.acknowledge_publisher_namespace && definition_.acknowledge_publisher_namespace_draft21) ||
        ((definition_.acknowledge_publisher_namespace || definition_.acknowledge_publisher_namespace_draft21) &&
         !definition_.acknowledge_skips_peer_target &&
         std::any_of(definition_.writes.begin(), definition_.writes.end(), [](const auto& write) {
             return write.channel == RawProbeChannel::PeerBidi;
         })) ||
        // The courtesy answers on the same publisher-opened request streams a
        // PeerBidi write targets, so the two would interleave bytes on one stream.
        ((definition_.courtesy.publish != RawProbePublishResponse::Ignore ||
          definition_.courtesy.update != RawProbeUpdateResponse::Ignore) &&
         std::any_of(definition_.writes.begin(), definition_.writes.end(), [](const auto& write) {
             return write.channel == RawProbeChannel::PeerBidi;
         })))
        throw std::invalid_argument("invalid raw probe definition");
    if (definition_.liveness) {
        const auto& policy = *definition_.liveness;
        // An unbound policy (no request yet) is allowed: it simply never sends.
        if ((policy.draft != 18 && policy.draft != 21) || policy.delay.count() < 0 ||
            policy.delay.count() > 60000 || policy.grace.count() < 0 || policy.grace.count() > 60000 ||
            !liveness_definition_eligible(definition_) ||
            (!policy.request.empty() && !liveness_request_valid(policy)))
            throw std::invalid_argument("invalid raw probe liveness follow-up");
    }
    transcript_.scenario_id = definition_.id;
    const auto& courtesy = definition_.courtesy;
    if (courtesy.publish != RawProbePublishResponse::Ignore ||
        courtesy.update != RawProbeUpdateResponse::Ignore)
        courtesy_ = std::make_unique<PublisherCourtesy>(courtesy);
    transcript_.setup.write = {RawProbeChannel::NewUni, definition_.setup_bytes, false};
    for (const auto& write : definition_.writes)
        transcript_.writes.push_back({write, {}, 0, false});
}
RawProbeController::~RawProbeController() = default;
void RawProbeController::fail() {
    transcript_.harness_failed = true;
    transcript_.complete = false;
}
bool RawProbeController::flush(RawProbeAcceptedWrite& pending) {
    if (transport_step(pending.write.channel)) {
        if (pending.stream_id || !pending.write.bytes.empty() || pending.write.fin) { fail(); return false; }
        const auto channel = pending.write.channel;
        const auto result = channel == RawProbeChannel::Credit
            ? transport_.grant_peer_streams(true, pending.write.application_error)
            : channel == RawProbeChannel::UniCredit
                ? transport_.grant_peer_streams(false, pending.write.application_error)
                : transport_.set_inbound_drop(channel == RawProbeChannel::DropInbound);
        if (result.status == transport::TransportStatus::WouldBlock) return false;
        if (result.status != transport::TransportStatus::Success) { fail(); return false; }
        return true;
    }
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
void RawProbeController::send_auto_replies() {
    if (!definition_.auto_accept_ready || !transcript_.peer_setup_received) return;
    for (const auto& [id, candidate] : auto_accept_candidates_) {
        if (auto_accept_replied_.contains(id) || cancelled_peer_requests_.contains(id)) continue;
        if (peer_request_stream_ == id ||
            (definition_.peer_request_ready && !peer_request_stream_ &&
             definition_.peer_request_ready(candidate))) continue;
        if (!definition_.auto_accept_ready(candidate)) continue;
        if (auto_accept_replied_.size() >= definition_.auto_accept_limit) return;
        const auto result = transport_.write(id, definition_.auto_accept_reply, false);
        if (result.status == transport::TransportStatus::WouldBlock && result.accepted == 0) continue;
        if (result.status != transport::TransportStatus::Success ||
            result.accepted != definition_.auto_accept_reply.size()) { fail(); return; }
        auto_accept_replied_.insert(id);
        transcript_.auto_replies.push_back({id, transcript_.events.size()});
    }
}
bool RawProbeController::acknowledgement_excluded(transport::StreamId id, std::span<const std::byte> request) const {
    if (!definition_.acknowledge_skips_peer_target) return false;
    return peer_request_stream_ == id ||
           (definition_.peer_request_ready && definition_.peer_request_ready(request));
}
void RawProbeController::acknowledge_publisher_namespaces() {
    static const std::vector<std::byte> request_ok_18 = [] {
        wire::ByteWriter output(16);
        wire::draft18::encode_message(wire::draft18::RequestOkMessage{{}, {}}, output);
        return std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
    }();
    // Draft 21 Section 9.3: REQUEST_OK with no Parameters and no Track Properties.
    static const std::vector<std::byte> request_ok_21 = [] {
        wire::ByteWriter output(16);
        wire::draft21::encode_empty_publish_ok(output);
        return std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
    }();
    const auto& request_ok = definition_.acknowledge_publisher_namespace_draft21 ? request_ok_21 : request_ok_18;
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
        if (!transcript_.setup.delivery_event_count) {
            transcript_.setup.delivery_event_count = transcript_.events.size();
            transcript_.setup.accepted_at = now;
        }
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
        send_auto_replies();
        if (transcript_.harness_failed) return;
        while (next_write_ < transcript_.writes.size()) {
            auto& pending = transcript_.writes[next_write_];
            if (pending.write.delay_after_previous.count() > 0) {
                const auto& before = next_write_ == 0 ? transcript_.setup : transcript_.writes[next_write_ - 1];
                if (!before.accepted_at || now - *before.accepted_at < pending.write.delay_after_previous) return;
            }
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
                    transcript_.events,pending.write.evidence_ready,transcript_.replacement_uri.value_or(""));
                if (gate == GateState::LimitExceeded) fail();
                if (gate != GateState::Ready) return;
            }
            if (pending.write.select_peer_stream && !pending.prepared_event_count) {
                const auto prior = std::span<const RawProbeAcceptedWrite>(transcript_.writes).first(next_write_);
                if (evidence_gate(prior,transcript_.events,[](const auto&){return true;}) != GateState::Ready) {
                    fail(); return;
                }
                const auto selected = pending.write.select_peer_stream({prior,transcript_.events});
                if (!selected) return;
                if ((*selected & 3u) != 2u) { fail(); return; }
                pending.stream_id = *selected;
                pending.prepared_event_count = transcript_.events.size();
            }
            if (pending.write.prepare_bytes && pending.write.channel == RawProbeChannel::PeerBidi &&
                !pending.write.reuse_write_stream && !peer_request_stream_) return;
            if (pending.write.prepare_bytes && !pending.prepared_event_count) {
                const auto prior = std::span<const RawProbeAcceptedWrite>(transcript_.writes).first(next_write_);
                if (evidence_gate(prior,transcript_.events,[](const auto&){return true;}) != GateState::Ready) {
                    fail(); return;
                }
                auto bytes = pending.write.prepare_bytes({prior,transcript_.events,transcript_.replacement_uri.value_or("")});
                if (!bytes) return;
                if (bytes->empty() || bytes->size() > kMaximumSetupBytes) { fail(); return; }
                pending.write.bytes = std::move(*bytes);
                pending.prepared_event_count = transcript_.events.size();
            }
            if (!flush(pending)) return;
            pending.delivery_event_count = transcript_.events.size();
            pending.accepted_at = now;
            ++next_write_;
        }
        if (!transcript_.stimulus_delivered) {
            transcript_.stimulus_delivered = true;
            transcript_.delivery_event_count = transcript_.events.size();
            delivered_at_ = now;
        }
    };
    if (courtesy_) courtesy_->set_now(now);
    for (auto& event : session_closed_ ? std::vector<transport::TransportEvent>{} : transport_.poll(256)) {
        if (transcript_.events.size() >= kMaximumEvents) { fail(); break; }
        transcript_.events.push_back(std::move(event));
        transcript_.event_times.push_back(now);
        const auto& observed = transcript_.events.back();
        if (courtesy_) courtesy_->on_event(observed);
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
            if (definition_.auto_accept_ready && (data->stream_id & 3u) == 0u) {
                if (!transcript_.transport_established) { fail(); break; }
                if ((!auto_accept_candidates_.contains(data->stream_id) && auto_accept_candidates_.size() >= 64) ||
                    data->data.size() > kMaximumSetupBytes) { fail(); break; }
                auto& candidate = auto_accept_candidates_[data->stream_id];
                if (data->data.size() > kMaximumSetupBytes - candidate.size()) { fail(); break; }
                candidate.insert(candidate.end(), data->data.begin(), data->data.end());
            }
            if ((definition_.acknowledge_publisher_namespace || definition_.acknowledge_publisher_namespace_draft21) &&
                (data->stream_id & 3u) == 0u &&
                !acknowledged_.contains(data->stream_id) &&
                !acknowledgement_pending_.contains(data->stream_id) &&
                (acknowledgement_candidates_.contains(data->stream_id) || acknowledgement_candidates_.size() < 64)) {
                auto& candidate = acknowledgement_candidates_[data->stream_id];
                if (candidate.size() + data->data.size() <= kMaximumSetupBytes) {
                    candidate.insert(candidate.end(), data->data.begin(), data->data.end());
                    if (definition_.acknowledge_publisher_namespace_draft21) {
                        if (answerable_draft21_announcement(candidate) && !acknowledgement_excluded(data->stream_id, candidate))
                            acknowledgement_pending_.insert(data->stream_id);
                    } else {
                    wire::Cursor cursor(candidate);
                    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request, cursor, {});
                    const auto* message = std::get_if<wire::draft18::Message>(&decoded);
                    const auto* announce = message ? std::get_if<wire::draft18::PublishNamespaceMessage>(message) : nullptr;
                    if (announce && announce->parameters.empty() && !announce->track_namespace.fields.empty() &&
                        !(announce->track_namespace.fields.front().size() == 1 &&
                          announce->track_namespace.fields.front().front() == std::byte{'.'}) &&
                        !acknowledgement_excluded(data->stream_id, candidate))
                        acknowledgement_pending_.insert(data->stream_id);
                    }
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
            // A publisher that migrates may close the old session first; the
            // replacement session is still awaited until it settles or times out.
            if (replacement_ && transcript_.complete && !transcript_.timed_out) {
                transcript_.complete = false;
                session_closed_ = true;
                break;
            }
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
    if (replacement_ && !transcript_.harness_failed) {
        for (auto& event : replacement_->poll(256)) {
            if (transcript_.replacement_events.size() >= kMaximumEvents) { fail(); break; }
            transcript_.replacement_events.push_back(std::move(event));
            // The replacement session gets the same SETUP as the first one.
            if (!replacement_setup_sent_ &&
                std::holds_alternative<transport::ConnectionEstablishedEvent>(transcript_.replacement_events.back())) {
                replacement_setup_sent_ = true;
                const auto opened = replacement_->open_uni();
                if (opened.status == transport::TransportStatus::Success)
                    replacement_->write(opened.stream_id, definition_.setup_bytes, false);
            }
        }
    }
    // All events in this batch were already observed before any new writes.
    if (!transcript_.harness_failed && !session_closed_ && transcript_.transport_established)
        acknowledge_publisher_namespaces();
    if (courtesy_ && !transcript_.harness_failed && !session_closed_ && transcript_.transport_established)
        for (const auto& write : courtesy_->step(transport_, now, transcript_.events.size()))
            transcript_.courtesy_writes.push_back(write);
    if (!transcript_.harness_failed && !session_closed_) send();
    step_liveness(now);
    if (transcript_.stimulus_delivered && !transcript_.harness_failed &&
        definition_.response_ready && definition_.response_ready(transcript_)) {
        transcript_.complete = true;
        return transcript_;
    }
    const auto timeout_start = delivered_at_.value_or(*started_at_);
    // A follow-up needs its delay and grace on top of the time to react.
    const auto follow_up = definition_.liveness && !definition_.liveness->request.empty()
        ? definition_.liveness->delay + definition_.liveness->grace : std::chrono::milliseconds{0};
    if (!transcript_.complete && now - timeout_start >= definition_.deadline + follow_up)
        transcript_.timed_out = true;
    return transcript_;
}
const RawProbeTranscript& RawProbeController::transcript() const noexcept { return transcript_; }

namespace {
// Each automatic reply answers a peer-opened bidirectional stream whose bytes,
// as received before the reply, satisfied the definition's predicate.
bool auto_replies_valid(const RawProbeTranscript& transcript, const RawProbeDefinition& definition) {
    if (!definition.auto_accept_ready) return transcript.auto_replies.empty();
    if (transcript.auto_replies.size() > definition.auto_accept_limit) return false;
    std::set<transport::StreamId> seen;
    std::size_t previous_marker = 0;
    for (const auto& reply : transcript.auto_replies) {
        if ((reply.stream_id & 3u) != 0u || !seen.insert(reply.stream_id).second ||
            reply.delivery_event_count > transcript.events.size() ||
            reply.delivery_event_count < previous_marker) return false;
        previous_marker = reply.delivery_event_count;
        std::vector<std::byte> request;
        bool peer_setup = false;
        for (std::size_t i = 0; i < reply.delivery_event_count; ++i) {
            const auto* data = std::get_if<transport::StreamDataEvent>(&transcript.events[i]);
            if (data && (data->stream_id & 3u) == 2u) peer_setup = true;
            if (data && data->stream_id == reply.stream_id) {
                if (data->data.size() > kMaximumSetupBytes - request.size()) return false;
                request.insert(request.end(), data->data.begin(), data->data.end());
            }
            if (const auto* reset = std::get_if<transport::PeerResetEvent>(&transcript.events[i]);
                reset && reset->stream_id == reply.stream_id) return false;
        }
        if (!peer_setup || !definition.auto_accept_ready(request)) return false;
    }
    return true;
}
}  // namespace

namespace {
// A parameter-free PUBLISH_NAMESPACE for "media", the shape the default answer acts on.
std::vector<std::byte> sample_announcement(unsigned draft) {
    if (draft == 21) {
        static const unsigned char frame[] = {0x06, 0x00, 0x09, 0x00, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a', 0x00};
        std::vector<std::byte> result;
        for (const auto byte : frame) result.push_back(static_cast<std::byte>(byte));
        return result;
    }
    wire::ByteWriter output(64);
    wire::draft18::encode_message(wire::draft18::PublishNamespaceMessage{
        0, wire::draft18::TrackNamespace{{{std::byte{'m'}, std::byte{'e'}, std::byte{'d'}, std::byte{'i'}, std::byte{'a'}}}}, {}}, output);
    return std::vector<std::byte>(output.bytes().begin(), output.bytes().end());
}
}  // namespace

std::span<const NamespaceAnswerOptOut> namespace_answer_opt_outs() {
    static const std::vector<NamespaceAnswerOptOut> table = {
        // Empty on purpose. Every scenario whose subject is the publisher's reaction
        // to a response the runner writes on the publisher's request stream (rejection,
        // redirect, malformed or unknown response; PeerBidi writes or peer_request_ready)
        // is skipped by apply_default_namespace_answer itself, and the credit-exhaustion
        // probes that spend the only bidirectional stream on the announcement already
        // carry their own acknowledgement. An entry here is for a probe that sets
        // neither but whose announcement must stay unanswered.
    };
    return table;
}

DefaultNamespaceAnswer apply_default_namespace_answer(RawProbeDefinition& definition, unsigned draft) {
    if (draft != 18 && draft != 21) return DefaultNamespaceAnswer::UnsupportedDraft;
    if (definition.no_default_namespace_answer ||
        std::any_of(namespace_answer_opt_outs().begin(), namespace_answer_opt_outs().end(),
                    [&](const auto& entry) { return entry.draft == draft && entry.scenario == definition.id; }))
        return DefaultNamespaceAnswer::OptedOut;
    if (definition.acknowledge_publisher_namespace || definition.acknowledge_publisher_namespace_draft21 ||
        definition.auto_accept_ready)
        return DefaultNamespaceAnswer::OwnMechanism;
    const bool peer_writes = std::any_of(definition.writes.begin(), definition.writes.end(),
        [](const auto& write) { return write.channel == RawProbeChannel::PeerBidi; });
    if (peer_writes || definition.peer_request_ready) {
        // Safe only when the stimulus targets some other publisher request (a PUBLISH, a
        // TRACK_STATUS): then an announcement is answered beside it, never on its stream.
        if (!definition.peer_request_ready || definition.peer_request_ready(sample_announcement(draft)))
            return DefaultNamespaceAnswer::TargetsRequest;
        definition.acknowledge_skips_peer_target = true;
    }
    (draft == 18 ? definition.acknowledge_publisher_namespace
                 : definition.acknowledge_publisher_namespace_draft21) = true;
    return DefaultNamespaceAnswer::Applied;
}

bool raw_probe_stimulus_valid(const RawProbeTranscript& transcript,
                             const RawProbeDefinition& definition) {
    if (!valid_stages(definition) || transcript.events.size() > kMaximumEvents ||
        transcript.scenario_id != definition.id || !transcript.complete ||
        transcript.replacement_uri.has_value() != definition.offer_replacement_session ||
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
        } else if (expected.select_peer_stream) {
            if (!write.prepared_event_count || !transcript.setup.delivery_event_count ||
                *transcript.setup.delivery_event_count > gate_marker ||
                gate_marker < previous_marker || gate_marker > marker ||
                marker > *transcript.delivery_event_count) return false;
            const auto prior = std::span<const RawProbeAcceptedWrite>(transcript.writes).first(i);
            const auto events = std::span<const transport::TransportEvent>(transcript.events).first(gate_marker);
            if (evidence_gate(prior,events,[](const auto&){return true;}) != GateState::Ready ||
                !setup_before(transcript,definition,gate_marker)) return false;
            const auto selected = expected.select_peer_stream({prior,events});
            if (!selected || write.stream_id != selected || (*selected & 3u) != 2u) return false;
        } else if (write.prepared_event_count) return false;
        if (!expected.prepare_bytes && !accepted(write, expected)) return false;
        if (marked && !write.delivery_event_count) return false;
        if (definition.writes[i].delay_after_previous.count() > 0) {
            const auto& before = i == 0 ? transcript.setup : transcript.writes[i - 1];
            if (!before.accepted_at || !write.accepted_at ||
                *write.accepted_at - *before.accepted_at < definition.writes[i].delay_after_previous) return false;
        }
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
        } else if (transport_step(write.write.channel)) {
            if (write.stream_id || write.write.fin || !write.write.bytes.empty()) return false;
        } else if (expected.select_peer_stream) {
            // The stream was selected and validated from the events above.
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
                definition.writes[i].evidence_ready, transcript.replacement_uri.value_or("")) != GateState::Ready) return false;
        if (expected.prepare_bytes) {
            const auto prior = std::span<const RawProbeAcceptedWrite>(transcript.writes).first(i);
            const auto events = std::span<const transport::TransportEvent>(transcript.events).first(gate_marker);
            const auto bytes = expected.prepare_bytes({prior,events,transcript.replacement_uri.value_or("")});
            if (!bytes || bytes->empty() || bytes->size() > kMaximumSetupBytes) return false;
            expected.bytes = *bytes;
            if (!accepted(write,expected)) return false;
        }
    }
    if (marked && !transcript.writes.empty() && previous_marker != *transcript.delivery_event_count) return false;
    if (!auto_replies_valid(transcript, definition)) return false;
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

bool raw_probe_liveness_proven(const RawProbeTranscript& transcript,
                              const RawProbeDefinition& definition) {
    if (!definition.liveness || !transcript.liveness || !liveness_definition_eligible(definition) ||
        !raw_probe_stimulus_valid(transcript, definition)) return false;
    const auto& policy = *definition.liveness;
    const auto& record = *transcript.liveness;
    const auto& write = record.write;
    // The follow-up is the policy's own request, exactly as a SUBSCRIBE for some track.
    RawProbeLiveness sent = policy;
    sent.request = write.write.bytes;
    if (!liveness_request_valid(sent) || (!policy.request.empty() && policy.request != write.write.bytes) ||
        write.write.channel != RawProbeChannel::NewBidi || write.write.fin ||
        write.write.operation != RawProbeOperation::Write || write.write.reuse_write_stream ||
        write.write.peer_response_ready || write.write.evidence_ready || write.write.prepare_bytes ||
        write.write.select_peer_stream || write.write.delay_after_previous.count() != 0 ||
        !write.stream_id || (*write.stream_id & 3u) != 1u || write.accepted != write.write.bytes.size() ||
        write.fin_accepted || write.operation_accepted || write.prepared_event_count ||
        !write.delivery_event_count || !write.accepted_at || !record.anchor_at ||
        !record.answered_at || !record.settled_at) return false;
    // A fresh stream, used for nothing else.
    if (*write.stream_id == transcript.setup.stream_id) return false;
    for (const auto& earlier : transcript.writes)
        if (earlier.stream_id == write.stream_id) return false;
    const auto stimulus_end = *transcript.delivery_event_count;
    if (record.anchor_event_count < stimulus_end || record.anchor_event_count > transcript.events.size() ||
        *write.delivery_event_count < record.anchor_event_count ||
        *write.delivery_event_count > transcript.events.size()) return false;
    // The publisher's SETUP had arrived when the delay started.
    bool connection = false;
    bool setup = false;
    std::map<transport::StreamId, std::vector<std::byte>> candidates;
    for (std::size_t i = 0; i < record.anchor_event_count && !setup; ++i) {
        const auto& event = transcript.events[i];
        if (std::holds_alternative<transport::ConnectionEstablishedEvent>(event)) connection = true;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && connection && (data->stream_id & 3u) == 2u) {
            auto& candidate = candidates[data->stream_id];
            candidate.insert(candidate.end(), data->data.begin(), data->data.end());
            setup = definition.peer_setup_ready && definition.peer_setup_ready(candidate);
        }
    }
    if (!setup) return false;
    // Delay measured from the later of the stimulus and the SETUP; grace after the answer.
    auto last_stimulus = transcript.setup.accepted_at;
    for (const auto& earlier : transcript.writes)
        if (earlier.accepted_at && (!last_stimulus || *earlier.accepted_at > *last_stimulus))
            last_stimulus = earlier.accepted_at;
    if (!last_stimulus || *record.anchor_at < *last_stimulus ||
        *write.accepted_at - *record.anchor_at < policy.delay ||
        *record.answered_at < *write.accepted_at ||
        *record.settled_at - *record.answered_at < policy.grace) return false;
    // No close of any kind, and a SUBSCRIBE_OK on the follow-up stream.
    for (const auto& event : transcript.events)
        if (std::holds_alternative<transport::PeerCloseEvent>(event)) return false;
    return liveness_answer(transcript, policy.draft) == LivenessAnswer::Serving;
}

namespace {
// How long after the last stimulus write a publisher's close is still read as its reaction.
// Closes the publisher makes on its own schedule (an idle or read timeout, a process deadline)
// arrive later and say nothing about the input, so they are left unscored.
constexpr std::chrono::milliseconds kReactionWindow{1500};

struct CloseObservation {
    enum class Reading { None, Reaction, Unattributable } reading{Reading::None};
    const transport::PeerCloseEvent* close{nullptr};
};

CloseObservation observe_close(const RawProbeTranscript& transcript, const RawProbeDefinition& definition) {
    for (std::size_t index = 0; index < transcript.events.size(); ++index) {
        const auto* close = std::get_if<transport::PeerCloseEvent>(&transcript.events[index]);
        if (!close) continue;
        using Reading = CloseObservation::Reading;
        if (close->error_space != transport::CloseErrorSpace::Application) return {Reading::Unattributable, close};
        // A close that precedes the point where the stimulus was fully accepted cannot be a
        // reaction to it.
        if (!transcript.delivery_event_count || index < *transcript.delivery_event_count)
            return {Reading::Unattributable, close};
        if (transcript.event_times.size() == transcript.events.size()) {
            auto last_stimulus = transcript.setup.accepted_at;
            for (const auto& write : transcript.writes)
                if (write.accepted_at && (!last_stimulus || *write.accepted_at > *last_stimulus))
                    last_stimulus = write.accepted_at;
            auto window = kReactionWindow;
            if (definition.liveness) window += definition.liveness->delay + definition.liveness->grace;
            if (last_stimulus && transcript.event_times[index] - *last_stimulus > window)
                return {Reading::Unattributable, close};
        }
        return {Reading::Reaction, close};
    }
    return {};
}
}  // namespace

std::optional<transport::PeerCloseEvent> observe_raw_probe_close(
    const RawProbeTranscript& transcript, const RawProbeDefinition& definition) {
    const auto observed = observe_close(transcript, definition);
    if (observed.reading != CloseObservation::Reading::Reaction) return std::nullopt;
    return *observed.close;
}

std::optional<bool> evaluate_raw_probe_close(const RawProbeTranscript& transcript,
                                           const RawProbeDefinition& definition,
                                           std::optional<std::uint64_t> expected_close) {
    if (!raw_probe_stimulus_valid(transcript, definition)) return std::nullopt;
    const auto observed = observe_close(transcript, definition);
    using Reading = CloseObservation::Reading;
    if (observed.reading == Reading::Unattributable) return std::nullopt;
    if (observed.reading == Reading::Reaction) {
        // When any code satisfies the rule, a NO_ERROR close is not a reaction to a violation:
        // it is how a publisher ends a session it is finished with, or gives up waiting.
        if (!expected_close && observed.close->error_code == 0) return std::nullopt;
        return !expected_close || observed.close->error_code == *expected_close;
    }
    // No close. The publisher stayed silent (unscored) unless it demonstrably kept
    // serving a request made after input that required it to close.
    if (raw_probe_liveness_proven(transcript, definition)) return false;
    return std::nullopt;
}
}  // namespace moq::interop::scenarios
