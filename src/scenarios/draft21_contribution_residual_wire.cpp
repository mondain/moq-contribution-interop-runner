#include "draft21_contribution_residual_internal.h"

#include <algorithm>

namespace moq::interop::scenarios::d21c::residual {

using namespace shared;

RawProbeDefinition residual_definition() {
    return base_definition("");
}

std::vector<AliasStream> subgroup_streams(const View& view, std::uint64_t alias) {
    std::vector<AliasStream> result;
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        auto parsed = parse_subgroup(stream.bytes);
        if (parsed.header && parsed.alias == alias) result.push_back({id, std::move(parsed)});
    }
    return result;
}

std::optional<Object> parse_datagram_object(const DatagramRecord& datagram, std::uint64_t alias) {
    wire::Cursor cursor(datagram.data);
    const auto flags = read_vi(cursor);
    const auto track = flags ? read_vi(cursor) : std::nullopt;
    const auto group = track ? read_vi(cursor) : std::nullopt;
    if (!flags || !track || !group || *track != alias) return std::nullopt;
    std::uint64_t object = 0;
    if ((*flags & 0x04u) == 0) {
        const auto value = read_vi(cursor);
        if (!value) return std::nullopt;
        object = *value;
    }
    // Without a Status the remainder of the datagram is the payload (Section 11.2).
    Bytes payload;
    const bool has_payload = (*flags & 0x20u) == 0;
    if (has_payload) {
        if ((*flags & 0x08u) == 0 && !read_n(cursor, 1)) return std::nullopt;
        if ((*flags & 0x01u) != 0) {
            const auto length = read_vi(cursor);
            if (!length || *length > 65535 || !read_n(cursor, static_cast<std::size_t>(*length))) return std::nullopt;
        }
        const auto rest = read_n(cursor, cursor.remaining());
        if (rest) payload.assign(rest->begin(), rest->end());
    }
    return Object{*group, object, has_payload, 0, true, std::move(payload)};
}

std::vector<Object> delivered_objects(const View& view, std::uint64_t alias) {
    std::vector<Object> result;
    for (const auto& stream : subgroup_streams(view, alias))
        for (const auto& object : stream.parsed.objects)
            result.push_back({object.group, object.object, !object.status, stream.id, false, object.payload});
    for (const auto& datagram : view.datagrams())
        if (const auto object = parse_datagram_object(datagram, alias)) result.push_back(*object);
    return result;
}

std::optional<std::uint64_t> alias_of(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    if (frames.empty() || frames.front().type != kSubscribeOk) return std::nullopt;
    wire::Cursor body(frames.front().body);
    return read_vi(body);
}

bool rejected(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    return !frames.empty() && frames.front().type == kRequestError;
}

std::optional<Namespace> read_namespace(wire::Cursor& cursor) {
    const auto count = read_vi(cursor);
    if (!count || *count > 32) return std::nullopt;
    Namespace result;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto length = read_vi(cursor);
        const auto field = length ? read_n(cursor, static_cast<std::size_t>(*length)) : std::nullopt;
        if (!field || field->empty()) return std::nullopt;
        result.emplace_back(field->begin(), field->end());
    }
    return result;
}

std::optional<std::size_t> earliest(std::optional<std::size_t> left, std::optional<std::size_t> right) {
    if (!left) return right;
    if (!right) return left;
    return std::min(*left, *right);
}

std::vector<PublishRecord> publish_records(const View& view) {
    std::vector<PublishRecord> result;
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 0u) continue;
        const auto frames = view.frames(record);
        if (frames.empty() || frames.front().type != kPublish) continue;
        wire::Cursor body(frames.front().body);
        if (!read_vi(body)) continue;
        auto name_space = read_namespace(body);
        const auto length = name_space ? read_vi(body) : std::nullopt;
        const auto name = length ? read_n(body, static_cast<std::size_t>(*length)) : std::nullopt;
        const auto alias = name ? read_vi(body) : std::nullopt;
        if (!name_space || !name || !alias) continue;
        PublishRecord publish;
        publish.stream = id;
        publish.first_event = record.first_event;
        publish.track = {std::move(*name_space), Bytes(name->begin(), name->end())};
        publish.alias = *alias;
        publish.terminated = earliest(record.fin_event, record.reset_event);
        for (std::size_t index = 1; index < frames.size(); ++index) {
            if (frames[index].type == kPublishDone)
                publish.terminated = earliest(publish.terminated, frames[index].event);
            if (frames[index].type == kRequestUpdate) ++publish.updates;
        }
        for (const auto& write : view.courtesy_writes()) {
            if (write.stream_id != id || publish.response) continue;
            if (write.kind != RawProbeCourtesyKind::PublishOk && write.kind != RawProbeCourtesyKind::PublishError)
                continue;
            publish.response = write.kind;
            publish.response_event = write.event_count;
        }
        result.push_back(std::move(publish));
    }
    return result;
}

RawProbeDefinition observing(RawProbeCourtesy courtesy) {
    auto definition = base_definition("");
    definition.courtesy = courtesy;
    // Rejecting a PUBLISH can make a publisher give up.
    definition.publisher_exit_is_evidence = courtesy.publish == RawProbePublishResponse::Reject ||
                                            courtesy.publish == RawProbePublishResponse::RejectAfterObject;
    return definition;
}

RawProbeCourtesy accepting_publishes() {
    RawProbeCourtesy result;
    result.publish = RawProbePublishResponse::Accept;
    return result;
}

}  // namespace moq::interop::scenarios::d21c::residual
