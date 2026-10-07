#include "moq/interop/session/lite_stream_reader.h"

#include <array>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::session {
namespace {

using wire::Cursor;
using wire::DecodeError;
using wire::DecodeErrorCode;
using wire::NeedMore;
using wire::moqlite06::BidiStreamType;
using wire::moqlite06::DecodeLimits;
using wire::moqlite06::UniStreamType;
using Phase = detail::LiteStreamDecoder::Phase;

// Slack over limits.max_message_length for the prefixes around a body (Type, Message Length, Timestamp Delta:
// three varints of at most 8 bytes). A buffer that grows past it without completing a message is a peer
// claiming more than the limit in a way no codec check caught; it is stopped rather than buffered further.
constexpr std::size_t kPrefixSlack = 32;

std::size_t buffer_cap(const DecodeLimits& limits) {
    const auto max = std::numeric_limits<std::size_t>::max();
    return limits.max_message_length > max - kPrefixSlack ? max : limits.max_message_length + kPrefixSlack;
}

std::string_view error_code_name(DecodeErrorCode code) {
    switch (code) {
        case DecodeErrorCode::InvalidValue: return kIssueInvalidValue;
        case DecodeErrorCode::ProtocolViolation: return kIssueProtocolViolation;
        case DecodeErrorCode::KeyValueFormattingError: return "key_value_formatting_error";
        case DecodeErrorCode::LengthExceedsLimit: return kIssueLengthExceedsLimit;
        case DecodeErrorCode::LengthNotRepresentable: return "length_not_representable";
        case DecodeErrorCode::OffsetOverflow: return kIssueOffsetOverflow;
    }
    return kIssueInvalidValue;
}

LiteStreamKind classify(std::uint64_t type, bool bidirectional) {
    if (bidirectional) {
        const auto bidi = wire::moqlite06::as_bidi_stream_type(type);
        if (!bidi) return LiteStreamKind::UnregisteredBidi;
        switch (*bidi) {
            case BidiStreamType::Announce: return LiteStreamKind::Announce;
            case BidiStreamType::Subscribe: return LiteStreamKind::Subscribe;
            case BidiStreamType::Fetch: return LiteStreamKind::Fetch;
            case BidiStreamType::Probe: return LiteStreamKind::Probe;
            case BidiStreamType::Goaway: return LiteStreamKind::Goaway;
            case BidiStreamType::Track: return LiteStreamKind::Track;
        }
        return LiteStreamKind::UnregisteredBidi;
    }
    const auto uni = wire::moqlite06::as_uni_stream_type(type);
    if (!uni) return LiteStreamKind::UnregisteredUni;
    return *uni == UniStreamType::Group ? LiteStreamKind::Group : LiteStreamKind::Setup;
}

bool is_l2(LiteStreamKind kind) {
    return kind == LiteStreamKind::Fetch || kind == LiteStreamKind::Probe || kind == LiteStreamKind::Goaway ||
           kind == LiteStreamKind::Track;
}

// Where decoding starts once the kind is known. The opener's bytes follow its STREAM_TYPE; the responder's bytes
// on a bidirectional stream carry no STREAM_TYPE.
Phase initial_phase(LiteStreamKind kind, bool opener) {
    if (opener) {
        switch (kind) {
            case LiteStreamKind::Setup:
            case LiteStreamKind::Group:
            case LiteStreamKind::Announce:
            case LiteStreamKind::Subscribe: return Phase::First;
            default: return Phase::Raw;
        }
    }
    switch (kind) {
        case LiteStreamKind::Announce: return Phase::First;   // ANNOUNCE_OK, then START/END/UPDATE
        case LiteStreamKind::Subscribe: return Phase::Rest;   // SUBSCRIBE_OK/END/DROP, any order
        default: return Phase::Raw;
    }
}

// One decode attempt on the pending bytes.
struct Next {
    enum class What { Message, Wait, Error, Skip, Unexpected };
    What what = What::Wait;
    std::optional<LiteMessage> message;
    DecodeError error{DecodeErrorCode::InvalidValue, 0, {}};
    std::string detail;     // Skip
    Phase next = Phase::Rest;
};

template <class T>
Next from_result(wire::DecodeResult<T>&& result, Phase next) {
    Next out;
    if (auto* value = std::get_if<T>(&result)) {
        out.what = Next::What::Message;
        if constexpr (std::is_constructible_v<LiteMessage, T>) {
            out.message.emplace(std::move(*value));
        } else {
            out.message.emplace(std::visit([](auto&& alternative) { return LiteMessage{std::move(alternative)}; },
                                           std::move(*value)));
        }
        out.next = next;
    } else if (std::holds_alternative<NeedMore>(result)) {
        out.what = Next::What::Wait;  // NeedMore always means wait; `required` is never relied on
    } else {
        out.what = Next::What::Error;
        out.error = std::get<DecodeError>(std::move(result));
    }
    return out;
}

Next wait() { return {}; }

Next error_of(DecodeError error) {
    Next out;
    out.what = Next::What::Error;
    out.error = std::move(error);
    return out;
}

// The responder's message on an Announce stream after ANNOUNCE_OK. Decision (a): an unknown Type is skipped by
// its Message Length (as moq.dev does) and reported; decoding continues.
Next announce_message(Cursor& input, const DecodeLimits& limits) {
    Cursor probe = input;
    auto type = wire::moqlite06::read_varint(probe);
    if (std::holds_alternative<NeedMore>(type)) return wait();
    if (auto* error = std::get_if<DecodeError>(&type)) return error_of(std::move(*error));
    const auto value = std::get<std::uint64_t>(type);
    if (value <= wire::moqlite06::kAnnounceTypeUpdate) {
        return from_result(wire::moqlite06::decode_announce_message(input, limits), Phase::Rest);
    }
    auto body = wire::moqlite06::read_framed_body(probe, limits);
    if (std::holds_alternative<NeedMore>(body)) return wait();
    if (auto* error = std::get_if<DecodeError>(&body)) return error_of(std::move(*error));
    Next out;
    out.what = Next::What::Skip;
    out.detail = "type=" + std::to_string(value) + " length=" +
                 std::to_string(std::get<std::span<const std::byte>>(body).size());
    out.next = Phase::Rest;
    input = probe;
    return out;
}

Next decode_next(LiteStreamKind kind, bool opener, Phase phase, Cursor& input, const DecodeLimits& limits) {
    using namespace wire::moqlite06;
    switch (kind) {
        case LiteStreamKind::Setup:
            if (opener && phase == Phase::First) return from_result(decode_setup(input, limits), Phase::Done);
            break;
        case LiteStreamKind::Group:
            if (!opener) break;
            if (phase == Phase::First) return from_result(decode_group_header(input, limits), Phase::Rest);
            return from_result(decode_frame(input, limits), Phase::Rest);
        case LiteStreamKind::Announce:
            if (opener) {
                if (phase == Phase::First) return from_result(decode_announce_request(input, limits), Phase::Done);
                break;
            }
            if (phase == Phase::First) return from_result(decode_announce_ok(input, limits), Phase::Rest);
            return announce_message(input, limits);
        case LiteStreamKind::Subscribe:
            if (opener) {
                if (phase == Phase::First) return from_result(decode_subscribe(input, limits), Phase::Rest);
                return from_result(decode_subscribe_update(input, limits), Phase::Rest);
            }
            return from_result(decode_subscribe_response(input, limits), Phase::Rest);
        default: break;
    }
    Next out;
    out.what = Next::What::Unexpected;
    return out;
}

std::string_view trailing_code(LiteStreamKind kind, bool opener) {
    if (kind == LiteStreamKind::Setup) return kIssueTrailingAfterSetup;
    if (kind == LiteStreamKind::Announce && opener) return kIssueTrailingAfterRequest;
    return kIssueProtocolViolation;
}

}  // namespace

