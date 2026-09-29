#include "moq/interop/scenarios/action_dispatcher.h"

#include "moq/interop/wire/cursor.h"

#include <span>
#include <type_traits>
#include <utility>

namespace moq::interop::scenarios {
namespace {

constexpr std::size_t kMaximumDraft18MessageFrame = 65'546;

std::optional<std::uint64_t> opening_request_id(
    const wire::draft18::Message& message) {
    return std::visit([](const auto& value)
                          -> std::optional<std::uint64_t> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (
            std::is_same_v<T, wire::draft18::SubscribeMessage> ||
            std::is_same_v<T, wire::draft18::PublishMessage> ||
            std::is_same_v<T, wire::draft18::FetchMessage> ||
            std::is_same_v<T, wire::draft18::TrackStatusMessage> ||
            std::is_same_v<T, wire::draft18::PublishNamespaceMessage> ||
            std::is_same_v<T, wire::draft18::SubscribeNamespaceMessage> ||
            std::is_same_v<T, wire::draft18::SubscribeTracksMessage>) {
            return value.request_id;
        }
        return std::nullopt;
    }, message);
}

}  // namespace

ActionDispatcher::ActionDispatcher(transport::SessionTransport& transport,
                                   session::PublisherSession& session)
    : transport_(transport), session_(session) {}

DispatchResult ActionDispatcher::submit(const OpenRequestAction& action) {
    if (pending_) {
        return {DispatchState::Failed, false, std::nullopt,
                transport::TransportStatus::InvalidState};
    }
    const auto request_id = opening_request_id(action.message);
    if (!request_id || *request_id != next_request_id_) {
        return {DispatchState::Failed, false, std::nullopt,
                transport::TransportStatus::InvalidState};
    }
    wire::ByteWriter writer(kMaximumDraft18MessageFrame);
    if (!wire::draft18::encode_message(action.message, writer).has_value()) {
        return {DispatchState::Failed, false, std::nullopt,
                transport::TransportStatus::InvalidState};
    }
    next_request_id_ += 2;
    pending_ = PendingMessage{action.message,
                              session::LocalStreamPurpose::Request,
                              action.fin,
                              {writer.bytes().begin(), writer.bytes().end()},
                              std::nullopt, 0, false};
    return flush();
}

DispatchResult ActionDispatcher::submit_setup() {
    if (pending_ || setup_submitted_ ||
        session_.phase() == session::SessionPhase::Active ||
        session_.phase() == session::SessionPhase::Closing ||
        session_.phase() == session::SessionPhase::Closed) {
        return {DispatchState::Failed, false, std::nullopt,
                transport::TransportStatus::InvalidState};
    }
    wire::draft18::Message message = wire::draft18::SetupMessage{};
    wire::ByteWriter writer(kMaximumDraft18MessageFrame);
    if (!wire::draft18::encode_message(message, writer).has_value()) {
        return {DispatchState::Failed, false, std::nullopt,
                transport::TransportStatus::InternalError};
    }
    setup_submitted_ = true;
    pending_ = PendingMessage{std::move(message),
                              session::LocalStreamPurpose::Control,
                              false,
                              {writer.bytes().begin(), writer.bytes().end()},
                              std::nullopt, 0, false};
    return flush();
}

DispatchResult ActionDispatcher::flush() {
    if (!pending_) {
        return {DispatchState::Complete, false, std::nullopt, std::nullopt};
    }
    const auto phase = session_.phase();
    if (phase == session::SessionPhase::Closing ||
        phase == session::SessionPhase::Closed) {
        const auto stream_id = pending_->stream_id;
        pending_.reset();
        return {DispatchState::Failed, false, stream_id,
                transport::TransportStatus::ConnectionClosed};
    }
    auto& pending = *pending_;
    if (!pending.stream_id) {
        if (phase != (pending.purpose ==
                              session::LocalStreamPurpose::Control
                          ? session::SessionPhase::AwaitingSetup
                          : session::SessionPhase::Active)) {
            return {DispatchState::Pending, false, std::nullopt,
                    std::nullopt};
        }
        const auto opened = pending.purpose ==
                                    session::LocalStreamPurpose::Control
                                ? transport_.open_uni()
                                : transport_.open_bidi();
        if (opened.status != transport::TransportStatus::Success) {
            if (opened.status == transport::TransportStatus::StreamLimit ||
                opened.status == transport::TransportStatus::WouldBlock) {
                return {DispatchState::Pending, false, std::nullopt,
                        opened.status};
            }
            pending_.reset();
            return {DispatchState::Failed, false, std::nullopt,
                    opened.status};
        }
        pending.stream_id = opened.stream_id;
        session_.observe_local_stream(opened.stream_id, pending.purpose);
    }

    const auto stream_id = *pending.stream_id;
    if (pending.accepted < pending.frame.size()) {
        const auto remaining = std::span<const std::byte>(pending.frame)
                                   .subspan(pending.accepted);
        const auto result = transport_.write(stream_id, remaining, false);
        if (result.status == transport::TransportStatus::WouldBlock &&
            result.accepted == 0) {
            return {DispatchState::Pending, false, stream_id, result.status};
        }
        if (result.status != transport::TransportStatus::Success &&
            result.status != transport::TransportStatus::Partial) {
            pending_.reset();
            return {DispatchState::Failed, false, stream_id, result.status};
        }
        if (result.accepted > remaining.size() ||
            (result.status == transport::TransportStatus::Success &&
             result.accepted != remaining.size())) {
            pending_.reset();
            return {DispatchState::Failed, false, stream_id,
                    transport::TransportStatus::InternalError};
        }
        pending.accepted += result.accepted;
        if (pending.accepted < pending.frame.size()) {
            return {DispatchState::Pending, false, stream_id,
                    transport::TransportStatus::WouldBlock};
        }
    }

    if (pending.fin && !pending.fin_sent) {
        const auto result = transport_.write(stream_id, {}, true);
        if (result.status != transport::TransportStatus::Success ||
            result.accepted != 0) {
            pending_.reset();
            return {DispatchState::Failed, false, stream_id, result.status};
        }
        pending.fin_sent = true;
    }
    session_.observe_local_message(stream_id, pending.message, pending.fin);
    pending_.reset();
    return {DispatchState::Complete, true, stream_id, std::nullopt};
}

bool ActionDispatcher::has_pending() const noexcept {
    return pending_.has_value();
}

}  // namespace moq::interop::scenarios
