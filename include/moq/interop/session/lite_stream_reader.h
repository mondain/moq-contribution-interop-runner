#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/group.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"

namespace moq::interop::session {

// The kind of a moq-lite-06 stream, from its STREAM_TYPE (draft 7.2). Unknown: the type has not been read, the
// stream is a peer-opened bidirectional stream (unexpected for L1) or a runner stream the runner never declared.
enum class LiteStreamKind { Unknown, Setup, Group, Announce, Subscribe, Fetch, Probe, Goaway, Track, UnregisteredBidi, UnregisteredUni };
enum class LiteOrigin { Peer, Runner };

using LiteMessage = std::variant<wire::moqlite06::SetupMessage, wire::moqlite06::GroupHeader, wire::moqlite06::Frame,
    wire::moqlite06::AnnounceRequest, wire::moqlite06::AnnounceOk, wire::moqlite06::AnnounceStart, wire::moqlite06::AnnounceEnd,
    wire::moqlite06::AnnounceUpdate, wire::moqlite06::Subscribe, wire::moqlite06::SubscribeUpdate, wire::moqlite06::SubscribeOk,
    wire::moqlite06::SubscribeEnd, wire::moqlite06::SubscribeDrop>;

// stream_event_index: the 0-based index of the event on THIS stream (every feed, local write, reset or
// STOP_SENDING counts as one event) that completed the message or raised the issue. `from` tells which endpoint
// wrote the bytes: the runner's own writes are decoded as well (ANNOUNCE_REQUEST, SUBSCRIBE, its SETUP).
struct LiteDecoded {
    std::size_t stream_event_index;
    LiteMessage message;
    std::uint64_t at_ns;
    LiteOrigin from{LiteOrigin::Peer};
};

// Issue codes. From the codecs: "protocol_violation", "invalid_value", "length_exceeds_limit", "offset_overflow"
// (plus "key_value_formatting_error" and "length_not_representable", which the lite codecs never return).
// From the reader: "unknown_announce_type" (skipped by Message Length, decoding continues), "trailing_after_fin",
// "trailing_after_setup", "trailing_after_request", "truncated_at_fin", "publisher_opened_bidi",
// "l2_stream_not_decoded", "undeclared_runner_stream", "message_limit_reached". Every code except
// "unknown_announce_type", "publisher_opened_bidi" and "l2_stream_not_decoded" stops decoding that direction of the
// stream; further bytes are only counted.
struct LiteDecodeIssue {
    std::size_t stream_event_index;
    std::string code;
    std::string detail;
    LiteOrigin from{LiteOrigin::Peer};
};

inline constexpr std::string_view kIssueUnknownAnnounceType = "unknown_announce_type";
inline constexpr std::string_view kIssueProtocolViolation = "protocol_violation";
inline constexpr std::string_view kIssueInvalidValue = "invalid_value";
inline constexpr std::string_view kIssueLengthExceedsLimit = "length_exceeds_limit";
inline constexpr std::string_view kIssueOffsetOverflow = "offset_overflow";
inline constexpr std::string_view kIssueTrailingAfterFin = "trailing_after_fin";
inline constexpr std::string_view kIssueTrailingAfterSetup = "trailing_after_setup";
inline constexpr std::string_view kIssueTrailingAfterRequest = "trailing_after_request";
inline constexpr std::string_view kIssueTruncatedAtFin = "truncated_at_fin";
inline constexpr std::string_view kIssuePublisherOpenedBidi = "publisher_opened_bidi";
inline constexpr std::string_view kIssueL2StreamNotDecoded = "l2_stream_not_decoded";
inline constexpr std::string_view kIssueUndeclaredRunnerStream = "undeclared_runner_stream";
inline constexpr std::string_view kIssueMessageLimitReached = "message_limit_reached";

// fin_seen, reset_*, bytes: the PEER's direction (bytes the runner received). local_*: the runner's direction.
// stop_sending_code: the peer asked the runner to stop sending on this stream.
struct LiteStreamRecord {
    std::uint64_t stream_id;
    LiteOrigin origin;
    bool bidirectional;
    LiteStreamKind kind;
    std::optional<std::uint64_t> stream_type;  // raw STREAM_TYPE value once read
    std::vector<LiteDecoded> messages;
    std::vector<LiteDecodeIssue> issues;
    bool fin_seen{false};
    std::optional<std::uint64_t> reset_code;
    std::optional<std::uint64_t> stop_sending_code;
    std::size_t bytes{0};
    std::uint64_t opened_ns{0};
    bool reset_seen{false};         // true also for a reset without an application error code
    bool stop_sending_seen{false};  // likewise
    std::size_t local_bytes{0};
    bool local_fin{false};
};

std::string_view to_string(LiteStreamKind kind);
// The draft name of the message held, for example "ANNOUNCE_OK" or "FRAME".
std::string_view lite_message_name(const LiteMessage& message);

namespace detail {

// The incremental decoder behind LiteStreamReader and LiteSession: it keeps no record of its own and writes into
// the LiteStreamRecord it is handed, so the session can hold the records contiguously. Never throws (allocation
// failure aside); every buffer is bounded by the DecodeLimits (one message at most) and dropped once a direction
// stops decoding.
class LiteStreamDecoder {
public:
    LiteStreamDecoder(LiteStreamRecord& record, const wire::moqlite06::DecodeLimits& limits, std::size_t max_messages);