std::string_view to_string(LiteStreamKind kind) {
    switch (kind) {
        case LiteStreamKind::Unknown: return "unknown";
        case LiteStreamKind::Setup: return "setup";
        case LiteStreamKind::Group: return "group";
        case LiteStreamKind::Announce: return "announce";
        case LiteStreamKind::Subscribe: return "subscribe";
        case LiteStreamKind::Fetch: return "fetch";
        case LiteStreamKind::Probe: return "probe";
        case LiteStreamKind::Goaway: return "goaway";
        case LiteStreamKind::Track: return "track";
        case LiteStreamKind::UnregisteredBidi: return "unregistered_bidi";
        case LiteStreamKind::UnregisteredUni: return "unregistered_uni";
    }
    return "unknown";
}

std::string_view lite_message_name(const LiteMessage& message) {
    static constexpr std::array<std::string_view, std::variant_size_v<LiteMessage>> kNames{
        "SETUP", "GROUP", "FRAME", "ANNOUNCE_REQUEST", "ANNOUNCE_OK", "ANNOUNCE_START", "ANNOUNCE_END",
        "ANNOUNCE_UPDATE", "SUBSCRIBE", "SUBSCRIBE_UPDATE", "SUBSCRIBE_OK", "SUBSCRIBE_END", "SUBSCRIBE_DROP"};
    return message.index() < kNames.size() ? kNames[message.index()] : std::string_view{};
}

