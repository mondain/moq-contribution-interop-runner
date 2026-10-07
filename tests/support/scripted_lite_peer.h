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
    // Code for resetting / stopping a runner stream of an unknown or unserved type.
    std::uint64_t unknown_stream_code{0x0};
    // Stream error code for a SUBSCRIBE naming something the publisher does not have (NOT_FOUND).
    std::uint64_t not_found_code{0x33};
    // Session error code for a violation of the session rules (PROTOCOL_VIOLATION).
    std::uint64_t protocol_violation_code{0x3};
    LiteDefect defect{LiteDefect::None};
    LitePublisherHooks hooks{};
};

// Replies per the draft: its own Setup stream first; ANNOUNCE_OK (Hop ID, Active Count) then one ANNOUNCE_START for
// the configured broadcast when the prefix covers it; SUBSCRIBE_OK (latest group) then one Group stream per poll,
// each GROUP plus frames and FIN, for the configured broadcast/track; NOT_FOUND reset for other tracks; resets (bidi)
// or stops (uni) runner streams of unknown or unserved types; a second runner Setup stream or a malformed SETUP
// closes the session with PROTOCOL_VIOLATION. Deterministic. The publisher must outlive the peer it reacts for.
//
// Defaults that are the implementer's choices, NOT draft rules (Tasks 4-7 set what their rows need):
//   - unknown_stream_code 0x0 for resetting/stopping unknown or unserved streams (the draft names no code);
//   - the ANNOUNCE_START hop list is empty and both route costs are 0 (the publisher is the origin);
//   - prefix coverage is plain std::string::starts_with on the configured broadcast path (no segment rules);
//   - one Group stream per poll, so the group rate follows the test tick, not media time;
//   - Fetch, Probe, Goaway and Track streams (L2 kinds) are refused like unknown types;
//   - a SUBSCRIBE floor (group_start > 0) starts at max(latest_group, group_start - 1); bounds are not checked.
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
            read_runner_stream(peer, id, stream);
        }
        emit_one_group(peer);
        if (config_.hooks.on_poll) config_.hooks.on_poll(*this, peer);
    }

    // Default behaviors, public so hooks can reuse them.
    void send_setup(ScriptedLitePeer& peer) {
        peer.data(peer.open_peer_uni(), setup_stream({config_.setup_parameters}), true);
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
    }
    void answer_subscribe(ScriptedLitePeer& peer, transport::StreamId stream, const l06::Subscribe& subscribe) {
        if (subscribe.broadcast_path != config_.broadcast || subscribe.track_name != config_.track) {
            refuse(peer, stream, config_.not_found_code);
            return;
        }
        std::uint64_t first = config_.latest_group;
        if (subscribe.range.group_start > 0) first = std::max(first, subscribe.range.group_start - 1);
        peer.data(stream, subscribe_response(l06::SubscribeOk{first}));
        for (std::size_t i = 0; i < config_.groups_per_subscription; ++i)
            pending_groups_.push_back({stream, subscribe.subscribe_id, first + i});
    }
    void refuse(ScriptedLitePeer& peer, transport::StreamId stream, std::uint64_t code) {
        if ((stream & 2u) == 0u) peer.peer_reset(stream, code);
        peer.peer_stop_sending(stream, code);
        cancelled_.insert(stream);
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
    };

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
        // False while the message is incomplete; true once decoded (into target) or malformed.
        const auto take = [&](auto result, auto& target) {
            if (std::holds_alternative<wire::NeedMore>(result)) return false;
            if (std::holds_alternative<wire::DecodeError>(result)) {
                malformed = true;
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
        handle_request(peer, request, malformed);
    }

    void handle_request(ScriptedLitePeer& peer, const LiteRunnerRequest& request, bool malformed) {
        if (!request.bidirectional && request.stream_type == 0x1) {
            if (malformed || request.setup_streams_seen > 1) close(peer, config_.protocol_violation_code);
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

    void emit_one_group(ScriptedLitePeer& peer) {
        while (!pending_groups_.empty()) {
            const auto group = pending_groups_.front();
            pending_groups_.pop_front();
            if (cancelled_.contains(group.subscribe_stream)) continue;
            Bytes bytes = join({stream_type(0x0), group_header({group.subscribe_id, group.sequence, 0})});
            for (std::size_t f = 0; f < config_.frames_per_group; ++f) {
                l06::Frame value;
                value.timestamp_delta = f == 0 ? static_cast<std::int64_t>(group.sequence * 1000) : 33;
                value.payload = bytes_of("frame-" + std::to_string(group.sequence) + "-" + std::to_string(f));
                const auto encoded = frame(value);
                bytes.insert(bytes.end(), encoded.begin(), encoded.end());
            }
            peer.data(peer.open_peer_uni(), std::move(bytes), true);
            ++groups_sent_;
            return;
        }
    }

    ConformingLitePublisherConfig config_;
    bool started_{false};
    std::map<transport::StreamId, Parse> parse_;
    std::set<transport::StreamId> cancelled_;
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
