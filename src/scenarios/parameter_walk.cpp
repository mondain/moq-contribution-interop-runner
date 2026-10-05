#include "moq/interop/scenarios/parameter_walk.h"

#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <limits>
#include <variant>

namespace moq::interop::scenarios {
namespace {

constexpr std::uint64_t kLocationFilter = 0x21;
constexpr std::uint64_t kFillParameters = 0x23;
constexpr std::uint64_t kMaximumLength = 65535;
// FILL_PARAMETERS may not nest (Table 7), but a peer can send it; walking stops computing nested status
// this deep, which bounds the recursion.
constexpr int kMaximumNesting = 4;

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

std::optional<ParameterValueKind> kind_of(std::uint64_t type) {
    switch (type) {
        case 0x10: case 0x20: case 0x22: case 0x35:
            return ParameterValueKind::Byte;
        case 0x02: case 0x04: case 0x06: case 0x08: case 0x0a: case 0x32:
            return ParameterValueKind::Varint;
        case 0x09:
            return ParameterValueKind::Location;
        case kLocationFilter:
            return current_wire_draft() == 22 ? ParameterValueKind::LocationFilter
                                              : ParameterValueKind::LengthPrefixed;
        case 0x03: case kFillParameters: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x34:
            return ParameterValueKind::LengthPrefixed;
        default:
            return std::nullopt;
    }
}

ParameterWalkResult walk_nested(std::span<const std::byte> payload, const ParameterVisitor& visit, int depth);

// Reads one value of `parameter.kind` from `body`, filling the parameter; false when it cannot be read.
bool read_value(wire::Cursor& body, WalkedParameter& parameter, int depth) {
    wire::Cursor start = body;
    switch (parameter.kind) {
        case ParameterValueKind::Byte: {
            const auto octet = read_n(body, 1);
            if (!octet) return false;
            parameter.number = std::to_integer<std::uint64_t>((*octet)[0]);
            break;
        }
        case ParameterValueKind::Varint:
            parameter.number = read_vi(body);
            if (!parameter.number) return false;
            break;
        case ParameterValueKind::Location: {
            const auto group = read_vi(body);
            const auto object = group ? read_vi(body) : std::nullopt;
            if (!group || !object) return false;
            parameter.location = {*group, *object};
            break;
        }
        case ParameterValueKind::LengthPrefixed: {
            const auto length = read_vi(body);
            if (!length || *length > kMaximumLength) return false;
            const auto bytes = read_n(body, static_cast<std::size_t>(*length));
            if (!bytes) return false;
            parameter.payload = *bytes;
            break;
        }
        case ParameterValueKind::LocationFilter: {
            const auto filter = wire::draft22::decode_location_filter(body);
            if (!std::holds_alternative<wire::draft22::LocationFilter>(filter)) return false;
            break;
        }
    }
    // The value is every byte the read advanced over.
    const auto value = read_n(start, body.offset() - start.offset());
    if (!value) return false;
    parameter.value = *value;
    if (parameter.kind != ParameterValueKind::LengthPrefixed) parameter.payload = parameter.value;
    if (parameter.type == kFillParameters && depth < kMaximumNesting)
        parameter.nested = walk_nested(parameter.payload, [](const WalkedParameter&) {}, depth + 1).status;
    return true;
}

// Reads one delta-encoded parameter after `previous`; on success visits it and advances `previous`.
std::optional<ParameterWalkResult> step(wire::Cursor& body, std::uint64_t& previous, const ParameterVisitor& visit,
                                        ParameterWalkResult& result, int depth) {
    const auto delta = read_vi(body);
    if (!delta) return ParameterWalkResult{ParameterWalkStatus::Malformed, result.visited, std::nullopt};
    if (*delta > std::numeric_limits<std::uint64_t>::max() - previous)
        return ParameterWalkResult{ParameterWalkStatus::TypeOverflow, result.visited, std::nullopt};
    WalkedParameter parameter;
    parameter.type = previous + *delta;
    previous = parameter.type;
    const auto kind = kind_of(parameter.type);
    if (!kind) return ParameterWalkResult{ParameterWalkStatus::UnknownType, result.visited, parameter.type};
    parameter.kind = *kind;
    if (!read_value(body, parameter, depth))
        return ParameterWalkResult{ParameterWalkStatus::Malformed, result.visited, parameter.type};
    visit(parameter);
    ++result.visited;
    return std::nullopt;
}

ParameterWalkResult walk_nested(std::span<const std::byte> payload, const ParameterVisitor& visit, int depth) {
    wire::Cursor body(payload);
    ParameterWalkResult result;
    std::uint64_t previous = 0;
    while (body.remaining() != 0)
        if (auto stop = step(body, previous, visit, result, depth)) return *stop;
    return result;
}

}  // namespace

ParameterWalkResult walk_message_parameters(wire::Cursor& body, std::uint64_t count, const ParameterVisitor& visit) {
    ParameterWalkResult result;
    std::uint64_t previous = 0;
    for (std::uint64_t index = 0; index < count; ++index)
        if (auto stop = step(body, previous, visit, result, 0)) return *stop;
    return result;
}

ParameterWalkResult walk_nested_parameters(std::span<const std::byte> payload, const ParameterVisitor& visit) {
    return walk_nested(payload, visit, 1);
}

}  // namespace moq::interop::scenarios