namespace detail {

LiteStreamDecoder::LiteStreamDecoder(LiteStreamRecord& record, const DecodeLimits& limits, std::size_t max_messages)
    : limits_(limits), max_messages_(max_messages) {
    inbound_.from = LiteOrigin::Peer;
    local_.from = LiteOrigin::Runner;
    if (record.origin == LiteOrigin::Peer) {
        inbound_.opener = true;
        inbound_.phase = Phase::StreamType;
        local_.opener = false;
        local_.phase = Phase::Raw;  // the runner answering on a peer-opened stream is not decoded
        if (record.bidirectional) {
            issue(record, inbound_, 0, kIssuePublisherOpenedBidi,
                  "the publisher opened bidirectional stream " + std::to_string(record.stream_id));
        }
    } else {
        local_.opener = true;
        local_.phase = Phase::StreamType;
        inbound_.opener = false;
        inbound_.phase = Phase::AwaitKind;
    }
}

std::size_t LiteStreamDecoder::begin_event(LiteStreamRecord& record, std::uint64_t at_ns) {
    if (events_ == 0) record.opened_ns = at_ns;
    return events_++;
}

void LiteStreamDecoder::feed(LiteStreamRecord& record, std::span<const std::byte> data, bool fin,
                             std::uint64_t at_ns) {
    const auto event = begin_event(record, at_ns);
    record.bytes += data.size();
    if (record.origin == LiteOrigin::Runner && !kind_final_ && record.local_bytes == 0 &&
        inbound_.phase == Phase::AwaitKind) {
        // The peer wrote on a runner-initiated stream the runner never declared (note_local_write).
        issue(record, inbound_, event, kIssueUndeclaredRunnerStream,
              "peer bytes on runner stream " + std::to_string(record.stream_id) + " with no local write");
        kind_final_ = true;
        stop(inbound_);
    }
    if (fin) record.fin_seen = true;
    ingest(record, inbound_, data, fin, event, at_ns);
}

void LiteStreamDecoder::feed_local(LiteStreamRecord& record, std::span<const std::byte> data, bool fin,
                                   std::uint64_t at_ns) {
    const auto event = begin_event(record, at_ns);
    record.local_bytes += data.size();
    if (fin) record.local_fin = true;
    ingest(record, local_, data, fin, event, at_ns);
    if (wake_inbound_) {
        wake_inbound_ = false;
        pump(record, inbound_, event, at_ns);
        if (inbound_.fin) finish(record, inbound_, event);
    }
}

void LiteStreamDecoder::reset(LiteStreamRecord& record, std::optional<std::uint64_t> code, std::uint64_t at_ns) {
    begin_event(record, at_ns);
    record.reset_seen = true;
    if (code && !record.reset_code) record.reset_code = code;
    // A reset is not a decode error: what was decoded stays, a partial message is dropped silently.
    stop(inbound_);
}

void LiteStreamDecoder::stop_sending(LiteStreamRecord& record, std::optional<std::uint64_t> code,
                                     std::uint64_t at_ns) {
    begin_event(record, at_ns);
    record.stop_sending_seen = true;
    if (code && !record.stop_sending_code) record.stop_sending_code = code;
}

void LiteStreamDecoder::ingest(LiteStreamRecord& record, Direction& direction, std::span<const std::byte> data,
                               bool fin, std::size_t event, std::uint64_t at_ns) {
    if (direction.fin) {
        if (!data.empty() && !direction.trailing_reported) {
            direction.trailing_reported = true;
            issue(record, direction, event, kIssueTrailingAfterFin,
                  std::to_string(data.size()) + " bytes after FIN");
        }
        return;
    }
    if (direction.phase != Phase::Raw && direction.phase != Phase::Stopped) {
        direction.buffer.insert(direction.buffer.end(), data.begin(), data.end());
        pump(record, direction, event, at_ns);
    }
    if (fin) {
        direction.fin = true;
        finish(record, direction, event);
    }
}

void LiteStreamDecoder::pump(LiteStreamRecord& record, Direction& direction, std::size_t event,
                             std::uint64_t at_ns) {
    while (true) {
        if (direction.phase == Phase::Raw || direction.phase == Phase::Stopped) {
            stop_buffering(direction);
            return;
        }
        const std::span<const std::byte> pending =
            std::span<const std::byte>(direction.buffer).subspan(direction.start);
        if (direction.phase == Phase::AwaitKind) {
            if (pending.size() > buffer_cap(limits_)) {
                issue(record, direction, event, kIssueLengthExceedsLimit,
                      std::to_string(pending.size()) + " bytes buffered before the stream kind was known");
                stop(direction);
            }
            return;
        }
        if (pending.empty()) {
            direction.buffer.clear();
            direction.start = 0;
            return;
        }
        if (direction.phase == Phase::Done) {
            issue(record, direction, event, trailing_code(record.kind, direction.opener),
                  std::to_string(pending.size()) + " bytes after the stream's only message");
            stop(direction);
            return;
        }
        Cursor cursor(pending);
        if (direction.phase == Phase::StreamType) {
            auto type = wire::moqlite06::read_stream_type(cursor);
            if (std::holds_alternative<NeedMore>(type)) return;
            if (auto* error = std::get_if<DecodeError>(&type)) {
                issue(record, direction, event, error_code_name(error->code), "STREAM_TYPE: " + error->detail);
                stop(direction);
                return;
            }
            direction.start += cursor.offset();
            const auto value = std::get<std::uint64_t>(type);
            record.stream_type = value;
            if (record.origin == LiteOrigin::Peer && record.bidirectional) {
                // Recorded as Unknown (publisher_opened_bidi was raised when the stream was created).
                kind_final_ = true;
                direction.phase = Phase::Raw;
                continue;
            }
            record.kind = classify(value, record.bidirectional);
            direction.phase = initial_phase(record.kind, true);
            on_kind_known(record, event);
            continue;
        }
        if (messages_and_skips_ >= max_messages_) {
            message_limit_reached_ = true;
            issue(record, direction, event, kIssueMessageLimitReached,
                  "more than " + std::to_string(max_messages_) + " messages on one stream");
            stop(inbound_);
            stop(local_);
            return;
        }
        auto next = decode_next(record.kind, direction.opener, direction.phase, cursor, limits_);
        switch (next.what) {
            case Next::What::Wait:
                if (pending.size() > buffer_cap(limits_)) {
                    issue(record, direction, event, kIssueLengthExceedsLimit,
                          std::to_string(pending.size()) + " bytes buffered without a complete message");
                    stop(direction);
                } else if (direction.start > 4096 && direction.start * 2 > direction.buffer.size()) {
                    direction.buffer.erase(direction.buffer.begin(),
                                           direction.buffer.begin() + static_cast<std::ptrdiff_t>(direction.start));
                    direction.start = 0;
                }
                return;
            case Next::What::Error:
                issue(record, direction, event, error_code_name(next.error.code), next.error.detail);
                stop(direction);
                return;
            case Next::What::Unexpected:
                issue(record, direction, event, kIssueProtocolViolation, "bytes where no message is expected");
                stop(direction);
                return;
            case Next::What::Skip:
                ++messages_and_skips_;
                issue(record, direction, event, kIssueUnknownAnnounceType, std::move(next.detail));
                break;
            case Next::What::Message:
                ++messages_and_skips_;
                record.messages.push_back(LiteDecoded{event, std::move(*next.message), at_ns, direction.from});
                break;
        }
        direction.start += cursor.offset();
        direction.phase = next.next;
    }
}

void LiteStreamDecoder::on_kind_known(LiteStreamRecord& record, std::size_t event) {
    if (kind_final_) return;  // an undeclared runner stream keeps its raw inbound handling
    kind_final_ = true;
    if (is_l2(record.kind)) {
        issue(record, local_, event, kIssueL2StreamNotDecoded,
              std::string(to_string(record.kind)) + " streams are L2 and recorded raw");
    }
    if (inbound_.phase == Phase::AwaitKind) {
        // The peer's buffered bytes are decoded once the runner's write is (see feed_local), so the runner's
        // request precedes the peer's response in the record.
        inbound_.phase = initial_phase(record.kind, false);
        wake_inbound_ = true;
    }
}

void LiteStreamDecoder::finish(LiteStreamRecord& record, Direction& direction, std::size_t event) {
    const auto pending = direction.buffer.size() - direction.start;
    switch (direction.phase) {
        case Phase::AwaitKind: return;  // judged once the kind is known
        case Phase::StreamType:
            issue(record, direction, event, kIssueTruncatedAtFin,
                  pending == 0 ? "FIN before any byte" : "FIN inside the STREAM_TYPE");
            break;
        case Phase::First:
        case Phase::Rest:
            if (pending != 0) {
                issue(record, direction, event, kIssueTruncatedAtFin,
                      "FIN with " + std::to_string(pending) + " bytes of an incomplete message");
            } else if (direction.phase == Phase::First && direction.opener &&
                       (record.kind == LiteStreamKind::Setup || record.kind == LiteStreamKind::Group)) {
                issue(record, direction, event, kIssueTruncatedAtFin,
                      record.kind == LiteStreamKind::Setup ? "Setup stream ended without SETUP"
                                                           : "Group stream ended without GROUP");
            }
            break;
        case Phase::Done:
        case Phase::Raw:
        case Phase::Stopped: break;
    }
    stop(direction);
}

void LiteStreamDecoder::issue(LiteStreamRecord& record, const Direction& direction, std::size_t event,
                              std::string_view code, std::string detail) {
    record.issues.push_back(LiteDecodeIssue{event, std::string(code), std::move(detail), direction.from});
}

void LiteStreamDecoder::stop(Direction& direction) {
    direction.phase = Phase::Stopped;
    stop_buffering(direction);
}

void LiteStreamDecoder::stop_buffering(Direction& direction) {
    direction.buffer.clear();
    direction.buffer.shrink_to_fit();
    direction.start = 0;
}

}  // namespace detail

