#include "moq/interop/scenarios/draft22_run_names.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/cursor.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace moq::interop::scenarios {
namespace {
// A probe frame body is at most 65535 bytes; the names leave room for the rest of the message.
constexpr std::size_t kMaximumNamesBytes = 65000;
// Recovery bounds: generous, since only an exact rebuild is accepted.
constexpr std::uint64_t kMaximumFields = 4096;
constexpr std::size_t kMaximumFieldBytes = 65535;
}  // namespace

ProbeTrackNames probe_track_names(std::vector<std::vector<std::byte>> track_namespace,
                                  std::vector<std::byte> track_name) {
    if (current_wire_draft() != 22 || track_name.empty()) return {};
    return {std::move(track_namespace), std::move(track_name)};
}

std::vector<std::byte> encode_probe_track_names(const ProbeTrackNames& names) {
    wire::ByteWriter output(kMaximumNamesBytes);
    bool success = wire::write_vi64(names.track_namespace.size(), output);
    for (const auto& field : names.track_namespace)
        success = success && wire::write_length_prefixed_bytes(field, output);
    success = success && wire::write_length_prefixed_bytes(names.track_name, output);
    if (!success) throw std::invalid_argument("track names do not fit a probe request");
    return {output.bytes().begin(), output.bytes().end()};
}

std::optional<ProbeTrackNames> recover_probe_track_names(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto type = wire::read_vi64(cursor);
    if (!std::holds_alternative<std::uint64_t>(type)) return std::nullopt;
    if (!std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor, 2))) return std::nullopt;
    const auto request = wire::read_vi64(cursor);
    const auto count = wire::read_vi64(cursor);
    const auto* fields = std::get_if<std::uint64_t>(&count);
    if (!std::holds_alternative<std::uint64_t>(request) || !fields || *fields > kMaximumFields)
        return std::nullopt;
    ProbeTrackNames names;
    names.track_name.clear();
    for (std::uint64_t index = 0; index < *fields; ++index) {
        const auto field = wire::read_length_prefixed_bytes(cursor, kMaximumFieldBytes);
        const auto* value = std::get_if<std::span<const std::byte>>(&field);
        if (!value) return std::nullopt;
        names.track_namespace.emplace_back(value->begin(), value->end());
    }
    const auto name = wire::read_length_prefixed_bytes(cursor, kMaximumFieldBytes);
    const auto* value = std::get_if<std::span<const std::byte>>(&name);
    if (!value) return std::nullopt;
    names.track_name.assign(value->begin(), value->end());
    return names;
}

}  // namespace moq::interop::scenarios
