#include "moq/interop/wire/draft18/objects.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace {

using namespace moq::interop::wire::draft18;
using moq::interop::wire::ByteWriter;
using moq::interop::wire::DecodeError;
using moq::interop::wire::write_vi64;

constexpr std::array kTerminalProbe{std::byte{0xa5}};

[[noreturn]] void invariant_failed() { std::abort(); }

void require(bool condition) {
    if (!condition) invariant_failed();
}

void append_vi64(std::vector<std::byte>& output, std::uint64_t value) {
    ByteWriter writer(9);
    require(write_vi64(value, writer));
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}

std::size_t memory_bound(const Limits& limits) {
    constexpr auto property_factor = sizeof(KeyValuePair) + 32u;
    return limits.maximum_object_properties_length * property_factor +
           limits.maximum_retained_payload_length * 2u + 256u;
}

void check_event(const ObjectEvent& event, std::size_t supplied,
                 const Limits& limits) {
    require(event.stream_offset <= event.stream_end_offset);
    require(event.stream_end_offset <= supplied);
    require(event.retained_payload.size() <=
            limits.maximum_retained_payload_length);
    require(event.retained_payload.size() <= event.payload_length);
}

bool terminal_ambiguity(const FetchDecoderObservation& observation) {
    return observation.kind == DecoderObservationKind::DraftAmbiguity &&
           (observation.phase == FetchDecodePhase::GroupIdDelta ||
            observation.phase == FetchDecodePhase::SubgroupId);
}

void check_subgroup_result(const SubgroupPushResult& result,
                           std::size_t supplied, const Limits& limits) {
    const bool evidence = result.header.has_value() || !result.objects.empty() ||
                          !result.observations.empty() ||
                          result.error.has_value() ||
                          result.final_object_id.has_value() || result.clean_fin;
    require(!result.local_api_misuse || !evidence);
    if (result.header) {
        require(result.header->stream_offset <= result.header->stream_end_offset);
        require(result.header->stream_end_offset <= supplied);
    }
    for (const auto& event : result.objects) check_event(event, supplied, limits);
    for (const auto& observation : result.observations) {
        require(observation.offset <= supplied);
    }
    if (result.error) require(result.error->offset <= supplied);
    require(!result.clean_fin || !result.error.has_value());
}

void check_fetch_result(const FetchPushResult& result, std::size_t supplied,
                        const Limits& limits) {
    const bool evidence = result.header.has_value() || !result.events.empty() ||
                          !result.observations.empty() ||
                          result.error.has_value() || result.clean_fin;
    require(!result.local_api_misuse || !evidence);
    if (result.header) {
        require(result.header->stream_offset <= result.header->stream_end_offset);
        require(result.header->stream_end_offset <= supplied);
    }
    for (const auto& event : result.events) {
        if (const auto* object = std::get_if<ObjectEvent>(&event)) {
            check_event(*object, supplied, limits);
        } else {
            const auto& range = std::get<FetchRangeEvent>(event);
            require(range.stream_offset <= range.stream_end_offset);
            require(range.stream_end_offset <= supplied);
        }
    }
    for (const auto& observation : result.observations) {
        require(observation.offset <= supplied);
    }
    if (result.error) require(result.error->offset <= supplied);
    require(!result.clean_fin || !result.error.has_value());
}

void check_padding_result(const PaddingStreamPushResult& result,
                          std::size_t supplied, std::size_t chunk_size) {
    const bool evidence = result.discarded_byte_count != 0u ||
                          result.first_nonzero_offset.has_value() ||
                          !result.observations.empty() ||
                          result.error.has_value() || result.clean_fin;
    require(!result.local_api_misuse || !evidence);
    require(result.discarded_byte_count <= chunk_size);
    if (result.first_nonzero_offset) {
        require(*result.first_nonzero_offset < supplied);
    }
    for (const auto& observation : result.observations) {
        require(observation.offset <= supplied);
    }
    if (result.error) require(result.error->offset <= supplied);
    require(!result.clean_fin || !result.error.has_value());
}

