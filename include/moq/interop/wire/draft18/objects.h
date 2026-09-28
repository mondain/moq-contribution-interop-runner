#pragma once

#include "moq/interop/wire/draft18/messages.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft18 {

enum class ObjectForwardingPreference {
    Datagram,
    Subgroup,
};

struct ObjectEvent {
    std::optional<std::uint64_t> datagram_type;
    std::uint64_t track_alias;
    std::uint64_t group_id;
    std::uint64_t object_id;
    std::optional<std::uint8_t> publisher_priority;
    bool end_of_group;
    KeyValuePairs properties;
    std::optional<std::uint64_t> status;
    std::uint64_t payload_length;
    std::vector<std::byte> retained_payload;
    ObjectForwardingPreference forwarding_preference{
        ObjectForwardingPreference::Datagram};
    std::optional<std::uint64_t> subgroup_id;
    std::optional<std::uint64_t> subgroup_header_type;
    bool first_object{false};
    bool priority_inherited{false};
    std::size_t stream_offset{0};
    std::size_t stream_end_offset{0};
};

struct DiscardedPaddingDatagram {
    std::optional<std::size_t> first_nonzero_offset;
};

using DatagramDecodeResult =
    std::variant<ObjectEvent, DiscardedPaddingDatagram, DecodeError,
                 DraftAmbiguity>;

DatagramDecodeResult decode_datagram(std::span<const std::byte> bytes,
                                     const Limits& limits);

struct SubgroupHeader {
    std::uint64_t raw_type;
    std::uint64_t track_alias;
    std::uint64_t group_id;
    std::optional<std::uint64_t> subgroup_id;
    std::optional<std::uint8_t> publisher_priority;
    bool properties_present;
    bool end_of_group;
    bool first_object;
    bool priority_inherited;
    std::size_t stream_offset;
    std::size_t stream_end_offset;
};

enum class DecoderObservationKind {
    ShouldClose,
    DraftAmbiguity,
};

enum class SubgroupDecodePhase {
    Type,
    TrackAlias,
    GroupId,
    SubgroupId,
    Priority,
    ObjectIdDelta,
    PropertiesLength,
    Properties,
    PayloadLength,
    Status,
    Payload,
};

struct DecoderObservation {
    DecoderObservationKind kind;
    SubgroupDecodePhase phase;
    std::size_t offset;
    std::string detail;
};

struct SubgroupPushResult {
    std::optional<SubgroupHeader> header;
    std::vector<ObjectEvent> objects;
    std::vector<DecoderObservation> observations;
    std::optional<DecodeError> error;
    std::optional<std::uint64_t> final_object_id;
    bool clean_fin{false};
    bool local_api_misuse{false};
};

class SubgroupDecoder {
public:
    explicit SubgroupDecoder(Limits limits = {});
    ~SubgroupDecoder();

    SubgroupDecoder(const SubgroupDecoder&) = delete;
    SubgroupDecoder& operator=(const SubgroupDecoder&) = delete;
    SubgroupDecoder(SubgroupDecoder&&) noexcept;
    SubgroupDecoder& operator=(SubgroupDecoder&&) noexcept;

    [[nodiscard]] SubgroupPushResult push(std::span<const std::byte> bytes,
                                          bool fin);
    [[nodiscard]] std::size_t buffered_byte_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::wire::draft18
