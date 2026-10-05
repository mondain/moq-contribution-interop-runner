#include "moq/interop/session/draft21_control_state.h"

#include "moq/interop/scenarios/wire_draft.h"

#include <algorithm>
#include <array>
#include <utility>
#include <variant>

namespace moq::interop::session::draft21 {
namespace {

constexpr std::uint64_t kProtocolViolation = 0x3;
constexpr std::array<std::byte, 7> kAlpn{
    std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
    std::byte{'-'}, std::byte{'2'}, std::byte{'1'}};
// A draft 22 lineage run (wire draft 22 on this thread) runs this draft 21 state on moqt-22.
constexpr std::array<std::byte, 7> kLineageAlpn22{
    std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
    std::byte{'-'}, std::byte{'2'}, std::byte{'2'}};

}  // namespace

ControlState::ControlState(std::size_t maximum_buffer_bytes)
    : maximum_buffer_bytes_(maximum_buffer_bytes) {}

void ControlState::refresh_phase() {
    if (phase_ == ControlPhase::Closing) return;
    phase_ = !transport_established_ ? ControlPhase::AwaitingTransport
           : local_setup_sent_ && peer_setup_received_ ? ControlPhase::Active
                                                       : ControlPhase::AwaitingSetup;
}

std::optional<std::uint64_t> ControlState::on_transport_established(
    std::span<const std::byte> alpn) {
    if (phase_ == ControlPhase::Closing) return kProtocolViolation;
    if (!std::ranges::equal(alpn, scenarios::current_wire_draft() == 22 ? kLineageAlpn22 : kAlpn)) {
        phase_ = ControlPhase::Closing;
        return kProtocolViolation;
    }
    transport_established_ = true;
    refresh_phase();
    return std::nullopt;
}

void ControlState::on_local_setup_sent() {
    if (phase_ == ControlPhase::Closing) return;
    local_setup_sent_ = true;
    refresh_phase();
}

ControlResult ControlState::on_peer_data(
    transport::StreamId stream_id, std::span<const std::byte> data, bool fin) {
    ControlResult result;
    if (phase_ == ControlPhase::Closing) return result;
    if ((stream_id & 3u) != 2u ||
        (peer_control_stream_ && *peer_control_stream_ != stream_id)) {
        phase_ = ControlPhase::Closing;
        result.close_error = kProtocolViolation;
        return result;
    }
    peer_control_stream_ = stream_id;
    if (pending_.size() > maximum_buffer_bytes_ ||
        data.size() > maximum_buffer_bytes_ - pending_.size()) {
        phase_ = ControlPhase::Closing;
        result.harness_limit = true;
        return result;
    }
    pending_.insert(pending_.end(), data.begin(), data.end());
    while (!pending_.empty()) {
        wire::Cursor cursor(pending_);
        const auto decoded = wire::draft21::decode_control_message(
            cursor, !peer_setup_received_, true);
        if (std::holds_alternative<wire::NeedMore>(decoded)) break;
        if (std::holds_alternative<wire::DecodeError>(decoded)) {
            phase_ = ControlPhase::Closing;
            result.close_error = kProtocolViolation;
            result.decode_detail = std::get<wire::DecodeError>(decoded).detail;
            return result;
        }
        auto message = std::get<wire::draft21::ControlMessage>(decoded);
        if (std::holds_alternative<wire::draft21::SetupMessage>(message)) {
            peer_setup_received_ = true;
            refresh_phase();
        }
        result.messages.push_back(std::move(message));
        pending_.erase(pending_.begin(),
                       pending_.begin() + static_cast<std::ptrdiff_t>(cursor.offset()));
    }
    if (fin) {
        phase_ = ControlPhase::Closing;
        result.close_error = kProtocolViolation;
    }
    return result;
}

ControlPhase ControlState::phase() const noexcept {
    return phase_;
}

}  // namespace moq::interop::session::draft21
