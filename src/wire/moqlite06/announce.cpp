#include "moq/interop/wire/moqlite06/announce.h"

#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

DecodeError violation(std::size_t offset, const char* detail) {
    return DecodeError{DecodeErrorCode::ProtocolViolation, offset, detail};
}

// A field read from inside a body: running out of body is a malformed message, not a short input.
template <class T>
std::optional<DecodeError> body_error(const DecodeResult<T>& result, std::size_t offset, const char* detail) {
    if (std::holds_alternative<NeedMore>(result)) return violation(offset, detail);
    if (const auto* error = std::get_if<DecodeError>(&result)) return *error;
    return std::nullopt;
}

// Reads one varint from a body; the value is only valid when no error is returned.
std::optional<DecodeError> body_varint(Cursor& cursor, std::uint64_t& value, const char* detail) {
    const auto result = read_varint(cursor);
    if (const auto error = body_error(result, cursor.offset(), detail)) return error;
    value = std::get<std::uint64_t>(result);
    return std::nullopt;
}

// Hop Count, Hop ID ..., Warm Route Cost, Cold Route Cost.
DecodeResult<RouteMetadata> read_route(Cursor& cursor, const DecodeLimits& limits) {
    std::uint64_t count = 0;
    if (const auto error = body_varint(cursor, count, "announce message has no hop count")) return *error;
    if (count > limits.max_hops) {
        return DecodeError{DecodeErrorCode::LengthExceedsLimit, cursor.offset(), "hop count exceeds configured limit"};
    }

    RouteMetadata route;
    std::unordered_set<std::uint64_t> seen;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t hop = 0;
        if (const auto error = body_varint(cursor, hop, "hop count exceeds hop ids present")) return *error;
        if (hop != 0 && !seen.insert(hop).second) return violation(cursor.offset(), "duplicate non-zero hop id");
        route.hop_ids.push_back(hop);
    }
    if (const auto error = body_varint(cursor, route.warm_cost, "announce message has no warm route cost")) {
        return *error;
    }
    if (const auto error = body_varint(cursor, route.cold_cost, "announce message has no cold route cost")) {
        return *error;
    }
    return route;
}

// Frames the next message from `input` into a body cursor; `working` is advanced past it.
struct Framed {
    std::span<const std::byte> body;
    std::size_t body_offset;
};

DecodeResult<Framed> frame(Cursor& working, const DecodeLimits& limits) {
    const auto framed = read_framed_body(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto body = std::get<std::span<const std::byte>>(framed);
    return Framed{body, working.offset() - body.size()};
}

bool has_duplicate_nonzero(const std::vector<std::uint64_t>& hops) {
    std::unordered_set<std::uint64_t> seen;
    for (const auto hop : hops) {
        if (hop != 0 && !seen.insert(hop).second) return true;
    }
    return false;
}

std::optional<EncodeError> check_route(const RouteMetadata& route, const DecodeLimits& limits) {
    if (route.hop_ids.size() > limits.max_hops) return EncodeError::LimitExceeded;
    for (const auto hop : route.hop_ids) {
        if (hop > kMaxVarint) return EncodeError::InvalidValue;
    }
    if (route.warm_cost > kMaxVarint || route.cold_cost > kMaxVarint) return EncodeError::InvalidValue;
    if (has_duplicate_nonzero(route.hop_ids)) return EncodeError::InvalidValue;
    return std::nullopt;
}

bool write_route(const RouteMetadata& route, ByteWriter& body) {
    bool fits = write_varint(route.hop_ids.size(), body);
    for (const auto hop : route.hop_ids) fits = fits && write_varint(hop, body);
    return fits && write_varint(route.warm_cost, body) && write_varint(route.cold_cost, body);
}

// Writes [Type (i)] Message Length (i) body, or nothing at all.
std::optional<EncodeError> emit(std::optional<std::uint64_t> type, const ByteWriter& body, ByteWriter& output) {
    const auto bytes = body.bytes();
    const std::size_t length_size = varint_size(bytes.size());
    if (length_size == 0) return EncodeError::LimitExceeded;
    const std::size_t total = (type ? varint_size(*type) : 0) + length_size + bytes.size();
    if (total > output.remaining()) return EncodeError::OutputCapacity;
    if (type && !write_varint(*type, output)) return EncodeError::OutputCapacity;
    if (!write_framed_message(bytes, output)) return EncodeError::OutputCapacity;
    return std::nullopt;
}

}  // namespace

DecodeResult<AnnounceRequest> decode_announce_request(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    const auto prefix = read_string(cursor, limits.max_string_length);
    if (const auto error = body_error(prefix, cursor.offset(), "announce request prefix is truncated")) return *error;
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return AnnounceRequest{std::get<std::string>(prefix)};
}

