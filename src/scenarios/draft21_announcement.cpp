#include "moq/interop/scenarios/draft21_announcement.h"

#include "moq/interop/wire/draft21/request_ok.h"
#include "moq/interop/wire/draft21/setup.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace moq::interop::scenarios {
namespace {

constexpr std::size_t kMaximumEvidence = 4096;
constexpr std::size_t kMaximumUniProbes = 64;
constexpr std::size_t kMaximumUniProbeBytes = 9;
constexpr std::size_t kMaximumIgnoredUniStreams = 64;

bool known_non_control_uni_type(std::uint64_t type) {
    // draft-ietf-moq-transport-21 section 6.4.1, Table 2.
    return type == 0x05 || type == 0x132b3e28 ||
           (type <= 0x3f && (type & 0x10) != 0);
}

}  // namespace

Draft21AnnouncementController::Draft21AnnouncementController(
    transport::SessionTransport& transport,
    std::vector<std::vector<std::byte>> expected_namespace,
    std::vector<std::byte> expected_track_name,
    std::chrono::milliseconds timeout,
    Draft21SetupProbe setup_probe)
    : transport_(transport),
      expected_namespace_(std::move(expected_namespace)),
      expected_track_name_(std::move(expected_track_name)),
      timeout_(timeout) {
    context_.setup_probe = setup_probe;
}

void Draft21AnnouncementController::record(
    Draft21AnnouncementEventKind kind,
    std::optional<transport::StreamId> stream_id,
    std::optional<std::uint64_t> request_id) {
    if (context_.evidence.size() >= kMaximumEvidence) {
        fail_harness();
        return;
    }
    context_.evidence.push_back({kind, stream_id, request_id});
}

void Draft21AnnouncementController::fail_harness() {
    harness_failed_ = true;
    status_ = Draft21AnnouncementStatus::Failed;
}

void Draft21AnnouncementController::close_protocol(
    std::uint64_t error, std::optional<transport::StreamId> stream_id) {
    record(Draft21AnnouncementEventKind::ProtocolViolation, stream_id);
    const auto closed = transport_.close(error, {});
    if (closed.status != transport::TransportStatus::Success &&
        closed.status != transport::TransportStatus::ConnectionClosed) {
        fail_harness();
    }
    status_ = Draft21AnnouncementStatus::Failed;
}

void Draft21AnnouncementController::handle_control(
    transport::StreamId stream_id, std::span<const std::byte> data, bool fin) {
    const auto result = control_.on_peer_data(stream_id, data, fin);
    if (result.harness_limit) {
        fail_harness();
        return;
    }
    if (result.close_error) {
        close_protocol(*result.close_error, stream_id);
        return;
    }
    for (const auto& message : result.messages) {
        if (std::holds_alternative<wire::draft21::SetupMessage>(message)) {
            record(Draft21AnnouncementEventKind::PeerSetupReceived, stream_id);
        }
    }
}

