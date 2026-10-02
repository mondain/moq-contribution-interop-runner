#pragma once

// Wire and parse helpers shared by the residual-slice translation units
// (subscription, publisher-observation and token rows). Private to src/scenarios.

#include "draft21_contribution_support.h"

#include <optional>
#include <vector>

namespace moq::interop::scenarios::d21c::residual {

constexpr std::uint64_t kPublish = 0x1d;
constexpr std::uint64_t kRequestUpdate = 0x2;

Bytes location_pair(std::uint64_t group, std::uint64_t object);
// PUBLISH_NAMESPACE announcements are acknowledged by base_definition().
RawProbeDefinition residual_definition();

struct Object {
    std::uint64_t group{0};
    std::uint64_t id{0};
    bool data{true};  // false for an Object Status
    transport::StreamId stream{0};
    bool datagram{false};
    Bytes payload;
};

// Section 11.3.1: the Subgroup streams (unidirectional, publisher-initiated)
// carrying `alias`, each with the Objects received so far.
struct AliasStream {
    transport::StreamId id{0};
    shared::SubgroupParse parsed;
};
std::vector<AliasStream> subgroup_streams(const View& view, std::uint64_t alias);
// Section 11.2: Object Datagram. Only the fields needed to place the Object.
std::optional<Object> parse_datagram_object(const DatagramRecord& datagram, std::uint64_t alias);
std::vector<Object> delivered_objects(const View& view, std::uint64_t alias);
// The Track Alias is the first field of SUBSCRIBE_OK (Section 9.7).
std::optional<std::uint64_t> alias_of(const View& view, std::size_t write);
bool rejected(const View& view, std::size_t write);

struct TrackName {
    Namespace track_namespace;
    Bytes name;
    bool operator==(const TrackName& other) const {
        return track_namespace == other.track_namespace && name == other.name;
    }
};
std::optional<Namespace> read_namespace(wire::Cursor& cursor);

struct PublishRecord {
    transport::StreamId stream{0};
    std::size_t first_event{0};
    TrackName track;
    std::uint64_t alias{0};
    // The first of the publisher's FIN, its reset or its PUBLISH_DONE on the stream.
    std::optional<std::size_t> terminated;
    // The runner's volunteered answer to this PUBLISH.
    std::optional<RawProbeCourtesyKind> response;
    std::size_t response_event{0};
    std::size_t updates{0};  // REQUEST_UPDATE messages after the PUBLISH
};
std::optional<std::size_t> earliest(std::optional<std::size_t> left, std::optional<std::size_t> right);
// PUBLISH messages the publisher opened as requests, with the runner's courtesy answer.
std::vector<PublishRecord> publish_records(const View& view);

// A context that sends nothing and answers what the publisher opens.
RawProbeDefinition observing(RawProbeCourtesy courtesy);
RawProbeCourtesy accepting_publishes();

}  // namespace moq::interop::scenarios::d21c::residual
