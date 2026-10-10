#pragma once

// A scripted moq-lite-06 PUBLISHER CLIENT for exercising the lite probe engine in process: the real
// LiteProbeController drives this fake transport, and a Reaction answers what the runner wrote. Unlike
// tests/support/scripted_publisher.h (MoQ Transport shaped) nothing is injected on its own except the
// connection-established event: the publisher's Setup stream, announce and subscribe answers, group streams,
// resets and closes all come from the Reaction (ConformingLitePublisher below is the ready-made one).
//
// Stream ids follow QUIC numbering with the runner as the server: runner bidi 1,5,9..., runner uni 3,7,11...,
// publisher bidi 0,4,8..., publisher uni 2,6,10....

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/group.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::test::lite {

using Bytes = std::vector<std::byte>;
namespace l06 = wire::moqlite06;

inline Bytes bytes_of(std::string_view text) {
    Bytes result;
    for (const auto c : text) result.push_back(static_cast<std::byte>(c));
    return result;
}

inline Bytes join(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

template <class Encode>
Bytes encode_with(Encode&& encode) {
    wire::ByteWriter output(std::size_t{1} << 21);
    if (!encode(output)) throw std::invalid_argument("unencodable lite test message");
    return {output.bytes().begin(), output.bytes().end()};
}

inline Bytes stream_type(std::uint64_t type) {
    return encode_with([&](wire::ByteWriter& out) { return l06::write_stream_type(type, out); });
}
inline Bytes setup(const l06::SetupMessage& message) {
    return encode_with([&](wire::ByteWriter& out) { return !l06::encode_setup(message, out).has_value(); });
}
inline Bytes announce_ok(const l06::AnnounceOk& message) {
    return encode_with([&](wire::ByteWriter& out) { return !l06::encode_announce_ok(message, out).has_value(); });
}
inline Bytes announce_message(const l06::AnnounceMessage& message) {
    return encode_with([&](wire::ByteWriter& out) { return !l06::encode_announce_message(message, out).has_value(); });
}
inline Bytes subscribe_response(const l06::SubscribeResponse& message) {
    return encode_with([&](wire::ByteWriter& out) { return !l06::encode_subscribe_response(message, out).has_value(); });
}
inline Bytes group_header(const l06::GroupHeader& header) {
    return encode_with([&](wire::ByteWriter& out) { return !l06::encode_group_header(header, out).has_value(); });
}
inline Bytes frame(const l06::Frame& value) {
    return encode_with([&](wire::ByteWriter& out) { return !l06::encode_frame(value, out).has_value(); });
}
// STREAM_TYPE 0x1 + SETUP: a whole Setup stream.
inline Bytes setup_stream(const l06::SetupMessage& message = {}) { return join({stream_type(0x1), setup(message)}); }

// What the runner did to one stream.
struct RunnerStream {
    Bytes bytes;
    bool fin{false};
    std::size_t writes{0};
    std::optional<std::uint64_t> reset_code;
    std::optional<std::uint64_t> stop_sending_code;
};

struct RunnerCall {
    enum class Kind { OpenBidi, OpenUni, Write, Reset, StopSending, Close } kind;
    transport::StreamId stream{0};
    std::uint64_t code{0};
    std::size_t bytes{0};
    bool fin{false};
};

struct RunnerClose {
    std::uint64_t code{0};
    std::string reason;
};

class ScriptedLitePeer final : public transport::SessionTransport {
public:
    using Reaction = std::function<void(ScriptedLitePeer&)>;

    explicit ScriptedLitePeer(Reaction reaction = {}, std::string alpn = std::string(scenarios::kLiteAlpn))
        : reaction_(std::move(reaction)), alpn_(std::move(alpn)) {}

    // --- configuration ---
    // The connection-established event is emitted on this poll (0: the first). nullopt: never connects.
    std::optional<std::size_t> establish_on_poll{0};
    // Writes accept at most this many bytes per call (Partial beyond), to exercise partial writes.
    std::size_t max_write_chunk{std::numeric_limits<std::size_t>::max()};
    // Forced statuses for runner calls on a stream (write, reset, stop_sending).
    std::map<transport::StreamId, transport::TransportStatus> forced_status;
    // How many runner write calls return WouldBlock (accepting nothing) before writes proceed.
    std::size_t would_block_writes{0};

    // --- SessionTransport ---
    transport::OpenResult open_bidi() override {
        if (closed()) return {transport::TransportStatus::ConnectionClosed, 0};
        const auto id = next_runner_bidi_;
        next_runner_bidi_ += 4;
        runner_[id];
        calls_.push_back({RunnerCall::Kind::OpenBidi, id});
        return {transport::TransportStatus::Success, id};
    }
    transport::OpenResult open_uni() override {
        if (closed()) return {transport::TransportStatus::ConnectionClosed, 0};
        const auto id = next_runner_uni_;
        next_runner_uni_ += 4;
        runner_[id];
        calls_.push_back({RunnerCall::Kind::OpenUni, id});
        return {transport::TransportStatus::Success, id};
    }
    transport::OperationResult write(transport::StreamId id, std::span<const std::byte> bytes, bool fin) override {
        if (closed()) return {transport::TransportStatus::ConnectionClosed, 0, {}};
        if (publisher_uni(id)) return {transport::TransportStatus::InvalidState, 0, {}};  // receive-only for us
        if (const auto forced = forced_status.find(id); forced != forced_status.end())
            return {forced->second, 0, {}};
        if (would_block_writes > 0) {
            --would_block_writes;
            return {transport::TransportStatus::WouldBlock, 0, {}};
        }
        const auto take = std::min(bytes.size(), max_write_chunk);
        auto& stream = runner_[id];
        stream.bytes.insert(stream.bytes.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(take));
        const bool all = take == bytes.size();
        stream.fin = stream.fin || (fin && all);
        ++stream.writes;
        calls_.push_back({RunnerCall::Kind::Write, id, 0, take, fin && all});
        return {all ? transport::TransportStatus::Success : transport::TransportStatus::Partial, take, {}};
    }
    transport::OperationResult reset(transport::StreamId id, std::uint64_t code) override {
        if (closed()) return {transport::TransportStatus::ConnectionClosed, 0, {}};
        if (publisher_uni(id)) return {transport::TransportStatus::InvalidState, 0, {}};  // no send side to reset
        if (const auto forced = forced_status.find(id); forced != forced_status.end())
            return {forced->second, 0, {}};
        runner_[id].reset_code = code;
        calls_.push_back({RunnerCall::Kind::Reset, id, code});
        return {transport::TransportStatus::Success, 0, {}};
    }
    transport::OperationResult stop_sending(transport::StreamId id, std::uint64_t code) override {
        if (closed()) return {transport::TransportStatus::ConnectionClosed, 0, {}};
        if (runner_uni(id)) return {transport::TransportStatus::InvalidState, 0, {}};  // no receive side to stop
        if (const auto forced = forced_status.find(id); forced != forced_status.end())
            return {forced->second, 0, {}};
        runner_[id].stop_sending_code = code;
        calls_.push_back({RunnerCall::Kind::StopSending, id, code});
        return {transport::TransportStatus::Success, 0, {}};
    }
    transport::OperationResult send_datagram(std::span<const std::byte>) override {
        return {transport::TransportStatus::InvalidState, 0, {}};
    }
    transport::OperationResult close(std::uint64_t code, std::span<const std::byte> reason) override {
        if (closed()) return {transport::TransportStatus::ConnectionClosed, 0, {}};
        runner_close_ = RunnerClose{code, std::string(reinterpret_cast<const char*>(reason.data()), reason.size())};
        calls_.push_back({RunnerCall::Kind::Close, 0, code});
        events_.push_back(transport::LocalCloseEvent{transport::CloseErrorSpace::Application, code,
                                                     Bytes(reason.begin(), reason.end())});
        return {transport::TransportStatus::Success, 0, {}};
    }
    std::vector<transport::TransportEvent> poll(std::size_t max_events) override {
        if (!established_ && establish_on_poll && polls_ >= *establish_on_poll) {
            established_ = true;
            transport::ConnectionEstablishedEvent event;
            event.alpn = bytes_of(alpn_);
            events_.push_back(std::move(event));
        }
        ++polls_;
        if (established_ && reaction_ && !peer_closed_ && !runner_close_) reaction_(*this);
        std::vector<transport::TransportEvent> result;
        while (!events_.empty() && result.size() < max_events) {
            result.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        return result;
    }

    // --- publisher-to-runner events ---
    transport::StreamId open_peer_uni() {
        const auto id = next_peer_uni_;
        next_peer_uni_ += 4;
        return id;
    }
    transport::StreamId open_peer_bidi() {
        const auto id = next_peer_bidi_;
        next_peer_bidi_ += 4;
        return id;
    }
    void data(transport::StreamId id, Bytes bytes, bool fin = false) {
        events_.push_back(transport::StreamDataEvent{id, std::move(bytes), fin});
    }
    void fin(transport::StreamId id) { data(id, {}, true); }
    void peer_reset(transport::StreamId id, std::optional<std::uint64_t> code) {
        events_.push_back(transport::PeerResetEvent{id, code});
    }
    void peer_stop_sending(transport::StreamId id, std::optional<std::uint64_t> code) {
        events_.push_back(transport::PeerStopSendingEvent{id, code});
    }
    void close_session(std::uint64_t code, std::string reason = {},
                       transport::CloseErrorSpace space = transport::CloseErrorSpace::Application) {
        peer_closed_ = true;
        events_.push_back(transport::PeerCloseEvent{space, code, bytes_of(reason)});
    }
    void raw_event(transport::TransportEvent event) { events_.push_back(std::move(event)); }

    // --- what the runner did ---
    [[nodiscard]] const RunnerStream* runner_stream(transport::StreamId id) const {
        const auto found = runner_.find(id);
        return found == runner_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] const std::map<transport::StreamId, RunnerStream>& runner_streams() const { return runner_; }
    [[nodiscard]] const std::vector<RunnerCall>& calls() const { return calls_; }
    [[nodiscard]] const std::optional<RunnerClose>& runner_close() const { return runner_close_; }
    [[nodiscard]] std::size_t polls() const { return polls_; }
    [[nodiscard]] bool established() const { return established_; }
    [[nodiscard]] bool peer_closed() const { return peer_closed_; }

private:
    [[nodiscard]] bool closed() const { return peer_closed_ || runner_close_.has_value(); }
    // Like a real transport: the runner cannot write or reset a stream the publisher opened unidirectionally, nor
    // STOP_SENDING one it opened unidirectionally itself (InvalidState).
    static bool publisher_uni(transport::StreamId id) { return (id & 3u) == 2u; }
    static bool runner_uni(transport::StreamId id) { return (id & 3u) == 3u; }

    Reaction reaction_;
    std::string alpn_;
    bool established_{false};
    bool peer_closed_{false};
    std::size_t polls_{0};
    std::uint64_t next_runner_bidi_{1};
    std::uint64_t next_runner_uni_{3};
    std::uint64_t next_peer_bidi_{0};
    std::uint64_t next_peer_uni_{2};
    std::map<transport::StreamId, RunnerStream> runner_;
    std::vector<RunnerCall> calls_;
    std::optional<RunnerClose> runner_close_;
    std::deque<transport::TransportEvent> events_;
};

// --- ConformingLitePublisher ---------------------------------------------------------------------------------------

// Named single defects. Tasks 4-7 add enumerators here (and their handling in ConformingLitePublisher), or put a
// scenario-local defect in a test file through LitePublisherHooks.
enum class LiteDefect {
    None,
    NoSetupStream,         // never opens its own Setup stream
    SilentOnAnnounce,      // never answers an ANNOUNCE_REQUEST
    IgnoreUnknownStreams,  // neither resets nor stops a runner stream of an unknown type (draft 7.2)
    CloseOnInvalidSubscribe,  // closes the session (PROTOCOL_VIOLATION) on an undecodable SUBSCRIBE (draft 3.6)
    OffsetGroupStart,         // reads a SUBSCRIBE Group Start offset by one: starts at max(latest, Group Start - 1)
};

// A request the runner opened, decoded far enough to answer.
struct LiteRunnerRequest {
    transport::StreamId stream{0};
    bool bidirectional{false};
    std::uint64_t stream_type{0};
    // monostate: a stream type the publisher does not serve (unknown, or Fetch/Probe/Goaway/Track).
    std::variant<std::monostate, l06::SetupMessage, l06::AnnounceRequest, l06::Subscribe> message;
    std::size_t setup_streams_seen{0};  // runner Setup streams so far, this one included
};

class ConformingLitePublisher;
// Extension point for scenario tests: each hook returns true when it handled the situation itself (the default
// behavior is then skipped). on_poll runs at the end of every reaction.
struct LitePublisherHooks {
    std::function<bool(ConformingLitePublisher&, ScriptedLitePeer&)> on_start;
    std::function<bool(ConformingLitePublisher&, ScriptedLitePeer&, const LiteRunnerRequest&)> on_request;
    std::function<void(ConformingLitePublisher&, ScriptedLitePeer&)> on_poll;
};

struct ConformingLitePublisherConfig {
    std::string broadcast{"demo/live"};
    std::string track{"video"};
    std::vector<l06::SetupParameter> setup_parameters{};
    std::uint64_t hop_id{7};
    std::uint64_t latest_group{5};
    std::size_t groups_per_subscription{2};
    std::size_t frames_per_group{2};
    // 0: each FRAME payload is the text "frame-<group>-<frame>"; otherwise that text padded to this many bytes (a
    // media-sized source for the evidence-cap tests).
    std::size_t frame_payload_bytes{0};
    // Code for resetting / stopping a runner stream of an unknown or unserved type.
    std::uint64_t unknown_stream_code{0x0};
    // Stream error code for a SUBSCRIBE naming something the publisher does not have (NOT_FOUND).
    std::uint64_t not_found_code{0x33};
    // Session error code for a violation of the session rules (PROTOCOL_VIOLATION).
    std::uint64_t protocol_violation_code{0x3};
    // Stream error code for resetting a SUBSCRIBE whose only defect is Frame End without Group End (INTERNAL_ERROR:
    // the stream table has no protocol-violation code).
    std::uint64_t invalid_subscribe_code{0x0};
    // Stream error code for ending a subscription, or a Group stream, the runner cancelled (CANCELLED).
    std::uint64_t cancelled_code{0x1};
    // After the `groups_per_subscription` whole groups of an unbounded subscription, one more Group stream is opened
    // and left open (GROUP + frames, no FIN): the group still being produced.
    bool keep_live_group_open{true};
    // Draft 4.3: close the send direction (FIN) of a stream this publisher answered once the runner closed (FIN) its
    // own. A test whose hooks script the reaction to the runner's FIN themselves (the row 025 tests) sets it false.
    bool echo_runner_fin{true};
    // The session URL the adapter gave this publisher, and the binding (draft 7.3.2): on native QUIC the SETUP
    // carries Path = path + "?" + query (no '?' when the query is empty); on WebTransport (or Unknown) no Path.
    std::string session_url_path{};
    std::string session_url_query{};
    scenarios::LiteBinding binding{scenarios::LiteBinding::Unknown};
    LiteDefect defect{LiteDefect::None};
    LitePublisherHooks hooks{};
};

// Replies per the draft, so that every moq-lite-06 scenario judges it conforming (the end-to-end conformance table,
// tests/protocol/lite_conformance_test.cpp):
//   - its own Setup stream first (config `setup_parameters`, plus Path on native QUIC from the session URL);
//   - ANNOUNCE_OK (Hop ID, Active Count) then one ANNOUNCE_START for the configured broadcast when the prefix
//     covers it;
//   - SUBSCRIBE_OK then Group streams for the configured broadcast/track, one per poll: Group Start read raw
//     (start = max(latest_group, Group Start), draft 3.6), Frame Start applied only when the start resolves at the
//     Group Start group (else the GROUP's Frame Start is 0), Group End / Frame End honored (the last group stops at
//     Frame End); an unbounded subscription gets `groups_per_subscription` whole groups (FIN) and then, with
//     `keep_live_group_open`, one more Group stream left open;
//   - a later SUBSCRIBE is answered the same way (SUBSCRIBE_OK), whatever was cancelled before it;
//   - NOT_FOUND reset (+ STOP_SENDING) for other broadcasts or tracks; a SUBSCRIBE whose only decode failure is the
//     bounds rule (Frame End without Group End, draft 3.6) is reset with `invalid_subscribe_code`, the session stays
//     open; any other undecodable SUBSCRIBE closes the session (below);
//   - the runner resetting or stopping a Subscribe stream this publisher serves ends that subscription: RESET_STREAM
//     CANCELLED on the Subscribe stream and on its open Group streams, no further groups; a STOP_SENDING on an open
//     Group stream resets only that stream (CANCELLED); the runner resetting or stopping an announce stream this
//     publisher answered is answered by resetting its send direction (draft 4.3);
//   - the runner closing (FIN) the send direction of a stream this publisher answered is answered by closing its own
//     (FIN; a served subscription's open Group streams are reset first), unless `echo_runner_fin` is false (draft
//     4.3);
//   - resets (bidi) or stops (uni) runner streams of unknown or unserved types;
//   - a second runner Setup stream, a malformed SETUP (a repeated Parameter ID included), a runner SETUP carrying
//     Path or Role (client-only, draft 7.3.2/7.3.3) or another malformed request (an undecodable SUBSCRIBE other
//     than the bounds rule included, draft 7.1) closes the session with PROTOCOL_VIOLATION; unknown Parameter IDs
//     are ignored (draft 7.3).
// Deterministic. The publisher must outlive the peer it reacts for.
//
// Defaults that are the implementer's choices, NOT draft rules (Tasks 4-7 set what their rows need):
//   - unknown_stream_code 0x0 for resetting/stopping unknown or unserved streams (the draft names no code);
//   - invalid_subscribe_code 0x0 (INTERNAL_ERROR) for the undecodable SUBSCRIBE (the stream table has no
//     protocol-violation code);
//   - the ANNOUNCE_START hop list is empty and both route costs are 0 (the publisher is the origin);
//   - prefix coverage is plain std::string::starts_with on the configured broadcast path (no segment rules);
//   - one Group stream per poll, so the group rate follows the test tick, not media time;
//   - Fetch, Probe, Goaway and Track streams (L2 kinds) are refused like unknown types;
//   - the Subscriber Max Age is not consulted: no history is held, every unfloored subscription starts at
//     latest_group;
//   - a bounded subscription ends with its last group (no SUBSCRIBE_END, the Subscribe stream stays open).
class ConformingLitePublisher {
public:
    explicit ConformingLitePublisher(ConformingLitePublisherConfig config = {}) : config_(std::move(config)) {}

    [[nodiscard]] ScriptedLitePeer::Reaction reaction() {
        return [this](ScriptedLitePeer& peer) { react(peer); };
    }
    [[nodiscard]] const ConformingLitePublisherConfig& config() const { return config_; }
    ConformingLitePublisherConfig& config() { return config_; }

    void react(ScriptedLitePeer& peer) {
        if (!started_) {
            started_ = true;
            if (!(config_.hooks.on_start && config_.hooks.on_start(*this, peer)) &&
                config_.defect != LiteDefect::NoSetupStream)
                send_setup(peer);
        }
        for (const auto& [id, stream] : peer.runner_streams()) {
            if (peer.peer_closed()) break;
            if (publisher_uni(id)) continue;  // a Group stream the runner stopped: handled below
            read_runner_stream(peer, id, stream);
        }
        if (!peer.peer_closed()) react_to_runner_endings(peer);
        if (!peer.peer_closed()) emit_one_group(peer);
        if (config_.hooks.on_poll) config_.hooks.on_poll(*this, peer);
    }

    // Default behaviors, public so hooks can reuse them.
    void send_setup(ScriptedLitePeer& peer) {
        l06::SetupMessage message{config_.setup_parameters};
        const bool has_path = std::any_of(message.parameters.begin(), message.parameters.end(),
                                          [](const l06::SetupParameter& p) { return p.id == l06::kParamPath; });
        if (config_.binding == scenarios::LiteBinding::NativeQuic && !config_.session_url_path.empty() && !has_path) {
            std::string value = config_.session_url_path;
            if (!config_.session_url_query.empty()) value += "?" + config_.session_url_query;
            message.parameters.insert(message.parameters.begin(),
                                      l06::SetupParameter{l06::kParamPath, bytes_of(value)});
        }
        peer.data(peer.open_peer_uni(), setup_stream(message), true);
    }
    void answer_announce(ScriptedLitePeer& peer, transport::StreamId stream, const l06::AnnounceRequest& request) {
        const bool covered = config_.broadcast.starts_with(request.prefix);
        Bytes reply = announce_ok({config_.hop_id, covered ? 1u : 0u});
        if (covered) {
            l06::AnnounceStart start;
            start.suffix = config_.broadcast.substr(request.prefix.size());
            const auto more = announce_message(start);
            reply.insert(reply.end(), more.begin(), more.end());
        }
        peer.data(stream, std::move(reply));
        answered_.insert(stream);
    }
    void answer_subscribe(ScriptedLitePeer& peer, transport::StreamId stream, const l06::Subscribe& subscribe) {
        if (subscribe.broadcast_path != config_.broadcast || subscribe.track_name != config_.track) {
            refuse(peer, stream, config_.not_found_code);
            return;
        }
        const auto& range = subscribe.range;
        std::uint64_t start = config_.latest_group;
        if (range.group_start > 0) {
            start = config_.defect == LiteDefect::OffsetGroupStart ? std::max(start, range.group_start - 1)
                                                                    : std::max(start, range.group_start);
        }
        // Draft 3.6: Frame Start qualifies only the Group Start group.
        const std::uint64_t first_frame = start == range.group_start ? range.frame_start : 0;
        peer.data(stream, subscribe_response(l06::SubscribeOk{start}));
        answered_.insert(stream);
        auto& served = subscriptions_[stream];
        served.subscribe_id = subscribe.subscribe_id;
        const auto plan = [&](std::uint64_t sequence, std::uint64_t total, bool fin) {
            const std::uint64_t from = sequence == start ? first_frame : 0;
            pending_groups_.push_back({stream, subscribe.subscribe_id, sequence, from, total > from ? total - from : 0,
                                       fin});
        };
        if (range.group_end != 0) {
            // Bounded: groups start .. Group End - 1, the last one cut at Frame End (when non-zero).
            for (std::uint64_t sequence = start; sequence + 1 <= range.group_end; ++sequence) {
                std::uint64_t total = config_.frames_per_group;
                if (sequence + 1 == range.group_end && range.frame_end != 0)
                    total = std::min<std::uint64_t>(total, range.frame_end);
                plan(sequence, total, true);
            }
            return;
        }
        for (std::size_t i = 0; i < config_.groups_per_subscription; ++i)
            plan(start + i, config_.frames_per_group, true);
        if (config_.keep_live_group_open)
            plan(start + config_.groups_per_subscription, config_.frames_per_group, false);
    }
    void refuse(ScriptedLitePeer& peer, transport::StreamId stream, std::uint64_t code) {
        if ((stream & 2u) == 0u) peer.peer_reset(stream, code);
        peer.peer_stop_sending(stream, code);
        cancelled_.insert(stream);
        send_ended_.insert(stream);
    }
    void close(ScriptedLitePeer& peer, std::uint64_t code, std::string reason = {}) {
        peer.close_session(code, std::move(reason));
    }

    [[nodiscard]] std::size_t runner_setups() const { return runner_setups_; }
    [[nodiscard]] const std::vector<LiteRunnerRequest>& requests() const { return requests_; }
    [[nodiscard]] std::size_t groups_sent() const { return groups_sent_; }

private:
    struct Parse {
        std::size_t offset{0};
        std::optional<std::uint64_t> type;
        bool request_done{false};
    };
    struct PendingGroup {
        transport::StreamId subscribe_stream;
        std::uint64_t subscribe_id;
        std::uint64_t sequence;
        std::uint64_t frame_start;
        std::uint64_t frames;
        bool fin;
    };
    // A subscription this publisher serves (answered by answer_subscribe).
    struct Served {
        std::uint64_t subscribe_id{0};
        std::set<transport::StreamId> open_groups;
    };

    static bool publisher_uni(transport::StreamId id) { return (id & 3u) == 2u; }

    void read_runner_stream(ScriptedLitePeer& peer, transport::StreamId id, const RunnerStream& stream) {
        if (stream.reset_code || stream.stop_sending_code) cancelled_.insert(id);
        auto& parse = parse_[id];
        if (parse.request_done) return;
        const std::span<const std::byte> pending = std::span<const std::byte>(stream.bytes).subspan(parse.offset);
        wire::Cursor cursor(pending);
        if (!parse.type) {
            const auto type = l06::read_stream_type(cursor);
            if (std::holds_alternative<wire::NeedMore>(type)) return;
            if (std::holds_alternative<wire::DecodeError>(type)) {
                parse.request_done = true;
                return;
            }
            parse.type = std::get<std::uint64_t>(type);
            parse.offset += cursor.offset();
            if ((id & 2u) != 0u && *parse.type == 0x1) ++runner_setups_;
        }
        const bool bidi = (id & 2u) == 0u;
        LiteRunnerRequest request{id, bidi, *parse.type, std::monostate{}, runner_setups_};
        wire::Cursor body(std::span<const std::byte>(stream.bytes).subspan(parse.offset));
        bool malformed = false;
        std::string error_detail;
        // False while the message is incomplete; true once decoded (into target) or malformed.
        const auto take = [&](auto result, auto& target) {
            if (std::holds_alternative<wire::NeedMore>(result)) return false;
            if (const auto* error = std::get_if<wire::DecodeError>(&result)) {
                malformed = true;
                error_detail = error->detail;
                return true;
            }
            target = std::move(std::get<0>(result));
            return true;
        };
        if (!bidi && *parse.type == 0x1) {
            l06::SetupMessage message;
            if (!take(l06::decode_setup(body), message)) return;
            if (!malformed) request.message = message;
        } else if (bidi && *parse.type == 0x1) {
            l06::AnnounceRequest message;
            if (!take(l06::decode_announce_request(body), message)) return;
            if (!malformed) request.message = message;
        } else if (bidi && *parse.type == 0x2) {
            l06::Subscribe message;
            if (!take(l06::decode_subscribe(body), message)) return;
            if (!malformed) request.message = message;
        }
        parse.request_done = true;
        requests_.push_back(request);
        if (config_.hooks.on_request && config_.hooks.on_request(*this, peer, request)) return;
        handle_request(peer, request, malformed, error_detail);
    }

    // The codec's detail for the one SUBSCRIBE decode failure that is a bounds violation (Frame End without Group
    // End, src/wire/moqlite06/subscribe.cpp check_range_coupling); every other decode failure is a malformed message.
    static constexpr std::string_view kSubscribeBoundsViolation = "frame end is set but group end is unbounded";

    void handle_request(ScriptedLitePeer& peer, const LiteRunnerRequest& request, bool malformed,
                        std::string_view error_detail) {
        if (!request.bidirectional && request.stream_type == 0x1) {
            if (malformed || request.setup_streams_seen > 1 || carries_client_only_parameter(request))
                close(peer, config_.protocol_violation_code);
            return;
        }
        if (malformed && request.bidirectional && request.stream_type == 0x2 &&
            error_detail == kSubscribeBoundsViolation && config_.defect != LiteDefect::CloseOnInvalidSubscribe) {
            // Draft 3.6: a SUBSCRIBE with Frame End but no Group End is refused by resetting its stream. Any other
            // undecodable SUBSCRIBE falls through to the PROTOCOL_VIOLATION close below (draft 7.1).
            refuse(peer, request.stream, config_.invalid_subscribe_code);
            return;
        }
        if (malformed) {
            close(peer, config_.protocol_violation_code);
            return;
        }
        if (const auto* announce = std::get_if<l06::AnnounceRequest>(&request.message)) {
            if (config_.defect != LiteDefect::SilentOnAnnounce) answer_announce(peer, request.stream, *announce);
            return;
        }
        if (const auto* subscribe = std::get_if<l06::Subscribe>(&request.message)) {
            answer_subscribe(peer, request.stream, *subscribe);
            return;
        }
        if (config_.defect == LiteDefect::IgnoreUnknownStreams) return;
        refuse(peer, request.stream, config_.unknown_stream_code);
    }

    // Draft 7.3.2 and 7.3.3: only the client sends Path and Role; this publisher is the client, so a runner
    // (server) SETUP carrying either is a PROTOCOL_VIOLATION.
    static bool carries_client_only_parameter(const LiteRunnerRequest& request) {
        const auto* setup = std::get_if<l06::SetupMessage>(&request.message);
        if (!setup) return false;
        for (const auto& parameter : setup->parameters)
            if (parameter.id == l06::kParamPath || parameter.id == l06::kParamRole) return true;
        return false;
    }

    // Ends a served subscription: its open Group streams reset (CANCELLED), no further groups.
    void end_groups(ScriptedLitePeer& peer, transport::StreamId subscribe_stream) {
        cancelled_.insert(subscribe_stream);
        const auto served = subscriptions_.find(subscribe_stream);
        if (served == subscriptions_.end()) return;
        for (const auto group : served->second.open_groups) peer.peer_reset(group, config_.cancelled_code);
        served->second.open_groups.clear();
    }

    // Draft 4.3: the runner ending a stream this publisher answered (or one of its open Group streams).
    void react_to_runner_endings(ScriptedLitePeer& peer) {
        const bool echo_fin = config_.echo_runner_fin;
        for (const auto& [id, stream] : peer.runner_streams()) {
            if (publisher_uni(id)) {
                // A STOP_SENDING on an open Group stream: reset only that stream.
                if (!stream.stop_sending_code) continue;
                for (auto& [subscribe_stream, served] : subscriptions_) {
                    if (served.open_groups.erase(id) > 0) peer.peer_reset(id, config_.cancelled_code);
                }
                continue;
            }
            if ((id & 2u) != 0u || !answered_.contains(id) || send_ended_.contains(id)) continue;
            if (stream.reset_code || stream.stop_sending_code) {
                send_ended_.insert(id);
                end_groups(peer, id);
                peer.peer_reset(id, config_.cancelled_code);
            } else if (stream.fin && echo_fin) {
                send_ended_.insert(id);
                end_groups(peer, id);
                peer.fin(id);
            }
        }
    }

    void emit_one_group(ScriptedLitePeer& peer) {
        while (!pending_groups_.empty()) {
            const auto group = pending_groups_.front();
            pending_groups_.pop_front();
            if (cancelled_.contains(group.subscribe_stream)) continue;
            Bytes bytes =
                join({stream_type(0x0), group_header({group.subscribe_id, group.sequence, group.frame_start})});
            for (std::uint64_t f = 0; f < group.frames; ++f) {
                l06::Frame value;
                value.timestamp_delta = f == 0 ? static_cast<std::int64_t>(group.sequence * 1000) : 33;
                value.payload = bytes_of("frame-" + std::to_string(group.sequence) + "-" +
                                         std::to_string(group.frame_start + f));
                if (value.payload.size() < config_.frame_payload_bytes)
                    value.payload.resize(config_.frame_payload_bytes, std::byte{0x2e});
                const auto encoded = frame(value);
                bytes.insert(bytes.end(), encoded.begin(), encoded.end());
            }
            const auto id = peer.open_peer_uni();
            peer.data(id, std::move(bytes), group.fin);
            if (!group.fin) subscriptions_[group.subscribe_stream].open_groups.insert(id);
            ++groups_sent_;
            return;
        }
    }

    ConformingLitePublisherConfig config_;
    bool started_{false};
    std::map<transport::StreamId, Parse> parse_;
    std::set<transport::StreamId> cancelled_;
    std::set<transport::StreamId> answered_;    // runner bidi streams this publisher answered itself
    std::set<transport::StreamId> send_ended_;  // runner bidi streams whose send direction this publisher ended
    std::map<transport::StreamId, Served> subscriptions_;
    std::deque<PendingGroup> pending_groups_;
    std::vector<LiteRunnerRequest> requests_;
    std::size_t runner_setups_{0};
    std::size_t groups_sent_{0};
};

// Runs a probe to completion on a manual clock advancing `tick` per poll (at most `max_polls` polls).
inline scenarios::LiteTranscript run_lite_probe(ScriptedLitePeer& peer, scenarios::LiteProbeDefinition definition,
                                               scenarios::ManualLiteClock& clock,
                                               std::chrono::milliseconds tick = std::chrono::milliseconds(1),
                                               std::size_t max_polls = 100000) {
    scenarios::LiteProbeController controller(std::move(definition), peer, clock);
    for (std::size_t i = 0; i < max_polls && controller.poll(); ++i) clock.advance(tick);
    return controller.transcript();
}

}  // namespace moq::interop::test::lite
