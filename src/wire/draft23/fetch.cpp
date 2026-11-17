#include "moq/interop/wire/draft23/fetch.h"
#include <limits>

namespace moq::interop::wire::draft23 {
namespace {
[[noreturn]] void malformed(const Cursor& c, const char* text) {
    throw DecodeError{DecodeErrorCode::InvalidValue, c.offset(), text};
}
template<class T> T take(DecodeResult<T> result) {
    if (const auto* value = std::get_if<T>(&result)) return *value;
    if (const auto* need = std::get_if<NeedMore>(&result)) throw *need;
    throw std::get<DecodeError>(result);
}
std::uint64_t number(Cursor& c) { return take(read_vi64(c)); }
std::uint64_t add(Cursor& c, std::uint64_t base, std::uint64_t delta, std::uint64_t extra = 0) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (delta > maximum - base || extra > maximum - base - delta) malformed(c, "FETCH delta encoding overflow");
    return base + delta + extra;
}
std::uint64_t group(Cursor& c, const FetchState& state, std::uint64_t delta, std::uint64_t extra) {
    if (!state.prior_location) return delta;
    const auto previous = state.prior_location->group;
    if (!state.descending) return add(c, previous, delta, extra);
    if (delta > previous || extra > previous - delta) malformed(c, "FETCH descending delta underflow");
    return previous - delta - extra;
}
draft21::KeyValues properties(Cursor& c, std::size_t maximum) {
    const auto bytes = take(read_length_prefixed_bytes(c, maximum));
    Cursor body(bytes);
    auto decoded = draft21::decode_key_values_to_end(body);
    if (std::holds_alternative<NeedMore>(decoded)) malformed(c, "truncated FETCH property value");
    return take(std::move(decoded));
}
}
DecodeResult<FetchEntry> decode_fetch_entry(Cursor& input, FetchState& state,
    std::size_t maximum_payload, std::size_t maximum_properties) {
    Cursor working = input;
    FetchState next = state;
    try {
        if (state.ended_track) malformed(working, "entry after End of Track");
        const auto flags = number(working);
        FetchEntry result;
        if (flags >= 128) {
            if (flags == 0x80 || flags == 0x81) {
                if (!next.prior_location) malformed(working, "end marker has no prior location");
                next.prior_location->object = std::numeric_limits<std::uint64_t>::max();
                next.ended_track = flags == 0x81;
                result = FetchEnd{next.ended_track};
            } else {
                FetchRangeKind kind;
                switch (flags) {
                    case 0x8c: kind = FetchRangeKind::NonExistent; break;
                    case 0x10c: kind = FetchRangeKind::Unknown; break;
                    case 0x20c: kind = FetchRangeKind::TimedOut; break;
                    case 0x40c: kind = FetchRangeKind::Gone; break;
                    default: malformed(working, "unknown FETCH indicator");
                }
                const auto gd = number(working); const auto od = number(working);
                const auto gid = group(working, state, gd, 0);
                const auto oid = state.prior_location && gd == 0
                    ? add(working, state.prior_location->object, od, 1) : od;
                next.prior_location = draft21::Location{gid, oid};
                result = FetchRange{kind, *next.prior_location};
            }
        } else {
            const bool has_group = (flags & 8) != 0;
            const bool has_object = (flags & 4) != 0;
            if (!state.prior_location && (!has_group || !has_object)) malformed(working, "first FETCH object lacks absolute IDs");
            const auto gid = has_group ? group(working, state, number(working), 1) : state.prior_location->group;
            std::optional<std::uint64_t> subgroup;
            if (!(flags & 0x40)) {
                switch (flags & 3) {
                    case 0: subgroup = 0; break;
                    case 1: if (!state.prior_subgroup) malformed(working, "FETCH subgroup references no prior subgroup"); subgroup = state.prior_subgroup; break;
                    case 2: if (!state.prior_subgroup) malformed(working, "FETCH subgroup references no prior subgroup"); subgroup = add(working, *state.prior_subgroup, 1); break;
                    case 3: subgroup = number(working); break;
                }
            }
            std::uint64_t oid;
            if (has_object) {
                const auto delta = number(working);
                oid = has_group ? delta : add(working, state.prior_location->object, delta, 2);
            } else {
                oid = add(working, state.prior_location->object, 1);
            }
            std::uint8_t priority;
            if (flags & 0x10) priority = std::to_integer<std::uint8_t>(take(read_bytes(working, 1))[0]);
            else {
                if (!state.prior_priority) malformed(working, "FETCH priority references no prior priority");
                priority = *state.prior_priority;
            }
            draft21::KeyValues values;
            if (flags & 0x20) values = properties(working, maximum_properties);
            const auto bytes = take(read_length_prefixed_bytes(working, maximum_payload));
            next.prior_location = draft21::Location{gid, oid};
            next.prior_priority = priority;
            // Datagram objects have no subgroup, including for later references.
            next.prior_subgroup = subgroup;
            result = FetchObject{flags, *next.prior_location, subgroup, priority,
                std::move(values), {bytes.begin(), bytes.end()}};
        }
        input = working;
        state = next;
        return result;
    } catch (const NeedMore& need) { return need; }
      catch (const DecodeError& error) { return error; }
}
}  // namespace moq::interop::wire::draft23