enum class DeliveryMode { OneShot, MultiChunk, ByteAtATime };

std::size_t vi_width(std::byte first_byte) {
    const auto first = std::to_integer<std::uint8_t>(first_byte);
    if (first == 0xffu) return 9u;
    std::size_t leading_ones = 0u;
    auto mask = std::uint8_t{0x80};
    while ((first & mask) != 0u) {
        ++leading_ones;
        mask = static_cast<std::uint8_t>(mask >> 1u);
    }
    return leading_ones + 1u;
}

std::vector<std::span<const std::byte>> chunks(
    std::span<const std::byte> input, DeliveryMode mode, std::uint8_t control) {
    std::vector<std::span<const std::byte>> result;
    if (input.empty()) return result;
    if (mode == DeliveryMode::OneShot) {
        result.push_back(input);
        return result;
    }
    if (mode == DeliveryMode::ByteAtATime) {
        result.reserve(input.size());
        for (std::size_t index = 0; index < input.size(); ++index) {
            result.push_back(input.subspan(index, 1));
        }
        return result;
    }

    const auto first = std::min(
        input.size(), static_cast<std::size_t>(control % 7u) + 1u);
    result.push_back(input.first(first));
    auto offset = first;
    std::size_t width = 1u;
    while (offset < input.size()) {
        width = width == 5u ? 1u : width + 1u;
        const auto count = std::min(width, input.size() - offset);
        result.push_back(input.subspan(offset, count));
        offset += count;
    }
    return result;
}

void require_empty_misuse(const SubgroupPushResult& result) {
    require(result.local_api_misuse);
    require(!result.header && result.objects.empty() &&
            result.observations.empty() && !result.error &&
            !result.final_object_id && !result.clean_fin);
}

void require_empty_misuse(const FetchPushResult& result) {
    require(result.local_api_misuse);
    require(!result.header && result.events.empty() &&
            result.observations.empty() && !result.error && !result.clean_fin);
}

void require_empty_misuse(const PaddingStreamPushResult& result) {
    require(result.local_api_misuse);
    require(result.discarded_byte_count == 0u &&
            !result.first_nonzero_offset && result.observations.empty() &&
            !result.error && !result.clean_fin);
}

void exercise_subgroup(std::span<const std::byte> input, DeliveryMode mode,
                       bool fin, std::uint8_t control, const Limits& limits) {
    SubgroupDecoder decoder(limits);
    const auto empty = decoder.push({}, false);
    check_subgroup_result(empty, 0u, limits);
    require(decoder.buffered_byte_count() <= memory_bound(limits));

    auto supplied = std::size_t{0};
    bool terminal = false;
    const auto parts = chunks(input, mode, control);
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const bool push_fin = fin && index + 1u == parts.size();
        supplied += parts[index].size();
        const auto result = decoder.push(parts[index], push_fin);
        check_subgroup_result(result, supplied, limits);
        require(decoder.buffered_byte_count() <= memory_bound(limits));
        terminal = result.error.has_value() || push_fin;
        if (terminal) break;
    }
    if (parts.empty() && fin) {
        const auto result = decoder.push({}, true);
        check_subgroup_result(result, supplied, limits);
        terminal = true;
    }
    if (terminal) require_empty_misuse(decoder.push(kTerminalProbe, false));
}

FetchGroupOrderResolver resolver_for(std::uint8_t selector) {
    if (selector % 3u == 0u) {
        return [](std::uint64_t) {
            return std::optional{FetchGroupOrder::Ascending};
        };
    }
    if (selector % 3u == 1u) {
        return [](std::uint64_t) {
            return std::optional{FetchGroupOrder::Descending};
        };
    }
    return [](std::uint64_t) -> std::optional<FetchGroupOrder> {
        return std::nullopt;
    };
}