LiteStreamRecord make_lite_stream_record(std::uint64_t stream_id, LiteOrigin origin, bool bidirectional) {
    LiteStreamRecord record{};
    record.stream_id = stream_id;
    record.origin = origin;
    record.bidirectional = bidirectional;
    record.kind = LiteStreamKind::Unknown;
    return record;
}

LiteStreamReader::LiteStreamReader(std::uint64_t stream_id, LiteOrigin origin, bool bidirectional,
                                   const DecodeLimits& limits, std::size_t max_messages)
    : record_(make_lite_stream_record(stream_id, origin, bidirectional)), decoder_(record_, limits, max_messages) {}

void LiteStreamReader::feed(std::span<const std::byte> data, bool fin, std::uint64_t at_ns) {
    last_ns_ = at_ns;
    decoder_.feed(record_, data, fin, at_ns);
}

void LiteStreamReader::feed_local(std::span<const std::byte> data, bool fin, std::uint64_t at_ns) {
    last_ns_ = at_ns;
    decoder_.feed_local(record_, data, fin, at_ns);
}

void LiteStreamReader::reset(std::uint64_t code) { decoder_.reset(record_, code, last_ns_); }

void LiteStreamReader::stop_sending(std::uint64_t code) { decoder_.stop_sending(record_, code, last_ns_); }

}  // namespace moq::interop::session
