#pragma once

#include "moq/interop/scenarios/engine.h"
#include "moq/interop/transport/session_transport.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace moq::interop::scenarios {

enum class DispatchState { Complete, Pending, Failed };

struct DispatchResult {
    DispatchState state{DispatchState::Failed};
    bool stimulus_delivered{false};
    std::optional<transport::StreamId> stream_id;
    std::optional<transport::TransportStatus> transport_error;
};

class ActionDispatcher {
public:
    ActionDispatcher(transport::SessionTransport& transport,
                     session::PublisherSession& session);

    DispatchResult submit(const OpenRequestAction& action);
    DispatchResult submit(const session::SendMessageAction& action);
    DispatchResult submit_setup();
    DispatchResult flush();
    [[nodiscard]] bool has_pending() const noexcept;

private:
    struct PendingMessage {
        wire::draft18::Message message;
        session::LocalStreamPurpose purpose;
        bool fin;
        std::vector<std::byte> frame;
        std::optional<transport::StreamId> stream_id;
        std::size_t accepted{0};
        bool fin_sent{false};
    };

    transport::SessionTransport& transport_;
    session::PublisherSession& session_;
    std::optional<PendingMessage> pending_;
    std::uint64_t next_request_id_{1};
    bool setup_submitted_{false};
};

}  // namespace moq::interop::scenarios
