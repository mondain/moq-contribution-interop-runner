#include "draft21_contribution_support.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/wire/draft21/successful_response.h"
#include "moq/interop/wire/draft21/token.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios::d21c {

bool fixture_valid(const Fixture& fixture) {
    return fetch_first_object_fixture_valid(fixture.track_namespace, fixture.track_name);
}

void put_vi(Bytes& output, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64 capacity");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}

void put_lp(Bytes& output, const Bytes& value) {
    put_vi(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

Bytes bytes_of(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

Bytes frame(std::uint64_t type, const Bytes& body) {
    if (body.size() > 65535) throw std::invalid_argument("contribution frame exceeds 65535 bytes");
    Bytes result;
    put_vi(result, type);
    result.push_back(static_cast<std::byte>(body.size() >> 8u));
    result.push_back(static_cast<std::byte>(body.size() & 255u));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

Param param_u8(std::uint64_t type, unsigned value) {
    return {type, bytes_of({value})};
}
Param param_vi(std::uint64_t type, std::uint64_t value) {
    Bytes encoded;
    put_vi(encoded, value);
    return {type, std::move(encoded)};
}
Param param_location(std::uint64_t type, std::uint64_t group, std::uint64_t object) {
    Bytes encoded;
    put_vi(encoded, group);
    put_vi(encoded, object);
    return {type, std::move(encoded)};
}
Param param_lp(std::uint64_t type, const Bytes& value) {
    Bytes encoded;
    put_lp(encoded, value);
    return {type, std::move(encoded)};
}

Bytes encode_params(std::vector<Param> params) {
    std::stable_sort(params.begin(), params.end(),
                     [](const Param& left, const Param& right) { return left.type < right.type; });
    Bytes result;
    std::uint64_t previous = 0;
    for (const auto& entry : params) {
        put_vi(result, entry.type - previous);
        previous = entry.type;
        result.insert(result.end(), entry.value.begin(), entry.value.end());
    }
    return result;
}

void put_namespace(Bytes& output, const Namespace& track_namespace) {
    put_vi(output, track_namespace.size());
    for (const auto& field : track_namespace) put_lp(output, field);
}

Bytes request_frame(std::uint64_t type, std::uint64_t request_id, const Fixture& fixture,
                    bool with_name, const std::vector<Param>& params) {
    Bytes body;
    put_vi(body, request_id);
    put_namespace(body, fixture.track_namespace);
    if (with_name) put_lp(body, fixture.track_name);
    put_vi(body, params.size());
    const auto encoded = encode_params(params);
    body.insert(body.end(), encoded.begin(), encoded.end());
    return frame(type, body);
}

Bytes subscribe_frame(std::uint64_t request_id, const Fixture& fixture,
                      const std::vector<Param>& params) {
    return request_frame(0x3, request_id, fixture, true, params);
}
Bytes fetch_frame(std::uint64_t request_id, const Fixture& fixture,
                  const std::vector<Param>& params) {
    return request_frame(0x16, request_id, fixture, true, params);
}
Bytes track_status_frame(std::uint64_t request_id, const Fixture& fixture) {
    return request_frame(0xd, request_id, fixture, true, {});
}
Bytes subscribe_namespace_frame(std::uint64_t request_id, const Namespace& prefix) {
    Bytes body;
    put_vi(body, request_id);
    put_namespace(body, prefix);
    put_vi(body, 0);
    return frame(0x50, body);
}
Bytes request_update_frame(std::uint64_t request_id, const std::vector<Param>& params) {
    Bytes body;
    put_vi(body, request_id);
    put_vi(body, params.size());
    const auto encoded = encode_params(params);
    body.insert(body.end(), encoded.begin(), encoded.end());
    return frame(0x2, body);
}

Bytes token_value(std::uint64_t alias_type, std::optional<std::uint64_t> alias,
                  std::optional<std::uint64_t> token_type, const Bytes& value) {
    Bytes result;
    put_vi(result, alias_type);
    if (alias) put_vi(result, *alias);
    if (token_type) {
        put_vi(result, *token_type);
        result.insert(result.end(), value.begin(), value.end());
    }
    return result;
}

std::optional<std::uint64_t> read_vi(wire::Cursor& cursor) {
    const auto decoded = wire::read_vi64(cursor);
    if (const auto* value = std::get_if<std::uint64_t>(&decoded)) return *value;
    return std::nullopt;
}

std::optional<std::span<const std::byte>> read_n(wire::Cursor& cursor, std::size_t length) {
    const auto decoded = wire::read_bytes(cursor, length);
    if (const auto* value = std::get_if<std::span<const std::byte>>(&decoded)) return *value;
    return std::nullopt;
}

std::optional<Fixture> recover_fixture(std::span<const std::byte> request) {
    if (request.size() > 65546) return std::nullopt;
    wire::Cursor cursor(request);
    const auto type = read_vi(cursor);
    if (!type || (*type != 0x3 && *type != 0x16 && *type != 0xd && *type != 0x50 && *type != 0x51)) return std::nullopt;
    const auto length = read_n(cursor, 2);
    if (!length) return std::nullopt;
    const auto size = (static_cast<std::size_t>(std::to_integer<unsigned>((*length)[0])) << 8u) |
                      std::to_integer<unsigned>((*length)[1]);
    const auto body_bytes = read_n(cursor, size);
    if (!body_bytes || cursor.remaining() != 0) return std::nullopt;
    wire::Cursor body(*body_bytes);
    const auto id = read_vi(body);
    const auto count = read_vi(body);
    if (!id || !count || *count > 32) return std::nullopt;
    Fixture fixture;
    std::size_t total = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto field = wire::read_length_prefixed_bytes(body, 4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&field);
        if (!value || value->empty()) return std::nullopt;
        total += value->size();
        fixture.track_namespace.emplace_back(value->begin(), value->end());
    }
    if (*type == 0x50 || *type == 0x51) {
        // Discovery names no track; any ordinary track name completes the fixture.
        fixture.track_name = bytes_of({'x'});
    } else {
        const auto name = wire::read_length_prefixed_bytes(body, 4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&name);
        if (!value) return std::nullopt;
        fixture.track_name.assign(value->begin(), value->end());
    }
    if (!fixture_valid(fixture)) return std::nullopt;
    return fixture;
}

std::optional<wire::draft21::Token> recover_token(std::span<const std::byte> request) {
    if (request.size() > 65546) return std::nullopt;
    wire::Cursor cursor(request);
    const auto type = read_vi(cursor);
    if (!type || (*type != 0x3 && *type != 0x16 && *type != 0xd)) return std::nullopt;
    const auto length = read_n(cursor, 2);
    if (!length) return std::nullopt;
    const auto size = (static_cast<std::size_t>(std::to_integer<unsigned>((*length)[0])) << 8u) |
                      std::to_integer<unsigned>((*length)[1]);
    const auto body_bytes = read_n(cursor, size);
    if (!body_bytes) return std::nullopt;
    wire::Cursor body(*body_bytes);
    const auto id = read_vi(body);
    const auto fields = read_vi(body);
    if (!id || !fields || *fields > 32) return std::nullopt;
    for (std::uint64_t index = 0; index < *fields; ++index) {
        const auto field_length = read_vi(body);
        if (!field_length || !read_n(body, static_cast<std::size_t>(*field_length))) return std::nullopt;
    }
    const auto name_length = read_vi(body);
    if (!name_length || !read_n(body, static_cast<std::size_t>(*name_length))) return std::nullopt;
    const auto count = read_vi(body);
    if (!count) return std::nullopt;
    std::uint64_t parameter = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto delta = read_vi(body);
        if (!delta) return std::nullopt;
        parameter += *delta;
        // Even types carry a varint; odd types a length-prefixed value (Section 9.20).
        if ((parameter & 1u) == 0u) {
            if (!read_vi(body)) return std::nullopt;
            continue;
        }
        const auto value_length = read_vi(body);
        const auto value = value_length ? read_n(body, static_cast<std::size_t>(*value_length)) : std::nullopt;
        if (!value) return std::nullopt;
        if (parameter != 0x03) continue;
        const auto token = wire::draft21::decode_token(*value);
        if (const auto* decoded = std::get_if<wire::draft21::Token>(&token)) return *decoded;
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<Frame> parse_frames(const StreamRecord& record, bool& malformed) {
    std::vector<Frame> result;
    malformed = false;
    std::size_t offset = 0;
    while (offset < record.bytes.size()) {
        wire::Cursor cursor(std::span<const std::byte>(record.bytes).subspan(offset));
        const auto decoded_type = wire::read_vi64(cursor);
        if (std::holds_alternative<wire::NeedMore>(decoded_type)) break;
        const auto* type = std::get_if<std::uint64_t>(&decoded_type);
        if (!type) { malformed = true; break; }
        const auto length = read_n(cursor, 2);
        if (!length) break;
        const auto size = (static_cast<std::size_t>(std::to_integer<unsigned>((*length)[0])) << 8u) |
                          std::to_integer<unsigned>((*length)[1]);
        const auto body = read_n(cursor, size);
        if (!body) break;
        const auto end = offset + cursor.offset();
        std::size_t event = 0;
        for (const auto& [index, cumulative] : record.chunks) {
            event = index;
            if (cumulative >= end) break;
        }
        result.push_back({*type, Bytes(body->begin(), body->end()), event});
        offset = end;
    }
    return result;
}

View::View(const RawProbeTranscript& transcript) : View(transcript.writes, transcript.events) {
    courtesy_ = transcript.courtesy_writes;
    unknown_alias_code_ = transcript.unknown_auth_token_alias_compatibility_code;
}

View::View(std::span<const RawProbeAcceptedWrite> writes,
           std::span<const transport::TransportEvent> events)
    : writes_(writes) {
    if (events.size() > kMaximumEvents) { valid_ = false; return; }
    std::size_t total = 0;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (data->data.size() > kMaximumTotalBytes - total) { valid_ = false; return; }
            total += data->data.size();
            auto [found, inserted] = streams_.try_emplace(data->stream_id);
            auto& record = found->second;
            if (inserted) record.first_event = index;
            if (record.fin || record.reset) { valid_ = false; continue; }
            record.bytes.insert(record.bytes.end(), data->data.begin(), data->data.end());
            record.chunks.emplace_back(index, record.bytes.size());
            if (data->fin) { record.fin = true; record.fin_event = index; }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            auto [found, inserted] = streams_.try_emplace(reset->stream_id);
            if (inserted) found->second.first_event = index;
            found->second.reset = true;
            found->second.reset_event = index;
            found->second.reset_code = reset->application_error;
        } else if (const auto* datagram = std::get_if<transport::DatagramEvent>(&event)) {
            if (datagram->data.size() > kMaximumTotalBytes - total) { valid_ = false; return; }
            total += datagram->data.size();
            datagrams_.push_back({index, datagram->data});
        } else if (const auto* closed = std::get_if<transport::PeerCloseEvent>(&event)) {
            if (!close_) {
                close_ = PeerCloseInfo{closed->error_space == transport::CloseErrorSpace::Application,
                                       closed->error_code, index};
            }
        }
    }
    // The peer control stream is the lowest peer unidirectional stream that
    // carries a complete SETUP.
    for (const auto& [id, record] : streams_) {
        if ((id & 3u) != 2u) continue;
        wire::Cursor cursor(record.bytes);
        const auto decoded = wire::draft21::decode_setup(cursor);
        if (const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded)) {
            peer_setup_ = *setup;
            break;
        }
    }
}

std::optional<std::uint64_t> View::peer_option(std::uint64_t type) const {
    if (!peer_setup_) return std::nullopt;
    for (const auto& option : peer_setup_->options) {
        if (option.type != type) continue;
        if (const auto* value = std::get_if<std::uint64_t>(&option.value)) return *value;
    }
    return std::nullopt;
}

const StreamRecord* View::stream(transport::StreamId id) const {
    const auto found = streams_.find(id);
    return found == streams_.end() ? nullptr : &found->second;
}

std::optional<transport::StreamId> View::write_stream_id(std::size_t index) const {
    if (index >= writes_.size()) return std::nullopt;
    return writes_[index].stream_id;
}

std::optional<std::size_t> View::write_event(std::size_t index) const {
    if (index >= writes_.size()) return std::nullopt;
    return writes_[index].delivery_event_count;
}

const StreamRecord* View::write_stream(std::size_t index) const {
    const auto id = write_stream_id(index);
    return id ? stream(*id) : nullptr;
}

std::vector<Frame> View::frames(const StreamRecord& record) const {
    bool malformed = false;
    return parse_frames(record, malformed);
}

std::vector<Frame> View::write_frames(std::size_t index) const {
    const auto* record = write_stream(index);
    return record ? frames(*record) : std::vector<Frame>{};
}

bool setup_decodes(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<wire::draft21::SetupMessage>(wire::draft21::decode_setup(cursor));
}

std::optional<wire::draft21::SetupMessage> peer_setup_from_events(
    std::span<const transport::TransportEvent> events) {
    std::map<transport::StreamId, Bytes> candidates;
    std::size_t total = 0;
    for (const auto& event : events) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 3u) != 2u) continue;
        if (data->data.size() > kMaximumTotalBytes - total) return std::nullopt;
        total += data->data.size();
        auto& bytes = candidates[data->stream_id];
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
    }
    for (const auto& [id, bytes] : candidates) {
        wire::Cursor cursor(bytes);
        const auto decoded = wire::draft21::decode_setup(cursor);
        if (const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded)) return *setup;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> setup_option_value(const wire::draft21::SetupMessage& setup,
                                                std::uint64_t type) {
    for (const auto& option : setup.options) {
        if (option.type != type) continue;
        if (const auto* value = std::get_if<std::uint64_t>(&option.value)) return *value;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> setup_numeric_option(std::span<const std::byte> input,
                                                  std::uint64_t type) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_setup(cursor);
    const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded);
    if (!setup) return std::nullopt;
    for (const auto& option : setup->options) {
        if (option.type != type) continue;
        if (const auto* value = std::get_if<std::uint64_t>(&option.value)) return *value;
    }
    return std::nullopt;
}

bool subscribe_ok_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    return std::holds_alternative<wire::draft21::SuccessfulResponse>(
        wire::draft21::decode_successful_response(cursor, wire::draft21::ResponseContext::Subscribe));
}

Bytes empty_setup() { return bytes_of({0xaf, 0, 0, 0}); }

RawProbeDefinition base_definition(const std::string& id) {
    RawProbeDefinition definition;
    definition.id = id;
    definition.setup_bytes = empty_setup();
    definition.peer_setup_ready = setup_decodes;
    return definition;
}

bool opens_with_response(const std::vector<Frame>& frames) {
    return !frames.empty() && (frames.front().type == kSubscribeOk || frames.front().type == kRequestError);
}

}  // namespace moq::interop::scenarios::d21c