DecodeResult<AnnounceOk> decode_announce_ok(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);

    Cursor cursor(body, body_offset);
    AnnounceOk message;
    if (const auto error = body_varint(cursor, message.hop_id, "announce ok has no hop id")) return *error;
    if (const auto error = body_varint(cursor, message.active_count, "announce ok has no active count")) {
        return *error;
    }
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

DecodeResult<AnnounceMessage> decode_announce_message(Cursor& input, const DecodeLimits& limits) {
    Cursor working = input;
    const auto type_result = read_varint(working);
    if (const auto* need = std::get_if<NeedMore>(&type_result)) return *need;
    const auto type = std::get<std::uint64_t>(type_result);
    if (type > kAnnounceTypeUpdate) {
        return DecodeError{DecodeErrorCode::InvalidValue, input.offset(), "unknown announce message type"};
    }

    const auto framed = frame(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&framed)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&framed)) return *error;
    const auto& [body, body_offset] = std::get<Framed>(framed);
    Cursor cursor(body, body_offset);

    AnnounceMessage message;
    if (type == kAnnounceTypeStart) {
        const auto suffix = read_string(cursor, limits.max_string_length);
        if (const auto error = body_error(suffix, cursor.offset(), "announce start suffix is truncated")) return *error;
        auto route = read_route(cursor, limits);
        if (const auto* need = std::get_if<NeedMore>(&route)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&route)) return *error;
        message = AnnounceStart{std::get<std::string>(suffix), std::move(std::get<RouteMetadata>(route))};
    } else if (type == kAnnounceTypeEnd) {
        AnnounceEnd end;
        if (const auto error = body_varint(cursor, end.announce_id, "announce end has no announce id")) return *error;
        message = end;
    } else {
        AnnounceUpdate update;
        if (const auto error = body_varint(cursor, update.announce_id, "announce update has no announce id")) {
            return *error;
        }
        auto route = read_route(cursor, limits);
        if (const auto* need = std::get_if<NeedMore>(&route)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&route)) return *error;
        update.route = std::move(std::get<RouteMetadata>(route));
        message = std::move(update);
    }
    if (const auto trailing = expect_body_consumed(cursor)) return *trailing;

    input = working;
    return message;
}

std::optional<EncodeError> encode_announce_request(const AnnounceRequest& message, ByteWriter& output,
                                                   const DecodeLimits& limits) {
    if (message.prefix.size() > limits.max_string_length) return EncodeError::LimitExceeded;
    ByteWriter body(limits.max_message_length);
    if (!write_string(message.prefix, body)) return EncodeError::LimitExceeded;
    return emit(std::nullopt, body, output);
}

std::optional<EncodeError> encode_announce_ok(const AnnounceOk& message, ByteWriter& output,
                                              const DecodeLimits& limits) {
    if (message.hop_id > kMaxVarint || message.active_count > kMaxVarint) return EncodeError::InvalidValue;
    ByteWriter body(limits.max_message_length);
    if (!write_varint(message.hop_id, body) || !write_varint(message.active_count, body)) {
        return EncodeError::LimitExceeded;
    }
    return emit(std::nullopt, body, output);
}

std::optional<EncodeError> encode_announce_message(const AnnounceMessage& message, ByteWriter& output,
                                                   const DecodeLimits& limits) {
    ByteWriter body(limits.max_message_length);
    std::uint64_t type = 0;
    bool fits = true;

    if (const auto* start = std::get_if<AnnounceStart>(&message)) {
        if (start->suffix.size() > limits.max_string_length) return EncodeError::LimitExceeded;
        if (const auto error = check_route(start->route, limits)) return error;
        type = kAnnounceTypeStart;
        fits = write_string(start->suffix, body) && write_route(start->route, body);
    } else if (const auto* end = std::get_if<AnnounceEnd>(&message)) {
        if (end->announce_id > kMaxVarint) return EncodeError::InvalidValue;
        type = kAnnounceTypeEnd;
        fits = write_varint(end->announce_id, body);
    } else {
        const auto& update = std::get<AnnounceUpdate>(message);
        if (update.announce_id > kMaxVarint) return EncodeError::InvalidValue;
        if (const auto error = check_route(update.route, limits)) return error;
        type = kAnnounceTypeUpdate;
        fits = write_varint(update.announce_id, body) && write_route(update.route, body);
    }
    if (!fits) return EncodeError::LimitExceeded;
    return emit(type, body, output);
}

}  // namespace moq::interop::wire::moqlite06