void Draft21AnnouncementController::handle_uni(
    const transport::StreamDataEvent& event) {
    if (peer_control_stream_ && *peer_control_stream_ == event.stream_id) {
        handle_control(event.stream_id, event.data, event.fin);
        return;
    }
    if (std::find(ignored_uni_streams_.begin(), ignored_uni_streams_.end(),
                  event.stream_id) != ignored_uni_streams_.end()) {
        return;
    }
    auto found = uni_probes_.find(event.stream_id);
    if (found == uni_probes_.end()) {
        if (uni_probes_.size() >= kMaximumUniProbes) {
            fail_harness();
            return;
        }
        found = uni_probes_.emplace(event.stream_id,
                                    std::vector<std::byte>{}).first;
    }
    auto& probe = found->second;
    const auto ignore_stream = [&] {
        uni_probes_.erase(found);
        if (ignored_uni_streams_.size() >= kMaximumIgnoredUniStreams) {
            fail_harness();
            return;
        }
        ignored_uni_streams_.push_back(event.stream_id);
        record(Draft21AnnouncementEventKind::UnsupportedStream,
               event.stream_id);
    };
    if (event.data.size() > kMaximumUniProbeBytes - probe.size()) {
        // A complete first chunk may contain an entire SETUP; inspect its
        // type without retaining the payload in the bounded probe.
        std::vector<std::byte> first(probe);
        first.insert(first.end(), event.data.begin(), event.data.end());
        wire::Cursor cursor(first);
        const auto type = wire::read_vi64(cursor);
        if (const auto* value = std::get_if<std::uint64_t>(&type);
            value && *value == 0x2f00) {
            peer_control_stream_ = event.stream_id;
            uni_probes_.erase(found);
            handle_control(event.stream_id, first, event.fin);
            return;
        }
        if (const auto* value = std::get_if<std::uint64_t>(&type);
            value && known_non_control_uni_type(*value)) {
            ignore_stream();
        } else {
            uni_probes_.erase(found);
            close_protocol(0x3, event.stream_id);
        }
        return;
    }
    probe.insert(probe.end(), event.data.begin(), event.data.end());
    wire::Cursor cursor(probe);
    const auto type = wire::read_vi64(cursor);
    if (std::holds_alternative<wire::NeedMore>(type) && !event.fin) return;
    if (const auto* value = std::get_if<std::uint64_t>(&type);
        value && *value == 0x2f00) {
        peer_control_stream_ = event.stream_id;
        auto first = std::move(probe);
        uni_probes_.erase(found);
        handle_control(event.stream_id, first, event.fin);
        return;
    }
    if (const auto* value = std::get_if<std::uint64_t>(&type);
        value && known_non_control_uni_type(*value)) {
        ignore_stream();
    } else {
        uni_probes_.erase(found);
        close_protocol(0x3, event.stream_id);
    }
}

void Draft21AnnouncementController::handle_request(
    const transport::StreamDataEvent& event) {
    const auto result = publish_open_.on_client_stream(
        event.stream_id, event.data, event.fin);
    if (result.harness_limit) {
        fail_harness();
        return;
    }
    if (result.close_error) {
        if (result.invalid_first_message) {
            record(Draft21AnnouncementEventKind::InvalidRequestOpener,
                   event.stream_id);
        }
        close_protocol(*result.close_error, event.stream_id);
        return;
    }
    if (result.unsupported_message || result.unsupported_followup) {
        record(Draft21AnnouncementEventKind::UnsupportedStream,
               event.stream_id);
    }
    if (!result.publish) return;
    const bool target = result.publish->track_namespace == expected_namespace_ &&
                        result.publish->track_name == expected_track_name_;
    context_.target_publish_seen |= target;
    record(Draft21AnnouncementEventKind::PublishObserved,
           event.stream_id, result.publish->request_id);
    pending_publications_.push_back(
        {event.stream_id, result.publish->request_id, target});
}

void Draft21AnnouncementController::handle_event(
    const transport::TransportEvent& event) {
    if (const auto* connection =
            std::get_if<transport::ConnectionEstablishedEvent>(&event)) {
        const auto error = control_.on_transport_established(connection->alpn);
        if (error) {
            close_protocol(*error, std::nullopt);
            return;
        }
        transport_established_ = true;
        record(Draft21AnnouncementEventKind::TransportEstablished);
        return;
    }
    if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
        if ((data->stream_id & 3u) == 2u) handle_uni(*data);
        else if ((data->stream_id & 3u) == 0u) handle_request(*data);
        return;
    }
    if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
        if (peer_control_stream_ && *peer_control_stream_ == reset->stream_id) {
            close_protocol(0x3, reset->stream_id);
        }
        return;
    }
    if (std::holds_alternative<transport::PeerCloseEvent>(event) ||
        std::holds_alternative<transport::LocalCloseEvent>(event) ||
        std::holds_alternative<transport::IdleTimeoutEvent>(event)) {
        record(Draft21AnnouncementEventKind::PeerClosed);
        status_ = Draft21AnnouncementStatus::TimedOut;
        return;
    }
    if (std::holds_alternative<transport::TransportErrorEvent>(event) ||
        std::holds_alternative<transport::EventQueueOverflowEvent>(event)) {
        fail_harness();
    }
}

