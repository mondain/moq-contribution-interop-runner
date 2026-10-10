#pragma once

#include <array>
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
#include "moq/interop/wire/moqlite06/fetch.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/goaway.h"
#include "moq/interop/wire/moqlite06/group.h"
#include "moq/interop/wire/moqlite06/probe.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "moq/interop/wire/moqlite06/track.h"

namespace moq::interop::session {

// The kind of a moq-lite-06 stream, from its STREAM_TYPE (draft 7.2). Unknown: the type has not been read, the
// stream is a peer-opened bidirectional stream (unexpected for L1) or a runner stream the runner never declared.
// Every peer-opened bidirectional stream is Unknown with a publisher_opened_bidi issue whatever its type, a
// GOAWAY (0x5) included: evaluators that care read the raw stream_type.
enum class LiteStreamKind { Unknown, Setup, Group, Announce, Subscribe, Fetch, Probe, Goaway, Track, UnregisteredBidi, UnregisteredUni };
enum class LiteOrigin { Peer, Runner };

using LiteMessage = std::variant<wire::moqlite06::SetupMessage, wire::moqlite06::GroupHeader, wire::moqlite06::Frame,
    wire::moqlite06::AnnounceRequest, wire::moqlite06::AnnounceOk, wire::moqlite06::AnnounceStart, wire::moqlite06::AnnounceEnd,
    wire::moqlite06::AnnounceUpdate, wire::moqlite06::Subscribe, wire::moqlite06::SubscribeUpdate, wire::moqlite06::SubscribeOk,
    wire::moqlite06::SubscribeEnd, wire::moqlite06::SubscribeDrop, wire::moqlite06::TrackRequest,
    wire::moqlite06::TrackInfo, wire::moqlite06::FetchRequest, wire::moqlite06::ProbeMessage,
    wire::moqlite06::GoawayMessage>;

// stream_event_index: the 0-based index of the event on THIS stream (every feed, local write, reset or
// STOP_SENDING counts as one event) that completed the message or raised the issue. `from` tells which endpoint
// wrote the bytes: the runner's own writes are decoded as well (ANNOUNCE_REQUEST, SUBSCRIBE, its SETUP).
struct LiteDecoded {
    std::size_t stream_event_index;
    LiteMessage message;
    std::uint64_t at_ns;
    LiteOrigin from{LiteOrigin::Peer};
};

// Issue codes, each with an explicit LiteIssueClass (see classify_issue). From the codecs: "protocol_violation",
// "invalid_value", "length_exceeds_limit", "offset_overflow", "key_value_formatting_error",
// "length_not_representable" (the last two are never returned by the lite codecs). From the reader:
// "unknown_announce_type" (skipped by Message Length, decoding continues), "trailing_after_fin",
// "trailing_after_setup", "trailing_after_request", "trailing_after_response" (a second TRACK_INFO),
// "unexpected_response" (reply bytes on a GOAWAY stream), "truncated_at_fin", "publisher_opened_bidi",
// "l2_stream_not_decoded" (kept for a stream kind the reader does not decode; none since L2a),
// "undeclared_runner_stream", "message_limit_reached", "buffer_limit_reached", "local_bidi_mismatch". Every code
// except "unknown_announce_type", "publisher_opened_bidi", "l2_stream_not_decoded" and "local_bidi_mismatch" stops
// decoding that direction of the stream; further bytes are only counted.
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
inline constexpr std::string_view kIssueKeyValueFormattingError = "key_value_formatting_error";
inline constexpr std::string_view kIssueLengthNotRepresentable = "length_not_representable";
inline constexpr std::string_view kIssueTrailingAfterFin = "trailing_after_fin";
inline constexpr std::string_view kIssueTrailingAfterSetup = "trailing_after_setup";
inline constexpr std::string_view kIssueTrailingAfterRequest = "trailing_after_request";
inline constexpr std::string_view kIssueTrailingAfterResponse = "trailing_after_response";
inline constexpr std::string_view kIssueUnexpectedResponse = "unexpected_response";
inline constexpr std::string_view kIssueTruncatedAtFin = "truncated_at_fin";
inline constexpr std::string_view kIssuePublisherOpenedBidi = "publisher_opened_bidi";
inline constexpr std::string_view kIssueL2StreamNotDecoded = "l2_stream_not_decoded";
inline constexpr std::string_view kIssueUndeclaredRunnerStream = "undeclared_runner_stream";
inline constexpr std::string_view kIssueMessageLimitReached = "message_limit_reached";
inline constexpr std::string_view kIssueBufferLimitReached = "buffer_limit_reached";
inline constexpr std::string_view kIssueLocalBidiMismatch = "local_bidi_mismatch";
// Datagram issues belong to a LiteDatagram, not to a stream (draft 6.4: a publisher MUST NOT send a body above 1200
// bytes; a header that does not decode is not a datagram body).
inline constexpr std::string_view kIssueDatagramOverLimit = "datagram_over_limit";
inline constexpr std::string_view kIssueDatagramMalformed = "datagram_malformed";

// Every issue code the reader and session emit (the test pins that each has an explicit class).
inline constexpr std::array<std::string_view, 21> kAllIssueCodes{
    kIssueUnknownAnnounceType, kIssueProtocolViolation, kIssueInvalidValue, kIssueLengthExceedsLimit,
    kIssueOffsetOverflow, kIssueKeyValueFormattingError, kIssueLengthNotRepresentable, kIssueTrailingAfterFin,
    kIssueTrailingAfterSetup, kIssueTrailingAfterRequest, kIssueTruncatedAtFin, kIssuePublisherOpenedBidi,
    kIssueL2StreamNotDecoded, kIssueUndeclaredRunnerStream, kIssueMessageLimitReached, kIssueBufferLimitReached,
    kIssueLocalBidiMismatch, kIssueTrailingAfterResponse, kIssueUnexpectedResponse, kIssueDatagramOverLimit,
    kIssueDatagramMalformed};

// How an evaluator may use an issue.
//   PeerProtocol: the peer's bytes broke the wire format; an evaluator may judge the peer on it (Fail).
//   Inconclusive: the reading is unsettled (decision (a): an unknown ANNOUNCE Type); the rows that depend on it are
//                 NotRun, never Pass and never Fail.
//   Harness:      a limit, a runner-side condition or a transport anomaly; the observation is incomplete, so the
//                 judgement is NotRun or a harness error, NEVER a Fail of the peer.
//   Informational: recorded for the transcript; not a judgement either way.
enum class LiteIssueClass { PeerProtocol, Inconclusive, Harness, Informational };

// nullopt for a code without an explicit class.
std::optional<LiteIssueClass> explicit_issue_class(std::string_view code);
// An unknown code is Harness: it can never become a peer Fail.
LiteIssueClass classify_issue(std::string_view code);

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
    // Bytes of the peer's direction buffered as an incomplete message (the inbound phase is First or Rest), 0
    // otherwise. A non-zero value at the end of a window means the record hides an undecoded tail.
    std::size_t peer_pending_bytes{0};
};