    void feed(LiteStreamRecord& record, std::span<const std::byte> data, bool fin, std::uint64_t at_ns);
    void feed_local(LiteStreamRecord& record, std::span<const std::byte> data, bool fin, std::uint64_t at_ns);
    void reset(LiteStreamRecord& record, std::optional<std::uint64_t> code, std::uint64_t at_ns);
    void stop_sending(LiteStreamRecord& record, std::optional<std::uint64_t> code, std::uint64_t at_ns);
    [[nodiscard]] bool message_limit_reached() const noexcept { return message_limit_reached_; }

    enum class Phase { StreamType, First, Rest, Done, Raw, Stopped, AwaitKind };
    struct Direction {
        std::vector<std::byte> buffer;
        std::size_t start{0};
        Phase phase{Phase::StreamType};
        bool opener{true};
        LiteOrigin from{LiteOrigin::Peer};
        bool fin{false};
        bool trailing_reported{false};
    };

private:
    std::size_t begin_event(LiteStreamRecord& record, std::uint64_t at_ns);
    void ingest(LiteStreamRecord& record, Direction& direction, std::span<const std::byte> data, bool fin,
                std::size_t event, std::uint64_t at_ns);
    void pump(LiteStreamRecord& record, Direction& direction, std::size_t event, std::uint64_t at_ns);
    void on_kind_known(LiteStreamRecord& record, std::size_t event);
    void finish(LiteStreamRecord& record, Direction& direction, std::size_t event);
    void issue(LiteStreamRecord& record, const Direction& direction, std::size_t event, std::string_view code,
               std::string detail);
    void stop(Direction& direction);
    static void stop_buffering(Direction& direction);

    wire::moqlite06::DecodeLimits limits_;
    std::size_t max_messages_;
    Direction inbound_;  // the peer's bytes
    Direction local_;    // the runner's bytes
    std::size_t events_{0};
    std::size_t messages_and_skips_{0};  // decoded messages plus skipped unknown ANNOUNCE types
    bool kind_final_{false};
    bool wake_inbound_{false};
    bool message_limit_reached_{false};
};

}  // namespace detail

// A fresh record: kind Unknown, nothing seen.
LiteStreamRecord make_lite_stream_record(std::uint64_t stream_id, LiteOrigin origin, bool bidirectional);

// One per stream; feeds bytes and decodes them incrementally into record(). Never throws.
class LiteStreamReader {
public:
    LiteStreamReader(std::uint64_t stream_id, LiteOrigin origin, bool bidirectional,
                     const wire::moqlite06::DecodeLimits& limits = wire::moqlite06::kDefaultLimits,
                     std::size_t max_messages = std::numeric_limits<std::size_t>::max());

    // The peer's bytes on this stream.
    void feed(std::span<const std::byte> data, bool fin, std::uint64_t at_ns);
    // The runner's own bytes on this stream; on a runner-opened stream the STREAM_TYPE written sets the kind.
    void feed_local(std::span<const std::byte> data, bool fin, std::uint64_t at_ns);
    void reset(std::uint64_t code);
    void stop_sending(std::uint64_t code);

    [[nodiscard]] const LiteStreamRecord& record() const noexcept { return record_; }
    [[nodiscard]] bool message_limit_reached() const noexcept { return decoder_.message_limit_reached(); }

private:
    LiteStreamRecord record_;
    detail::LiteStreamDecoder decoder_;
    std::uint64_t last_ns_{0};
};

}  // namespace moq::interop::session