void Draft21AnnouncementController::open_local_setup() {
    if (!transport_established_ || local_setup_opened_) return;
    const auto opened = transport_.open_uni();
    if (opened.status == transport::TransportStatus::StreamLimit ||
        opened.status == transport::TransportStatus::WouldBlock) return;
    if (opened.status != transport::TransportStatus::Success ||
        (opened.stream_id & 3u) != 3u) {
        fail_harness();
        return;
    }
    wire::draft21::SetupMessage setup;
    if (context_.setup_probe != Draft21SetupProbe::None) {
        // Section 13 reserves 0x9d as a GREASE Setup Option.  It is an
        // odd (byte-valued) option in the version-independent namespace.
        setup.options.push_back({0x9d, std::vector<std::byte>{std::byte{0xaa}}});
        if (context_.setup_probe == Draft21SetupProbe::DuplicateUnknownOption) {
            setup.options.push_back(
                {0x9d, std::vector<std::byte>{std::byte{0xbb}}});
        }
    }
    wire::ByteWriter output(65'546);
    if (wire::draft21::encode_setup(setup, output)) {
        fail_harness();
        return;
    }
    writes_.push_back({opened.stream_id,
                       {output.bytes().begin(), output.bytes().end()},
                       0, true, false, 0});
    local_setup_opened_ = true;
}

void Draft21AnnouncementController::queue_pending_responses() {
    if (control_.phase() != session::draft21::ControlPhase::Active) return;
    for (const auto& publish : pending_publications_) {
        wire::ByteWriter output(16);
        if (!wire::draft21::encode_empty_publish_ok(output)) {
            fail_harness();
            return;
        }
        writes_.push_back({publish.stream_id,
                           {output.bytes().begin(), output.bytes().end()},
                           0, false, publish.target, publish.request_id});
    }
    pending_publications_.clear();
}

void Draft21AnnouncementController::flush_writes() {
    while (!writes_.empty() && !harness_failed_) {
        auto& front = writes_.front();
        const auto remaining = std::span<const std::byte>(front.bytes).subspan(
            front.offset);
        const auto written = transport_.write(front.stream_id, remaining, false);
        if (written.status == transport::TransportStatus::WouldBlock ||
            written.status == transport::TransportStatus::StreamLimit) return;
        if ((written.status != transport::TransportStatus::Success &&
             written.status != transport::TransportStatus::Partial) ||
            written.accepted == 0 || written.accepted > remaining.size()) {
            fail_harness();
            return;
        }
        front.offset += written.accepted;
        if (front.offset < front.bytes.size()) return;
        if (front.setup) {
            control_.on_local_setup_sent();
            record(Draft21AnnouncementEventKind::LocalSetupSent,
                   front.stream_id);
        } else {
            record(Draft21AnnouncementEventKind::ResponseDelivered,
                   front.stream_id, front.request_id);
            context_.response_delivered |= front.target_response;
        }
        writes_.pop_front();
    }
}

Draft21AnnouncementSnapshot Draft21AnnouncementController::poll(
    Draft21Clock::time_point now) {
    if (status_ != Draft21AnnouncementStatus::Running) {
        return {status_, harness_failed_};
    }
    if (!started_) started_ = now;
    for (const auto& event : transport_.poll(256)) {
        handle_event(event);
        if (status_ != Draft21AnnouncementStatus::Running || harness_failed_) break;
    }
    if (!harness_failed_ && status_ == Draft21AnnouncementStatus::Running) {
        open_local_setup();
        flush_writes();
        queue_pending_responses();
        flush_writes();
        if (context_.target_publish_seen && context_.response_delivered &&
            control_.phase() == session::draft21::ControlPhase::Active) {
            context_.complete = true;
            status_ = Draft21AnnouncementStatus::Passed;
        } else if (now - *started_ >= timeout_) {
            status_ = Draft21AnnouncementStatus::TimedOut;
        }
    }
    return {status_, harness_failed_};
}

const Draft21AnnouncementContext&
Draft21AnnouncementController::context() const noexcept {
    return context_;
}

}  // namespace moq::interop::scenarios
