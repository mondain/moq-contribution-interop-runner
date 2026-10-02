#pragma once
#include "moq/interop/wire/draft21/key_values.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
namespace moq::interop::wire::draft21 {
struct Limits {
    std::size_t maximum_object_properties_length{65535};
    std::size_t maximum_retained_payload_length{4096};
};
enum class ObjectForwardingPreference {
    Datagram,
    Subgroup,
};

struct ObjectEvent {
    std::optional<std::uint64_t> datagram_type;
    std::optional<std::uint64_t> track_alias;
    std::uint64_t group_id;
    std::uint64_t object_id;
    std::optional<std::uint8_t> publisher_priority;
    bool end_of_group;
    KeyValues properties;
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
    std::optional<std::uint64_t> request_id;
    std::optional<std::uint64_t> serialization_flags;
};

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
    ShouldViolation,
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

enum class FetchGroupOrder {
    Ascending,
    Descending,
};

using FetchGroupOrderResolver =
    std::function<std::optional<FetchGroupOrder>(std::uint64_t request_id)>;

struct FetchHeader {
    std::uint64_t raw_type;
    std::uint64_t request_id;
    std::size_t stream_offset;
    std::size_t stream_end_offset;
};

enum class FetchRangeKind {
    NonExistent,
    Unknown,
    TimedOut,
};

struct FetchRangeEvent {
    FetchRangeKind kind;
    std::uint64_t serialization_flags;
    std::uint64_t group_id;
    std::uint64_t object_id;
    std::size_t stream_offset;
    std::size_t stream_end_offset;
    std::uint64_t request_id;
};

using FetchEvent = std::variant<ObjectEvent, FetchRangeEvent>;

enum class FetchDecodePhase {
    Type,
    RequestId,
    SerializationFlags,
    GroupIdDelta,
    SubgroupId,
    ObjectIdDelta,
    Priority,
    PropertiesLength,
    Properties,
    PayloadLength,
    Payload,
    RangeGroupId,
    RangeObjectId,
};

struct FetchDecoderObservation {
    DecoderObservationKind kind;
    FetchDecodePhase phase;
    std::size_t offset;
    std::string detail;
};

struct FetchPushResult {
    std::optional<FetchHeader> header;
    std::vector<FetchEvent> events;
    std::vector<FetchDecoderObservation> observations;
    std::optional<DecodeError> error;
    bool clean_fin{false};
    bool local_api_misuse{false};
};

class FetchDecoder {
public:
    explicit FetchDecoder(FetchGroupOrderResolver group_order_resolver,
                          Limits limits = {});
    ~FetchDecoder();

    FetchDecoder(const FetchDecoder&) = delete;
    FetchDecoder& operator=(const FetchDecoder&) = delete;
    FetchDecoder(FetchDecoder&&) noexcept;
    FetchDecoder& operator=(FetchDecoder&&) noexcept;

    [[nodiscard]] FetchPushResult push(std::span<const std::byte> bytes,
                                       bool fin);
    [[nodiscard]] std::size_t buffered_byte_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
