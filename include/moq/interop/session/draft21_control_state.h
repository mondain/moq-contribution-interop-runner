#pragma once

#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/draft21/control.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace moq::interop::session::draft21 {

enum class ControlPhase { AwaitingTransport, AwaitingSetup, Active, Closing };

struct ControlResult {
    std::vector<wire::draft21::ControlMessage> messages;
    std::optional<std::uint64_t> close_error;
    bool harness_limit = false;
};

// Draft-21-only control-stream state. The caller owns QUIC I/O and sends
// local SETUP; this class validates peer control bytes and activation.
class ControlState {
public:
    explicit ControlState(std::size_t maximum_buffer_bytes = 65'546);

    std::optional<std::uint64_t> on_transport_established(
        std::span<const std::byte> alpn);
    void on_local_setup_sent();
    ControlResult on_peer_data(transport::StreamId stream_id,
                               std::span<const std::byte> data, bool fin);
    [[nodiscard]] ControlPhase phase() const noexcept;

private:
    void refresh_phase();

    std::size_t maximum_buffer_bytes_;
    std::vector<std::byte> pending_;
    std::optional<transport::StreamId> peer_control_stream_;
    ControlPhase phase_{ControlPhase::AwaitingTransport};
    bool transport_established_{false};
    bool local_setup_sent_{false};
    bool peer_setup_received_{false};
};

}  // namespace moq::interop::session::draft21
