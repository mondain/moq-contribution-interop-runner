#include "moq/interop/wire/draft23/messages.h"
#include "moq/interop/wire/draft23/frame.h"
#include "moq/interop/wire/draft21/token.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace moq::interop::wire::draft23 {
namespace {
[[noreturn]] void invalid(const Cursor& c, const char* reason, std::uint64_t code = 0x300) {
    throw MessageDecodeFailure{{DecodeErrorCode::InvalidValue, c.offset(), reason}, code};
}
template<class T> T required(DecodeResult<T> result, const Cursor& c, std::uint64_t code = 0x300) {
    if (const auto* value = std::get_if<T>(&result)) return *value;
    if (const auto* error = std::get_if<DecodeError>(&result))
        throw MessageDecodeFailure{*error, error->code == DecodeErrorCode::LengthExceedsLimit ? 0x401u : code};
    invalid(c, "truncated draft-23 message body", code);
}
std::uint64_t integer(Cursor& c, std::uint64_t code = 0x300) { return required(read_vi64(c), c, code); }
std::vector<std::byte> field(Cursor& c, std::size_t limit = 65535, std::uint64_t code = 0x300) {
    const auto bytes = required(read_length_prefixed_bytes(c, limit), c, code);
    return {bytes.begin(), bytes.end()};
}
TrackNamespace names(Cursor& c) {
    const auto count = integer(c);
    if (count > 32) invalid(c, "namespace has more than 32 fields", 0x302);
    TrackNamespace result;
    std::size_t total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto value = field(c, 4096 - total);
        if (value.empty()) invalid(c, "empty namespace field", 0x302);
        total += value.size();
        result.push_back(std::move(value));
    }
    return result;
}
std::vector<std::byte> track(Cursor& c, const TrackNamespace& ns) {
    std::size_t total = 0;
    for (const auto& value : ns) total += value.size();
    auto value = field(c, 4096 - total);
    if (value.empty()) invalid(c, "empty track name", 0x301);
    return value;
}
bool allowed(std::uint64_t type, std::uint64_t message, bool fill) {
    if (fill) {
        switch (type) { case 0x0a: case 0x20: case 0x21: case 0x22: case 0x25: case 0x26: case 0x27: case 0x28: return true; default: return false; }
    }
    if (message == 6) return type == 3;
    if (message == 7 || message == 4) return type == 8 || type == 9;
    if (message == 0x1d) {
        switch (type) { case 2: case 3: case 6: case 8: case 9: case 0x11: case 0x20: case 0x21: case 0x22: case 0x23: case 0x36: return true; default: return false; }
    }
    if (message == 0x16) {
        switch (type) { case 3: case 0xa: case 0x20: case 0x21: case 0x22: case 0x25: case 0x26: case 0x27: case 0x28: case 0x35: return true; default: return false; }
    }
    if (message == 3) {
        switch (type) { case 2: case 3: case 4: case 6: case 0x11: case 0x20: case 0x21: case 0x22: case 0x23: case 0x25: case 0x26: case 0x27: case 0x28: case 0x32: case 0x35: return true; default: return false; }
    }
    return false;
}
Parameters parameters(Cursor& c, std::uint64_t message, bool fill = false) {
    const auto count = integer(c);
    if (count > c.remaining()) invalid(c, "parameter count exceeds message body");
    Parameters result;
    std::uint64_t previous = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto delta = integer(c);
        if (delta > std::numeric_limits<std::uint64_t>::max() - previous) invalid(c, "parameter type overflow", 0x400);
        const auto type = previous + delta;
        if (i && !delta && type != 3) invalid(c, "duplicate message parameter", 0x305);
        if (!allowed(type, message, fill)) invalid(c, "unknown or out-of-scope message parameter", 0x305);
        previous = type;
        if (type == 0x11 || type == 0x20 || type == 0x22 || type == 0x35) {
            const auto bytes = required(read_bytes(c, 1), c);
            const auto value = std::to_integer<std::uint8_t>(bytes[0]);
            if (((type == 0x11 || type == 0x35) && value > 1) || (type == 0x22 && (value < 1 || value > 2))) invalid(c, "invalid uint8 parameter", 0x305);
            result.push_back({type, value});
        } else if (type == 9) {
            const auto group = integer(c);
            result.push_back({type, draft21::Location{group, integer(c)}});
        } else if (type == 0x21) {
            const auto filter_type = integer(c);
            if (filter_type > 5) invalid(c, "unknown Location Filter Type", 0x305);
            draft22::LocationFilter filter;
            filter.type = static_cast<draft22::LocationFilterType>(filter_type);
            const unsigned count = filter_type == 5 ? 0 : static_cast<unsigned>(filter_type);
            if (count >= 1) filter.start_group = integer(c);
            if (count >= 2) filter.start_object = integer(c);
            if (count >= 3) {
                filter.end_group_delta = integer(c);
                if (*filter.end_group_delta > std::numeric_limits<std::uint64_t>::max() - filter.start_group)
                    invalid(c, "Location Filter end group overflows", 0x400);
            }
            if (count == 4) filter.end_object = integer(c);
            result.push_back({type, filter});
        } else if (type == 0x23) {
            result.push_back({type, parameters(c, message, true)});
        } else if (type == 3 || (type >= 0x25 && type <= 0x28)) {
            auto value = field(c, c.remaining());
            if (type == 3) (void)required(draft21::decode_token(value), c, 0x16);
            result.push_back({type, std::move(value)});
        } else {
            result.push_back({type, integer(c)});
        }
    }
    return result;
}
// Section 8.3 retains the draft-21 key/value layout and 65535-byte value cap.
draft21::KeyValues key_values(Cursor& c) {
    draft21::KeyValues values;
    std::uint64_t previous = 0;
    while (c.remaining()) {
        const auto delta = integer(c);
        if (delta > std::numeric_limits<std::uint64_t>::max() - previous) invalid(c, "key-value type overflow", 0x400);
        previous += delta;
        if (previous & 1) values.push_back({previous, field(c, 65535)});
        else values.push_back({previous, integer(c)});
    }
    return values;
}
bool put_values(const draft21::KeyValues& values, ByteWriter& w) {
    std::uint64_t previous = 0;
    for (const auto& value : values) {
        if (value.type < previous || !write_vi64(value.type - previous, w)) return false;
        previous = value.type;
        if (value.type & 1) {
            const auto* bytes = std::get_if<std::vector<std::byte>>(&value.value);
            if (!bytes || !write_length_prefixed_bytes(*bytes, w)) return false;
        } else {
            const auto* number = std::get_if<std::uint64_t>(&value.value);
            if (!number || !write_vi64(*number, w)) return false;
        }
    }
    return true;
}
bool put_names(const TrackNamespace& ns, ByteWriter& w) {
    if (ns.size() > 32 || !write_vi64(ns.size(), w)) return false;
    std::size_t total = 0;
    for (const auto& value : ns) {
        if (value.empty() || value.size() > 4096 - total || !write_length_prefixed_bytes(value, w)) return false;
        total += value.size();
    }
    return true;
}
bool put_parameters(const Parameters& values, ByteWriter& w, std::uint64_t message, bool fill = false) {
    if (!write_vi64(values.size(), w)) return false;
    std::uint64_t previous = 0;
    for (const auto& p : values) {
        if (!allowed(p.type, message, fill) || p.type < previous || !write_vi64(p.type - previous, w)) return false;
        previous = p.type;
        const auto t = p.type;
        const auto expected = t == 0x11 || t == 0x20 || t == 0x22 || t == 0x35 ? 0u
            : t == 9 ? 2u : t == 0x21 ? 4u : t == 0x23 ? 5u
            : t == 3 || (t >= 0x25 && t <= 0x28) ? 3u : 1u;
        if (p.value.index() != expected) return false;
        const bool ok = std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::uint8_t>) return w.append_byte(std::byte{v});
            else if constexpr (std::is_same_v<T, std::uint64_t>) return write_vi64(v, w);
            else if constexpr (std::is_same_v<T, draft21::Location>) return write_vi64(v.group, w) && write_vi64(v.object, w);
            else if constexpr (std::is_same_v<T, std::vector<std::byte>>) return write_length_prefixed_bytes(v, w);
            else if constexpr (std::is_same_v<T, draft22::LocationFilter>) return !draft22::encode_location_filter(v, w);
            else return put_parameters(v, w, message, true);
        }, p.value);
        if (!ok) return false;
    }
    return true;
}
}  // namespace

