#pragma once

#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/draft21/key_values.h"

#include <memory>
#include <optional>
#include <vector>

namespace moq::interop::session {

struct Draft23SessionConfig {
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name;
    bool webtransport{false};
    bool subscribe_immediately{false};
    std::size_t maximum_streams{128};
    std::size_t maximum_buffered_bytes{1u << 20};
    std::size_t maximum_stream_bytes{1u << 18};
};

// The publisher's SETUP exactly as received, kept even when the session then closes over it.
struct Draft23PeerSetup {
    wire::draft21::KeyValues options;
    std::uint64_t frame_length{0};
};

// One publisher-opened unidirectional data stream, kept after the stream finishes or is reset. `valid_header` is
// false when the first varint is not a SUBGROUP_HEADER type; the other header fields are then unset.
struct Draft23DataStream {
    std::uint64_t stream_id{0};
    std::uint64_t type_flags{0};
    bool valid_header{false};
    std::uint64_t track_alias{0};
    std::uint64_t group_id{0};
    std::optional<std::uint64_t> subgroup_id;      // from the header, or the first Object ID in mode 0b01
    std::optional<std::uint64_t> first_object_id;
    std::uint64_t objects{0};
    bool fin{false};
    bool reset{false};
};

struct Draft23SessionStats {
    bool connected{false};
    bool setup_received{false};
    bool namespace_announced{false};
    bool subscription_established{false};
    std::uint64_t objects{0};
    std::uint64_t payload_bytes{0};
    std::uint64_t distinct_objects{0};
    std::optional<Draft23PeerSetup> peer_setup;
    std::vector<Draft23DataStream> data_streams;  // bounded; see data_streams_truncated
    bool data_streams_truncated{false};
    [[nodiscard]] bool progressing() const noexcept { return distinct_objects >= 2 && payload_bytes > 0; }
};

// A bounded publisher smoke receiver, not a full draft-23 conformance engine.
// Call flush() each polling iteration to retry writes after transport backpressure.
class Draft23Session {
public:
    Draft23Session(transport::SessionTransport& transport, Draft23SessionConfig config);
    ~Draft23Session();
    Draft23Session(const Draft23Session&) = delete;
    Draft23Session& operator=(const Draft23Session&) = delete;
    void on_event(const transport::TransportEvent& event);
    void flush();
    [[nodiscard]] const Draft23SessionStats& stats() const noexcept;
    [[nodiscard]] const std::optional<std::string>& error() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> application_error() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moq::interop::session
