#pragma once

#include "moq/interop/session/draft23_probes.h"
#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/draft21/key_values.h"

#include <memory>
#include <string>
#include <optional>
#include <vector>

namespace moq::interop::session {

struct Draft23SessionConfig {
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name;
    bool webtransport{false};
    bool subscribe_immediately{false};
    // The publisher offers its tracks with PUBLISH (moqxr --forward 1) instead of waiting for a SUBSCRIBE. The harness then
    // accepts every PUBLISH in the fixture namespace, not only the fixture track, and does not subscribe on its own.
    bool publisher_pushes{false};
    const Draft23Probe* probe{nullptr};  // a negative probe run; the session then injects its malformed input
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

// A control message the publisher sent on a request stream: its type code and the length of its body.
struct Draft23ControlMessage {
    std::uint64_t type{0};
    std::uint64_t body_length{0};
};

struct Draft23PublishDone {
    std::uint64_t status_code{0};
    std::uint64_t stream_count{0};
};

// One request stream, kept after it ends. `local` streams are the ones the harness opened (SUBSCRIBE);
// the publisher opened the others. Only the publisher's messages and FIN are recorded.
struct Draft23RequestStream {
    std::uint64_t stream_id{0};
    bool local{false};
    std::optional<std::uint64_t> request_id;
    std::vector<Draft23ControlMessage> messages;
    std::optional<Draft23PublishDone> publish_done;
    bool fin{false};
    bool fin_after_publish_done{false};
    bool established{false};  // the harness accepted this request as the media subscription
};

// How the publisher ended the connection after a negative probe. `application` is false for a transport-level close.
struct Draft23ProbeClose {
    bool application{false};
    std::uint64_t code{0};
};

// The first message the publisher sent back on the probe stream. `error_code` and `detail_types` are set for a
// REQUEST_ERROR; the detail types are listed in wire order so a repeated type is visible.
struct Draft23ProbeReply {
    std::uint64_t type{0};
    std::optional<std::uint64_t> error_code;
    std::vector<std::uint64_t> detail_types;
};

// How the publisher ended the probe's request stream: by resetting its half or by asking the runner to stop sending.
struct Draft23ProbeStreamEnd {
    enum class Kind { Reset, StopSending };
    Kind kind{Kind::Reset};
    std::optional<std::uint64_t> code;
};

struct Draft23OtherStreamEnd {
    std::uint64_t stream_id{0};
    Draft23ProbeStreamEnd end;
};

struct Draft23ProbeResult {
    bool injected{false};
    std::optional<Draft23ProbeClose> close;  // the first close the publisher caused
    std::uint64_t reply_bytes{0};            // what the publisher sent back on the stream the probe used
    bool reply_fin{false};
    std::optional<Draft23ProbeReply> reply;
    std::optional<Draft23ProbeStreamEnd> stream_end;
    std::vector<Draft23OtherStreamEnd> other_stream_ends;  // resets or stop-sendings of the publisher's own streams
};

// A track the publisher announced with PUBLISH: the first field of its namespace, its name and Track Alias.
struct Draft23PublishedTrack {
    std::string namespace_first;
    std::string name;
    std::uint64_t alias{0};
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
    std::vector<Draft23RequestStream> requests;
    bool duplicate_request_id{false};
    bool oversize_control_message{false};
    std::vector<Draft23PublishedTrack> published_tracks;
    std::vector<std::string> announced_namespaces;  // first field of each PUBLISH_NAMESPACE namespace
    bool empty_track_name{false};
    bool empty_namespace_field{false};
    bool duplicate_parameter{false};
    bool delta_overflow{false};
    bool control_stream_closed{false};
    std::uint64_t max_datagram_payload{0};  // 0 when QUIC DATAGRAM was not negotiated
    std::uint64_t padding_streams{0};
    bool padding_nonzero{false};
    std::optional<Draft23ProbeResult> probe;  // present only in negative probe runs
    std::optional<std::uint64_t> subscription_alias;  // Track Alias of the accepted media subscription
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