void exercise_fetch(std::span<const std::byte> input, DeliveryMode mode,
                    bool fin, std::uint8_t control, const Limits& limits,
                    std::uint8_t order_selector) {
    FetchDecoder decoder(resolver_for(order_selector), limits);
    const auto empty = decoder.push({}, false);
    check_fetch_result(empty, 0u, limits);
    require(decoder.buffered_byte_count() <= memory_bound(limits));

    auto supplied = std::size_t{0};
    bool terminal = false;
    const auto parts = chunks(input, mode, control);
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const bool push_fin = fin && index + 1u == parts.size();
        supplied += parts[index].size();
        const auto result = decoder.push(parts[index], push_fin);
        check_fetch_result(result, supplied, limits);
        require(decoder.buffered_byte_count() <= memory_bound(limits));
        terminal = result.error.has_value() || push_fin ||
                   std::ranges::any_of(result.observations,
                                       terminal_ambiguity);
        if (terminal) break;
    }
    if (parts.empty() && fin) {
        const auto result = decoder.push({}, true);
        check_fetch_result(result, supplied, limits);
        terminal = true;
    }
    if (terminal) require_empty_misuse(decoder.push(kTerminalProbe, false));
}

void exercise_padding(std::span<const std::byte> input, DeliveryMode mode,
                      bool fin, std::uint8_t control) {
    PaddingStreamDecoder decoder;
    const auto empty = decoder.push({}, false);
    check_padding_result(empty, 0u, 0u);
    require(decoder.buffered_byte_count() <= 9u);

    auto supplied = std::size_t{0};
    auto discarded = std::size_t{0};
    bool terminal = false;
    const auto parts = chunks(input, mode, control);
    const auto header_width = input.empty() ? 0u : vi_width(input.front());
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const bool push_fin = fin && index + 1u == parts.size();
        supplied += parts[index].size();
        const auto result = decoder.push(parts[index], push_fin);
        check_padding_result(result, supplied, parts[index].size());
        discarded += result.discarded_byte_count;
        const auto bytes_after_header =
            supplied > header_width ? supplied - header_width : 0u;
        require(discarded <= bytes_after_header);
        require(decoder.buffered_byte_count() <= 9u);
        terminal = result.error.has_value() || push_fin;
        if (terminal) break;
    }
    if (parts.empty() && fin) {
        const auto result = decoder.push({}, true);
        check_padding_result(result, supplied, 0u);
        terminal = true;
    }
    if (terminal) require_empty_misuse(decoder.push(kTerminalProbe, false));
}