std::string_view to_string(LiteStreamKind kind);
// The draft name of the message held, for example "ANNOUNCE_OK" or "FRAME".
std::string_view lite_message_name(const LiteMessage& message);

// EVALUATORS MUST READ A RECORD ONLY THROUGH THESE ACCESSORS. record.messages and record.issues mix both
// directions: the runner's own SETUP, ANNOUNCE_REQUEST and SUBSCRIBE are decoded too, and the runner's deliberately
// malformed probe bytes raise issues; a naive scan would credit or blame the peer for them. Pointers stay valid
// until the record changes.
std::vector<const LiteDecoded*> peer_messages(const LiteStreamRecord& record);
std::vector<const LiteDecoded*> runner_messages(const LiteStreamRecord& record);
// The peer's TRACK_INFO replies (more than one is also a trailing_after_response issue), its FETCH response FRAMEs
// in arrival order (zigzag deltas as decoded; cumulative timestamps are the evaluator's), and its PROBE reports.
// Each returns the peer's messages of that alternative only, never the runner's own TRACK, FETCH or PROBE target.
std::vector<const LiteDecoded*> peer_track_info(const LiteStreamRecord& record);
std::vector<const LiteDecoded*> peer_fetch_frames(const LiteStreamRecord& record);
std::vector<const LiteDecoded*> peer_probe_reports(const LiteStreamRecord& record);
// Issues raised by the peer's bytes or the peer's stream (any class).
std::vector<const LiteDecodeIssue*> peer_issues(const LiteStreamRecord& record);
// The only issues an evaluator may turn into a Fail of the peer: from the peer and classified PeerProtocol.
std::vector<const LiteDecodeIssue*> peer_protocol_issues(const LiteStreamRecord& record);
// Runner-side Harness-class codes that are anomalies of the runner or the transport, never part of a deliberate
// probe: trailing_after_fin, offset_overflow, undeclared_runner_stream, local_bidi_mismatch,
// message_limit_reached, buffer_limit_reached.
bool is_runner_anomaly(std::string_view code);
// Issues that make the observation incomplete (judge NotRun, never Fail): every Harness-class issue raised by the
// PEER's bytes, plus runner-origin Harness-class issues that is_runner_anomaly() names. Harness-class issues the
// runner's own deliberately malformed probe bytes raise (length_exceeds_limit, length_not_representable from the
// runner) are the stimulus and are NOT returned.
std::vector<const LiteDecodeIssue*> harness_issues(const LiteStreamRecord& record);

