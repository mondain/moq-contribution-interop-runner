#pragma once

#include "moq/interop/session/draft21_control_state.h"
#include "moq/interop/session/draft21_publish_open.h"
#include "moq/interop/transport/session_transport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop::scenarios {

using Draft21Clock = std::chrono::steady_clock;

enum class Draft21AnnouncementStatus { Running, Passed, Failed, TimedOut };
enum class Draft21SetupProbe {
    None, UnknownOption, DuplicateUnknownOption, ServerAuthority, ServerPath
};

enum class Draft21AnnouncementEventKind {
    TransportEstablished,
    LocalSetupSent,
    PeerSetupReceived,
    NamespaceObserved,
    NamespaceResponseDelivered,
    PublishObserved,
    ResponseDelivered,
    UnsupportedStream,
    InvalidRequestOpener,
    ProtocolViolation,
    PeerClosed,
    HarnessLimit,
};

struct Draft21AnnouncementEvent {
    Draft21AnnouncementEventKind kind;
    std::optional<transport::StreamId> stream_id;
    std::optional<std::uint64_t> request_id;
    std::optional<std::uint64_t> application_close_code;
    std::vector<std::uint64_t> setup_option_types;
};

// One decoded Setup Option as received from the publisher (slice A).
struct Draft21SetupOptionValue {
    std::uint64_t type{0};
    bool is_bytes{false};
    std::uint64_t integer{0};
    std::vector<std::byte> bytes;
};

// The moqt URI handed to a driven native-QUIC publisher, split into the pieces
// Section 9.1.1 and 9.1.2 require in AUTHORITY and PATH.
struct Draft21ExpectedConnectionUri {
    std::string authority;
    std::string path_and_query;
};

struct Draft21AnnouncementContext {
    bool complete{false};
    bool target_publish_seen{false};
    bool response_delivered{false};
    std::vector<Draft21AnnouncementEvent> evidence;
    Draft21SetupProbe setup_probe{Draft21SetupProbe::None};
    bool webtransport{false};
    std::vector<std::uint64_t> peer_setup_option_types;
    // Slice A additions: decoded option values, the scenario being run, the
    // URI given to a driven native publisher, and whether the observation
    // window ended with a live, fully set up session.
    std::vector<Draft21SetupOptionValue> peer_setup_options;
    std::string scenario_id;
    std::optional<Draft21ExpectedConnectionUri> expected_uri;
    bool window_elapsed{false};
};

struct Draft21AnnouncementSnapshot {
    Draft21AnnouncementStatus status{Draft21AnnouncementStatus::Running};
    bool harness_failed{false};
};

// A bounded draft-21 native-QUIC announcement scenario, not a full relay.
// It observes a specified PUBLISH and responds with an empty REQUEST_OK.
class Draft21AnnouncementController {
public:
    Draft21AnnouncementController(
        transport::SessionTransport& transport,
        std::vector<std::vector<std::byte>> expected_namespace,
        std::vector<std::byte> expected_track_name,
        std::chrono::milliseconds timeout,
        Draft21SetupProbe setup_probe = Draft21SetupProbe::None,
        bool webtransport = false);

    // Records the scenario and, for a driven native publisher, the connection
    // URI so evaluators can compare the SETUP AUTHORITY and PATH options.
    void configure_scenario(std::string scenario_id,
                            std::optional<Draft21ExpectedConnectionUri> uri = std::nullopt);

    Draft21AnnouncementSnapshot poll(Draft21Clock::time_point now);
    [[nodiscard]] const Draft21AnnouncementContext& context() const noexcept;

private:
    struct PendingPublication {
        transport::StreamId stream_id;
        std::uint64_t request_id;
        bool target;
    };
    struct PendingNamespace {
        transport::StreamId stream_id;
        std::uint64_t request_id;
        bool forbidden_dot;
    };
    struct PendingWrite {
        transport::StreamId stream_id;
        std::vector<std::byte> bytes;
        std::size_t offset{0};
        bool setup{false};
        bool target_response{false};
        std::uint64_t request_id{0};
        bool namespace_response{false};
        bool fin_after{false};
    };

    void record(Draft21AnnouncementEventKind kind,
                std::optional<transport::StreamId> stream_id = std::nullopt,
                std::optional<std::uint64_t> request_id = std::nullopt,
                std::optional<std::uint64_t> application_close_code =
                    std::nullopt,
                std::vector<std::uint64_t> setup_option_types = {});
    void fail_harness();
    void close_protocol(std::uint64_t error,
                        std::optional<transport::StreamId> stream_id);
    void handle_uni(const transport::StreamDataEvent& event);
    void handle_control(transport::StreamId stream_id,
                        std::span<const std::byte> data, bool fin);
    void handle_request(const transport::StreamDataEvent& event);
    void handle_event(const transport::TransportEvent& event);
    void open_local_setup();
    void queue_pending_responses();
    void flush_writes();

    transport::SessionTransport& transport_;
    std::vector<std::vector<std::byte>> expected_namespace_;
    std::vector<std::byte> expected_track_name_;
    std::chrono::milliseconds timeout_;
    session::draft21::ControlState control_;
    session::draft21::PublishOpenState publish_open_;
    Draft21AnnouncementContext context_;
    std::optional<Draft21Clock::time_point> started_;
    std::optional<transport::StreamId> peer_control_stream_;
    std::map<transport::StreamId, std::vector<std::byte>> uni_probes_;
    std::vector<transport::StreamId> ignored_uni_streams_;
    std::vector<PendingPublication> pending_publications_;
    std::vector<PendingNamespace> pending_namespaces_;
    std::deque<PendingWrite> writes_;
    Draft21AnnouncementStatus status_{Draft21AnnouncementStatus::Running};
    bool transport_established_{false};
    bool local_setup_opened_{false};
    bool harness_failed_{false};
};

}  // namespace moq::interop::scenarios
