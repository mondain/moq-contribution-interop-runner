#include "raw_probe_courtesy.h"

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/token.h"

#include <algorithm>
#include <variant>

namespace moq::interop::scenarios {
namespace {

namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;

constexpr std::size_t kMaximumStreams = 64;
constexpr std::size_t kMaximumStreamBytes = 65546;
constexpr std::size_t kMaximumDataPrefix = 32;
constexpr std::uint64_t kRequestUpdate = 0x02;
constexpr std::uint64_t kPublish = 0x1d;
constexpr std::uint64_t kRequestError = 0x05;
constexpr std::uint64_t kUninterested = 0x20;

std::optional<std::uint64_t> vi(wire::Cursor& cursor) {
    const auto decoded = wire::read_vi64(cursor);
    if (const auto* value = std::get_if<std::uint64_t>(&decoded)) return *value;
    return std::nullopt;
}

// Section 8.9: does an AUTHORIZATION TOKEN parameter (0x03) in this REQUEST_UPDATE
// body (Request ID, parameters) name an Alias with Alias Type USE_ALIAS? PUBLISH
// tokens come from its decoder.
bool update_uses_alias(std::span<const std::byte> body) {
    wire::Cursor cursor(body);
    if (!vi(cursor)) return false;
    const auto count = vi(cursor);
    if (!count) return false;
    std::uint64_t parameter = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto delta = vi(cursor);
        if (!delta) return false;
        parameter += *delta;
        if ((parameter & 1u) == 0u) {
            if (!vi(cursor)) return false;
            continue;
        }
        const auto length = vi(cursor);
        if (!length) return false;
        const auto value = wire::read_bytes(cursor, static_cast<std::size_t>(*length));
        const auto* span = std::get_if<std::span<const std::byte>>(&value);
        if (!span) return false;
        if (parameter != 0x03) continue;
        const auto token = d21::decode_token(*span);
        if (const auto* decoded = std::get_if<d21::Token>(&token);
            decoded && decoded->alias_type == d21::TokenAliasType::UseAlias) return true;
    }
    return false;
}

}  // namespace

PublisherCourtesy::PublisherCourtesy(RawProbeCourtesy policy) : policy_(policy) {}

void PublisherCourtesy::note_alias(std::uint64_t alias) { aliases_with_objects_.insert(alias); }

void PublisherCourtesy::on_event(const transport::TransportEvent& event) {
    if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
        if ((data->stream_id & 3u) == 0u) {
            if (!streams_.contains(data->stream_id) && streams_.size() >= kMaximumStreams) return;
            auto& stream = streams_[data->stream_id];
            if (data->data.size() > kMaximumStreamBytes - std::min(stream.bytes.size(), kMaximumStreamBytes)) return;
            stream.bytes.insert(stream.bytes.end(), data->data.begin(), data->data.end());
            parse(data->stream_id, stream);
        } else if ((data->stream_id & 3u) == 2u &&
                   policy_.publish == RawProbePublishResponse::RejectAfterObject) {
            // A Subgroup stream begins with its Type and Track Alias (Section 11.3.1).
            if (!data_prefixes_.contains(data->stream_id) && data_prefixes_.size() >= kMaximumStreams) return;
            auto& prefix = data_prefixes_[data->stream_id];
            const auto room = kMaximumDataPrefix - std::min(prefix.size(), kMaximumDataPrefix);
            prefix.insert(prefix.end(), data->data.begin(),
                          data->data.begin() + static_cast<std::ptrdiff_t>(std::min(room, data->data.size())));
            wire::Cursor cursor(prefix);
            const auto type = vi(cursor);
            const auto alias = type ? vi(cursor) : std::nullopt;
            if (type && alias && *type < 128 && (*type & 0x10u) != 0) note_alias(*alias);
        }
    } else if (const auto* datagram = std::get_if<transport::DatagramEvent>(&event)) {
        if (policy_.publish != RawProbePublishResponse::RejectAfterObject) return;
        wire::Cursor cursor(datagram->data);
        const auto flags = vi(cursor);
        const auto alias = flags ? vi(cursor) : std::nullopt;
        if (alias) note_alias(*alias);
    }
}

void PublisherCourtesy::enqueue(Stream& stream, Bytes bytes, bool fin, RawProbeCourtesyKind kind,
                                RawProbeClock::time_point release) {
    // A hostile publisher can send tiny frames without end; past the budget
    // the runner stops volunteering responses.
    if (responses_enqueued_ >= kMaximumResponses) return;
    ++responses_enqueued_;
    // Responses on one request stream leave in order, so a held one holds the rest.
    if (!stream.queue.empty()) release = std::max(release, stream.queue.back().release);
    Response response;
    response.bytes = std::move(bytes);
    response.fin = fin;
    response.kind = kind;
    response.release = release;
    stream.queue.push_back(std::move(response));
}