namespace detail {

// A session-wide budget the decoders draw on before storing anything (LiteSession owns it). bytes_left is charged
// the wire bytes (by the session) and, per stored message or issue, its in-memory size (by the decoder), so the
// memory a peer can make the recorder hold is bounded by the budget whatever the wire-to-memory ratio.
struct LiteBudget {
    std::size_t bytes_left = std::numeric_limits<std::size_t>::max();
    std::size_t messages_left = std::numeric_limits<std::size_t>::max();
    bool exhausted = false;
};

// The bytes a stored message or issue is charged (the wire bytes it came from are charged separately).
inline constexpr std::size_t kMessageCharge = sizeof(LiteDecoded);
std::size_t issue_charge(std::string_view code, std::string_view detail);

// The incremental decoder behind LiteStreamReader and LiteSession: it keeps no record of its own and writes into
// the LiteStreamRecord it is handed, so the session can hold the records contiguously. Never throws (allocation
// failure aside); every buffer is bounded by the DecodeLimits (one message at most) and dropped once a direction
// stops decoding. The message cap applies to each direction on its own (the runner's writes never use up the
// peer's allowance); hitting it stops that direction and sets message_limit_reached().
class LiteStreamDecoder {
public:
    LiteStreamDecoder(LiteStreamRecord& record, const wire::moqlite06::DecodeLimits& limits, std::size_t max_messages);

    void feed(LiteStreamRecord& record, std::span<const std::byte> data, bool fin, std::uint64_t at_ns,
              LiteBudget* budget = nullptr);
    void feed_local(LiteStreamRecord& record, std::span<const std::byte> data, bool fin, std::uint64_t at_ns,
                    LiteBudget* budget = nullptr);
    void reset(LiteStreamRecord& record, std::optional<std::uint64_t> code, std::uint64_t at_ns,
               LiteBudget* budget = nullptr);
    void stop_sending(LiteStreamRecord& record, std::optional<std::uint64_t> code, std::uint64_t at_ns,
                      LiteBudget* budget = nullptr);
    // A runner-side (harness) issue raised outside the byte stream, e.g. local_bidi_mismatch.
    void note_runner_issue(LiteStreamRecord& record, std::string_view code, std::string detail, std::uint64_t at_ns,
                           LiteBudget* budget = nullptr);
    [[nodiscard]] bool message_limit_reached() const noexcept { return message_limit_reached_; }

    enum class Phase { StreamType, First, Rest, Done, Raw, Stopped, AwaitKind };
    // Peer bytes held while the stream kind is unknown keep their arrival: absolute end offset, event, time.
    struct HeldChunk {
        std::size_t end;
        std::size_t event;
        std::uint64_t at_ns;
    };
    struct Direction {
        std::vector<std::byte> buffer;
        std::size_t start{0};
        std::size_t base{0};  // absolute stream offset of buffer[0]
        Phase phase{Phase::StreamType};
        bool opener{true};
        LiteOrigin from{LiteOrigin::Peer};
        bool fin{false};
        bool trailing_reported{false};
        std::size_t messages{0};  // decoded messages plus skipped unknown ANNOUNCE types, this direction
        std::vector<HeldChunk> held;
    };

private:
    std::size_t begin_event(LiteStreamRecord& record, std::uint64_t at_ns);
    void ingest(LiteStreamRecord& record, Direction& direction, std::span<const std::byte> data, bool fin,
                std::size_t event, std::uint64_t at_ns);
    void pump(LiteStreamRecord& record, Direction& direction, std::size_t event, std::uint64_t at_ns);
    void on_kind_known(LiteStreamRecord& record, std::size_t event);
    void refresh_pending(LiteStreamRecord& record) const;
    void finish(LiteStreamRecord& record, Direction& direction, std::size_t event);
    void issue(LiteStreamRecord& record, const Direction& direction, std::size_t event, std::string_view code,
               std::string detail);
    bool charge(std::size_t bytes, bool is_message);
    void stop(Direction& direction);
    static void stop_buffering(Direction& direction);

    wire::moqlite06::DecodeLimits limits_;
    std::size_t max_messages_;
    Direction inbound_;  // the peer's bytes
    Direction local_;    // the runner's bytes
    std::size_t events_{0};
    bool kind_final_{false};
    bool wake_inbound_{false};
    bool message_limit_reached_{false};
    LiteBudget* budget_{nullptr};  // set for the duration of one public call
};

}  // namespace detail

// A fresh record: kind Unknown, nothing seen.
LiteStreamRecord make_lite_stream_record(std::uint64_t stream_id, LiteOrigin origin, bool bidirectional);

// One per stream; feeds bytes and decodes them incrementally into record(). Never throws. max_messages caps each
// direction separately. A standalone reader has no session budget (LiteSession adds one).
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
