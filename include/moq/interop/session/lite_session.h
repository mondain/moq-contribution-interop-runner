#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "moq/interop/session/lite_stream_reader.h"
#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::session {

// The evidence kinds lite scenarios record (registered in requirements::known_evidence_kind).
inline constexpr std::string_view kLiteStreamOpenedKind = "lite_stream_opened";
inline constexpr std::string_view kLiteMessageKind = "lite_message";
inline constexpr std::string_view kLiteDecodeErrorKind = "lite_decode_error";

// Harness resource limits, never protocol judgements. Once one is hit, limit_reached() is true and nothing more is
// recorded for the whole session (a peer CONNECTION_CLOSE is still captured); that includes the per-stream message
// cap: one stream reaching it ends recording everywhere.
//
// Memory accounting: max_bytes is charged the wire bytes of every peer and runner write PLUS, per stored message,
// sizeof(LiteDecoded) and, per stored issue, sizeof(LiteDecodeIssue) + its code and detail lengths (payloads are
// already in the wire count). So the recorded data stays within max_bytes whatever the wire-to-memory ratio (an
// empty FRAME is 2 wire bytes but a full LiteDecoded); with vector growth slack the resident size is at most about
// twice max_bytes (default 64 MiB: well under 200 MiB). max_messages_total caps the stored messages over all
// streams (default 250000, about 38 MiB of LiteDecoded at 152 bytes each).
struct LiteSessionLimits {
    std::size_t max_streams = 1024;
    std::size_t max_bytes = std::size_t{64} << 20;
    std::size_t max_messages_per_stream = 100000;  // per direction of a stream
    std::size_t max_messages_total = 250000;
    wire::moqlite06::DecodeLimits decode = wire::moqlite06::kDefaultLimits;
};

struct PeerCloseInfo {
    transport::CloseErrorSpace space = transport::CloseErrorSpace::Transport;
    std::uint64_t code = 0;
    std::string reason;
    std::uint64_t at_ns = 0;
};

// EVALUATORS MUST USE ONLY THE ACCESSORS: peer_streams()/runner_streams() here and peer_messages(),
// peer_issues(), peer_protocol_issues(), harness_issues() and classify_issue() in lite_stream_reader.h.
//
// Owns the stream readers of one connection. Peer streams are classified from the QUIC stream id (bit 0 set: the
// server, i.e. the runner, opened it; bit 1 set: unidirectional); runner streams take their kind from the
// STREAM_TYPE the runner wrote (note_local_write). streams() is in first-seen order. Never throws.
class LiteSession {
public:
    explicit LiteSession(LiteSessionLimits limits = {});

    // Handles StreamData, PeerReset, PeerStopSending, PeerClose and ConnectionEstablished; an
    // EventQueueOverflow means events were lost, so it sets limit_reached(). Other events are ignored.
    void on_event(const transport::TransportEvent& event, std::uint64_t at_ns);
    // Directionality is taken from the stream id (bit 1); a caller flag that disagrees raises a
    // local_bidi_mismatch issue (Harness) on the stream.
    void note_local_write(std::uint64_t stream_id, bool bidirectional, std::span<const std::byte> bytes, bool fin,
                          std::uint64_t at_ns);

    [[nodiscard]] const std::vector<LiteStreamRecord>& streams() const noexcept { return records_; }
    [[nodiscard]] const LiteStreamRecord* find(std::uint64_t stream_id) const;
    [[nodiscard]] std::optional<PeerCloseInfo> peer_close() const { return peer_close_; }
    [[nodiscard]] std::optional<std::uint64_t> established_ns() const { return established_ns_; }
    [[nodiscard]] bool limit_reached() const noexcept { return limit_reached_; }
    // Wire bytes recorded (peer and runner).
    [[nodiscard]] std::size_t total_bytes() const noexcept { return total_bytes_; }
    // Everything charged against max_bytes: wire bytes plus the stored messages' and issues' sizes.
    [[nodiscard]] std::size_t charged_bytes() const noexcept { return limits_.max_bytes - budget_.bytes_left; }
    [[nodiscard]] std::size_t message_count() const noexcept {
        return limits_.max_messages_total - budget_.messages_left;
    }

private:
    // The record index; nullopt when the stream is new and max_streams is reached (limit_reached() is set).
    std::optional<std::size_t> slot(std::uint64_t stream_id, bool bidirectional);
    // Charges the bytes against the budget; false (and limit_reached() set) when they would exceed max_bytes.
    bool admit_bytes(std::size_t size);
    void after_update(std::size_t index);

    LiteSessionLimits limits_;
    std::vector<LiteStreamRecord> records_;
    std::vector<detail::LiteStreamDecoder> decoders_;
    std::map<std::uint64_t, std::size_t> index_;
    std::size_t total_bytes_{0};
    detail::LiteBudget budget_;
    bool limit_reached_{false};
    std::optional<PeerCloseInfo> peer_close_;
    std::optional<std::uint64_t> established_ns_;
};

// Streams the peer opened / the runner opened, in first-seen order.
std::vector<const LiteStreamRecord*> peer_streams(const LiteSession& session);
std::vector<const LiteStreamRecord*> runner_streams(const LiteSession& session);

}  // namespace moq::interop::session