void PublisherCourtesy::parse(transport::StreamId id, Stream& stream) {
    static const Bytes ok{std::byte{7}, std::byte{0}, std::byte{1}, std::byte{0}};
    while (stream.consumed < stream.bytes.size()) {
        if (frames_parsed_ >= kMaximumFrames) {
            // Nothing past the frame budget is parsed or answered.
            stream.consumed = stream.bytes.size();
            return;
        }
        wire::Cursor cursor(std::span<const std::byte>(stream.bytes).subspan(stream.consumed));
        const auto type = vi(cursor);
        if (!type) return;
        const auto length = wire::read_bytes(cursor, 2);
        const auto* prefix = std::get_if<std::span<const std::byte>>(&length);
        if (!prefix) return;
        const auto size = (static_cast<std::size_t>(std::to_integer<unsigned>((*prefix)[0])) << 8u) |
                          std::to_integer<unsigned>((*prefix)[1]);
        const auto body = wire::read_bytes(cursor, size);
        const auto* span = std::get_if<std::span<const std::byte>>(&body);
        if (!span) return;
        const Bytes frame_body(span->begin(), span->end());
        stream.consumed += cursor.offset();
        ++frames_parsed_;
        const bool first = stream.frames++ == 0;

        bool alias_use = false;
        std::optional<std::uint64_t> publish_alias;
        if (*type == kPublish) {
            // Decode the whole frame body as a PUBLISH body: re-frame to reuse the decoder.
            Bytes framed;
            wire::ByteWriter writer(9);
            wire::write_vi64(*type, writer);
            framed.insert(framed.end(), writer.bytes().begin(), writer.bytes().end());
            framed.push_back(static_cast<std::byte>(frame_body.size() >> 8u));
            framed.push_back(static_cast<std::byte>(frame_body.size() & 255u));
            framed.insert(framed.end(), frame_body.begin(), frame_body.end());
            wire::Cursor publish_cursor(framed);
            const auto decoded = d21::decode_publish(publish_cursor);
            if (const auto* publish = std::get_if<d21::PublishMessage>(&decoded)) {
                publish_alias = publish->track_alias;
                for (const auto& parameter : publish->parameters)
                    if (const auto* token = std::get_if<d21::Token>(&parameter.value))
                        alias_use = alias_use || token->alias_type == d21::TokenAliasType::UseAlias;
            }
        } else if (*type == kRequestUpdate) {
            alias_use = update_uses_alias(frame_body);
        }
        const bool hold = alias_use && policy_.update == RawProbeUpdateResponse::HoldAliasUses;
        const auto release = hold ? now_ + policy_.hold : now_;

        if (*type == kPublish && first) {
            switch (policy_.publish) {
            case RawProbePublishResponse::Ignore: break;
            case RawProbePublishResponse::Accept:
                enqueue(stream, ok, false, RawProbeCourtesyKind::PublishOk, release);
                break;
            case RawProbePublishResponse::Reject: {
                // REQUEST_ERROR UNINTERESTED, no retry interval, empty reason, then FIN.
                Bytes error{std::byte{kRequestError}, std::byte{0}, std::byte{3},
                            std::byte{kUninterested}, std::byte{0}, std::byte{0}};
                enqueue(stream, std::move(error), true, RawProbeCourtesyKind::PublishError, release);
                break;
            }
            case RawProbePublishResponse::RejectAfterObject:
                if (publish_alias) awaiting_.push_back({id, *publish_alias});
                break;
            }
        } else if (*type == kRequestUpdate && !first) {
            if (policy_.update != RawProbeUpdateResponse::Ignore)
                enqueue(stream, ok, false, RawProbeCourtesyKind::UpdateOk, release);
        }
    }
}

std::vector<RawProbeCourtesyWrite> PublisherCourtesy::step(transport::SessionTransport& transport,
                                                           RawProbeClock::time_point now,
                                                           std::size_t event_count) {
    now_ = now;
    // A rejection waiting for the publisher to be seen producing.
    for (auto it = awaiting_.begin(); it != awaiting_.end();) {
        if (!aliases_with_objects_.contains(it->alias)) { ++it; continue; }
        Bytes error{std::byte{kRequestError}, std::byte{0}, std::byte{3},
                    std::byte{kUninterested}, std::byte{0}, std::byte{0}};
        enqueue(streams_[it->stream_id], std::move(error), true, RawProbeCourtesyKind::PublishError, now);
        it = awaiting_.erase(it);
    }
    std::vector<RawProbeCourtesyWrite> completed;
    for (auto& [id, stream] : streams_) {
        while (!stream.queue.empty() && stream.queue.front().release <= now) {
            auto& response = stream.queue.front();
            if (response.written < response.bytes.size()) {
                const auto remaining = std::span<const std::byte>(response.bytes).subspan(response.written);
                const auto result = transport.write(id, remaining, false);
                if ((result.status != transport::TransportStatus::Success &&
                     result.status != transport::TransportStatus::Partial) ||
                    result.accepted > remaining.size()) break;
                response.written += result.accepted;
                if (response.written < response.bytes.size()) break;
            }
            if (response.fin && !response.fin_sent) {
                const auto result = transport.write(id, {}, true);
                if (result.status != transport::TransportStatus::Success) break;
                response.fin_sent = true;
            }
            completed.push_back({id, event_count, response.kind});
            stream.queue.pop_front();
        }
    }
    return completed;
}

}  // namespace moq::interop::scenarios