MessageDecodeResult decode_message_with_error(Cursor& input, std::uint64_t maximum_body_size) {
    Cursor working = input;
    auto frame = decode_frame(working, maximum_body_size);
    if (const auto* need = std::get_if<NeedMore>(&frame)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&frame))
        return MessageDecodeFailure{*error, error->code == DecodeErrorCode::LengthExceedsLimit ? 0x401u : 0x300u};
    const auto& f = std::get<Frame>(frame);
    Cursor body(f.body, working.offset() - f.body.size());
    try {
        Message message;
        switch (f.type) {
            case 6: {
                const auto id = integer(body); auto ns = names(body);
                message = PublishNamespaceMessage{id, std::move(ns), parameters(body, 6)}; break;
            }
            case 0x1d: {
                const auto id = integer(body); auto ns = names(body); auto name = track(body, ns);
                const auto alias = integer(body); auto params = parameters(body, 0x1d);
                message = PublishMessage{id, std::move(ns), std::move(name), alias, std::move(params), key_values(body)}; break;
            }
            case 3: {
                const auto id = integer(body); auto ns = names(body); auto name = track(body, ns);
                message = SubscribeMessage{id, std::move(ns), std::move(name), parameters(body, 3)}; break;
            }
            case 0x16: {
                const auto id = integer(body); auto ns = names(body); auto name = track(body, ns);
                message = FetchMessage{id, std::move(ns), std::move(name), parameters(body, 0x16)}; break;
            }
            case 7: { auto params = parameters(body, 7); message = RequestOkMessage{std::move(params), key_values(body)}; break; }
            case 4: { const auto alias = integer(body); auto params = parameters(body, 4); message = SubscribeOkMessage{alias, std::move(params), key_values(body)}; break; }
            case 5: {
                const auto code = integer(body); const auto retry = integer(body); auto reason = field(body, 1024);
                auto details = key_values(body);
                if (code == 0x34 && std::none_of(details.begin(), details.end(), [](const auto& d) { return d.type == 1; }))
                    invalid(body, "REDIRECT requires a REDIRECT Error Detail");
                if (code == 0x34) {
                    for (const auto& detail : details) {
                        if (detail.type != 1) continue;
                        Cursor redirect(std::get<std::vector<std::byte>>(detail.value));
                        (void)field(redirect, 8192);
                        const auto ns = names(redirect);
                        std::size_t namespace_size = 0;
                        for (const auto& f : ns) namespace_size += f.size();
                        (void)field(redirect, 4096 - namespace_size);
                        if (redirect.remaining()) invalid(redirect, "trailing REDIRECT detail bytes");
                    }
                }
                message = RequestErrorMessage{code, retry, std::move(reason), std::move(details)}; break;
            }
            case 0xb: { const auto status = integer(body); const auto count = integer(body); message = PublishDoneMessage{status, count, field(body, 1024)}; break; }
            case 0x10: { auto uri = field(body, 8192); message = GoawayMessage{std::move(uri), integer(body)}; break; }
            // Table 5 types not yet implemented by this bounded smoke codec.
            case 0x2f00: case 2: case 0x22: case 0xd: case 0x50: case 0x51: case 8: case 0xe: case 0xf:
                return MessageDecodeFailure{{DecodeErrorCode::InvalidValue, input.offset(),
                    "harness does not implement this known draft-23 message"}, 0, true};
            default: invalid(body, "unknown draft-23 control message type", 0x303);
        }
        if (body.remaining()) invalid(body, "trailing message body bytes");
        input = working;
        return message;
    } catch (const MessageDecodeFailure& failure) { return failure; }
}

