#include "moq/interop/session/lite_session.h"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace moq::interop::session {
namespace {

// RFC 9000 section 2.1: bit 0 set = server-initiated (the runner is the server), bit 1 set = unidirectional.
LiteOrigin origin_of(std::uint64_t stream_id) {
    return (stream_id & 0x1) != 0 ? LiteOrigin::Runner : LiteOrigin::Peer;
}

bool bidirectional_of(std::uint64_t stream_id) { return (stream_id & 0x2) == 0; }

}  // namespace

LiteSession::LiteSession(LiteSessionLimits limits) : limits_(limits) {
    budget_.bytes_left = limits_.max_bytes;
    budget_.messages_left = limits_.max_messages_total;
}

void LiteSession::on_event(const transport::TransportEvent& event, std::uint64_t at_ns) {
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, transport::StreamDataEvent>) {
                if (limit_reached_ || !admit_bytes(value.data.size())) return;
                const auto index = slot(value.stream_id, bidirectional_of(value.stream_id));
                if (!index) return;
                budget_.bytes_left -= value.data.size();
                total_bytes_ += value.data.size();
                decoders_[*index].feed(records_[*index], value.data, value.fin, at_ns, &budget_);
                after_update(*index);
            } else if constexpr (std::is_same_v<T, transport::PeerResetEvent>) {
                if (limit_reached_) return;
                const auto index = slot(value.stream_id, bidirectional_of(value.stream_id));
                if (!index) return;
                decoders_[*index].reset(records_[*index], value.application_error, at_ns, &budget_);
                after_update(*index);
            } else if constexpr (std::is_same_v<T, transport::PeerStopSendingEvent>) {
                if (limit_reached_) return;
                const auto index = slot(value.stream_id, bidirectional_of(value.stream_id));
                if (!index) return;
                decoders_[*index].stop_sending(records_[*index], value.application_error, at_ns, &budget_);
                after_update(*index);
            } else if constexpr (std::is_same_v<T, transport::PeerCloseEvent>) {
                if (peer_close_) return;  // the first close is the one that counts
                PeerCloseInfo info;
                info.space = value.error_space;
                info.code = value.error_code;
                info.reason.reserve(value.reason.size());
                for (const auto byte : value.reason) info.reason.push_back(static_cast<char>(byte));
                info.at_ns = at_ns;
                peer_close_ = std::move(info);
            } else if constexpr (std::is_same_v<T, transport::ConnectionEstablishedEvent>) {
                if (!established_ns_) established_ns_ = at_ns;
            } else if constexpr (std::is_same_v<T, transport::EventQueueOverflowEvent>) {
                limit_reached_ = true;  // events were lost: the recording is incomplete
            }
        },
        event);
}

void LiteSession::note_local_write(std::uint64_t stream_id, bool bidirectional, std::span<const std::byte> bytes,
                                   bool fin, std::uint64_t at_ns) {
    if (limit_reached_ || !admit_bytes(bytes.size())) return;
    const bool by_id = bidirectional_of(stream_id);
    const auto index = slot(stream_id, by_id);
    if (!index) return;
    budget_.bytes_left -= bytes.size();
    total_bytes_ += bytes.size();
    if (bidirectional != by_id) {
        decoders_[*index].note_runner_issue(
            records_[*index], kIssueLocalBidiMismatch,
            std::string("caller said ") + (bidirectional ? "bidirectional" : "unidirectional") + " for stream " +
                std::to_string(stream_id) + "; the stream id says otherwise",
            at_ns, &budget_);
    }
    decoders_[*index].feed_local(records_[*index], bytes, fin, at_ns, &budget_);
    after_update(*index);
}

const LiteStreamRecord* LiteSession::find(std::uint64_t stream_id) const {
    const auto found = index_.find(stream_id);
    return found == index_.end() ? nullptr : &records_[found->second];
}

std::optional<std::size_t> LiteSession::slot(std::uint64_t stream_id, bool bidirectional) {
    if (const auto found = index_.find(stream_id); found != index_.end()) return found->second;
    if (records_.size() >= limits_.max_streams) {
        limit_reached_ = true;
        return std::nullopt;
    }
    records_.push_back(make_lite_stream_record(stream_id, origin_of(stream_id), bidirectional));
    decoders_.emplace_back(records_.back(), limits_.decode, limits_.max_messages_per_stream);
    const auto index = records_.size() - 1;
    index_.emplace(stream_id, index);
    return index;
}

bool LiteSession::admit_bytes(std::size_t size) {
    if (size > budget_.bytes_left) {
        limit_reached_ = true;
        return false;
    }
    return true;
}

void LiteSession::after_update(std::size_t index) {
    if (decoders_[index].message_limit_reached() || budget_.exhausted) limit_reached_ = true;
}

std::vector<const LiteStreamRecord*> peer_streams(const LiteSession& session) {
    std::vector<const LiteStreamRecord*> out;
    for (const auto& record : session.streams()) {
        if (record.origin == LiteOrigin::Peer) out.push_back(&record);
    }
    return out;
}

std::vector<const LiteStreamRecord*> runner_streams(const LiteSession& session) {
    std::vector<const LiteStreamRecord*> out;
    for (const auto& record : session.streams()) {
        if (record.origin == LiteOrigin::Runner) out.push_back(&record);
    }
    return out;
}

}  // namespace moq::interop::session