void exercise_fixed_oracles() {
    constexpr std::array object_datagram{
        std::byte{0x0c}, std::byte{0x01}, std::byte{0x02}, std::byte{0xaa}};
    const auto object = decode_datagram(object_datagram, {});
    require(std::holds_alternative<ObjectEvent>(object));

    constexpr std::array padding_datagram{
        std::byte{0xf0}, std::byte{0x13}, std::byte{0x2b},
        std::byte{0x3e}, std::byte{0x29}, std::byte{0x00}};
    const auto padding = decode_datagram(padding_datagram, {});
    require(std::holds_alternative<DiscardedPaddingDatagram>(padding));

    for (std::uint64_t type = 0; type < 128u; ++type) {
        if ((type & 0x10u) == 0u) continue;
        std::vector<std::byte> encoded;
        append_vi64(encoded, type);
        append_vi64(encoded, 1);
        append_vi64(encoded, 2);
        if ((type & 0x06u) == 0x04u) append_vi64(encoded, 3);
        if ((type & 0x20u) == 0u) encoded.push_back(std::byte{4});
        SubgroupDecoder decoder;
        const auto result = decoder.push(encoded, true);
        if ((type & 0x06u) == 0x06u) {
            require(result.error.has_value());
        } else {
            require(result.header.has_value());
            require(result.header->raw_type == type);
        }
    }

    for (const auto range_flags : {0x8cu, 0x10cu}) {
        std::vector<std::byte> encoded;
        append_vi64(encoded, 0x05);
        append_vi64(encoded, 7);
        append_vi64(encoded, range_flags);
        append_vi64(encoded, 11);
        append_vi64(encoded, 13);
        FetchDecoder decoder(resolver_for(0));
        const auto result = decoder.push(encoded, true);
        require(result.header.has_value());
        require(result.events.size() == 1u);
        require(std::holds_alternative<FetchRangeEvent>(result.events.front()));
        require(result.clean_fin);
    }

    constexpr std::array nonminimal_fetch_type{
        std::byte{0x80}, std::byte{0x05}, std::byte{0x07}};
    FetchDecoder fetch(resolver_for(0));
    const auto fetch_result = fetch.push(nonminimal_fetch_type, true);
    require(fetch_result.header.has_value() && fetch_result.clean_fin);

    constexpr std::array padding_stream{
        std::byte{0xf0}, std::byte{0x13}, std::byte{0x2b},
        std::byte{0x3e}, std::byte{0x28}, std::byte{0x00}};
    PaddingStreamDecoder padding_decoder;
    const auto padding_result = padding_decoder.push(padding_stream, true);
    require(padding_result.clean_fin);
    require(padding_result.discarded_byte_count == 1u);

    constexpr std::array nonminimal_padding_stream{
        std::byte{0xf8}, std::byte{0x00}, std::byte{0x13},
        std::byte{0x2b}, std::byte{0x3e}, std::byte{0x28}};
    PaddingStreamDecoder nonminimal_padding_decoder;
    const auto nonminimal_padding_result =
        nonminimal_padding_decoder.push(nonminimal_padding_stream, true);
    require(nonminimal_padding_result.clean_fin);
}

void exercise_datagram(std::span<const std::byte> input,
                       const Limits& limits) {
    const auto result = decode_datagram(input, limits);
    require(result.index() < std::variant_size_v<DatagramDecodeResult>);
    if (const auto* object = std::get_if<ObjectEvent>(&result)) {
        check_event(*object, input.size(), limits);
    } else if (const auto* padding =
                   std::get_if<DiscardedPaddingDatagram>(&result)) {
        if (padding->first_nonzero_offset) {
            require(*padding->first_nonzero_offset < input.size());
        }
    } else if (const auto* error = std::get_if<DecodeError>(&result)) {
        require(error->offset <= input.size());
    } else {
        require(std::get<DraftAmbiguity>(result).offset <= input.size());
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    static const bool fixed_oracles_checked = [] {
        exercise_fixed_oracles();
        return true;
    }();
    require(fixed_oracles_checked);

    const auto all_bytes = std::as_bytes(std::span(data, size));
    const auto controls_size = std::min<std::size_t>(4u, all_bytes.size());
    const auto input = all_bytes.subspan(controls_size);
    const auto control = [data, size](std::size_t index) {
        return index < size ? data[index] : std::uint8_t{0};
    };

    Limits limits;
    limits.maximum_object_properties_length =
        static_cast<std::size_t>(control(0) % 64u) * 32u;
    limits.maximum_retained_payload_length =
        static_cast<std::size_t>(control(1));
    limits.maximum_odd_value_length =
        static_cast<std::size_t>(control(2) % 64u) * 32u;
    limits.maximum_parameter_count =
        static_cast<std::size_t>(control(3) % 32u);

    exercise_datagram(input, limits);
    for (const auto mode : {DeliveryMode::OneShot, DeliveryMode::MultiChunk,
                            DeliveryMode::ByteAtATime}) {
        for (const bool fin : {false, true}) {
            exercise_subgroup(input, mode, fin, control(2), limits);
            for (std::uint8_t order = 0; order < 3u; ++order) {
                exercise_fetch(input, mode, fin, control(3), limits, order);
            }
            exercise_padding(input, mode, fin, control(2));
        }
    }
    return 0;
}
