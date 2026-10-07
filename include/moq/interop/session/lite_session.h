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
// recorded (a peer CONNECTION_CLOSE is still captured).
struct LiteSessionLimits {
    std::size_t max_streams = 1024;
    std::size_t max_bytes = std::size_t{64} << 20;  // peer and runner bytes over all streams
    std::size_t max_messages_per_stream = 100000;
    wire::moqlite06::DecodeLimits decode = wire::moqlite06::kDefaultLimits;
};

struct PeerCloseInfo {
    transport::CloseErrorSpace space = transport::CloseErrorSpace::Transport;
    std::uint64_t code = 0;
    std::string reason;
    std::uint64_t at_ns = 0;
};

// Owns the stream readers of one connection. Peer streams are classified from the QUIC stream id (bit 0 set: the
// server, i.e. the runner, opened it; bit 1 set: unidirectional); runner streams take their kind from the
// STREAM_TYPE the runner wrote (note_local_write). streams() is in first-seen order. Never throws.
class LiteSession {
public:
    explicit LiteSession(LiteSessionLimits limits = {});

    // Handles StreamData, PeerReset, PeerStopSending, PeerClose and ConnectionEstablished; an
    // EventQueueOverflow means events were lost, so it sets limit_reached(). Other events are ignored.
    void on_event(const transport::TransportEvent& event, std::uint64_t at_ns);
    void note_local_write(std::uint64_t stream_id, bool bidirectional, std::span<const std::byte> bytes, bool fin,
                          std::uint64_t at_ns);

    [[nodiscard]] const std::vector<LiteStreamRecord>& streams() const noexcept { return records_; }
    [[nodiscard]] const LiteStreamRecord* find(std::uint64_t stream_id) const;
    [[nodiscard]] std::optional<PeerCloseInfo> peer_close() const { return peer_close_; }
    [[nodiscard]] std::optional<std::uint64_t> established_ns() const { return established_ns_; }
    [[nodiscard]] bool limit_reached() const noexcept { return limit_reached_; }
    [[nodiscard]] std::size_t total_bytes() const noexcept { return total_bytes_; }

private:
    // The record index; nullopt when the stream is new and max_streams is reached (limit_reached() is set).
    std::optional<std::size_t> slot(std::uint64_t stream_id, bool bidirectional);
    // False (and limit_reached() set) when the bytes would take the session over max_bytes; the caller adds them.
    bool admit_bytes(std::size_t size);
    void after_update(std::size_t index);

    LiteSessionLimits limits_;
    std::vector<LiteStreamRecord> records_;
    std::vector<detail::LiteStreamDecoder> decoders_;
    std::map<std::uint64_t, std::size_t> index_;
    std::size_t total_bytes_{0};
    bool limit_reached_{false};
    std::optional<PeerCloseInfo> peer_close_;
    std::optional<std::uint64_t> established_ns_;
};

}  // namespace moq::interop::session