DecodeResult<Message> decode_message(Cursor& input, std::uint64_t maximum_body_size) {
    auto result = decode_message_with_error(input, maximum_body_size);
    if (const auto* message = std::get_if<Message>(&result)) return *message;
    if (const auto* need = std::get_if<NeedMore>(&result)) return *need;
    return std::get<MessageDecodeFailure>(result).error;
}

bool encode_message(const Message& message, ByteWriter& output, std::uint64_t maximum_body_size) {
    const auto capacity = static_cast<std::size_t>(std::min<std::uint64_t>(maximum_body_size, output.remaining()));
    ByteWriter body(capacity);
    std::uint64_t type = 0;
    const auto put = [&](std::uint64_t v) { return write_vi64(v, body); };
    const bool ok = std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, PublishNamespaceMessage>) {
            type = 6; return put(m.request_id) && put_names(m.track_namespace, body) && put_parameters(m.parameters, body, type);
        } else if constexpr (std::is_same_v<T, PublishMessage>) {
            type = 0x1d; return put(m.request_id) && put_names(m.track_namespace, body) && write_length_prefixed_bytes(m.track_name, body) && put(m.track_alias) && put_parameters(m.parameters, body, type) && put_values(m.track_properties, body);
        } else if constexpr (std::is_same_v<T, SubscribeMessage>) {
            type = 3; return put(m.request_id) && put_names(m.track_namespace, body) && write_length_prefixed_bytes(m.track_name, body) && put_parameters(m.parameters, body, type);
        } else if constexpr (std::is_same_v<T, FetchMessage>) {
            type = 0x16; return put(m.request_id) && put_names(m.track_namespace, body) && write_length_prefixed_bytes(m.track_name, body) && put_parameters(m.parameters, body, type);
        } else if constexpr (std::is_same_v<T, RequestOkMessage>) {
            type = 7; return put_parameters(m.parameters, body, type) && put_values(m.track_properties, body);
        } else if constexpr (std::is_same_v<T, SubscribeOkMessage>) {
            type = 4; return put(m.track_alias) && put_parameters(m.parameters, body, type) && put_values(m.track_properties, body);
        } else if constexpr (std::is_same_v<T, RequestErrorMessage>) {
            type = 5; return put(m.error_code) && put(m.retry_interval) && write_length_prefixed_bytes(m.reason, body) && put_values(m.error_details, body);
        } else if constexpr (std::is_same_v<T, PublishDoneMessage>) {
            type = 0xb; return put(m.status_code) && put(m.stream_count) && write_length_prefixed_bytes(m.reason, body);
        } else {
            type = 0x10; return write_length_prefixed_bytes(m.uri, body) && put(m.timeout);
        }
    }, message);
    if (!ok) return false;
    ByteWriter encoded(output.remaining());
    if (!encode_frame(type, body.bytes(), maximum_body_size, encoded)) return false;
    Cursor validate(encoded.bytes());
    if (!std::holds_alternative<Message>(decode_message(validate, maximum_body_size))) return false;
    return output.append_bytes(encoded.bytes());
}
}  // namespace moq::interop::wire::draft23
