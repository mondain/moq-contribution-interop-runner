#pragma once

// Shared helpers for the draft-18 gap-A raw probes. Every helper derives its
// behaviour from the draft text; nothing here is shared with draft 21.

#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/draft18/messages.h"
#include "moq/interop/wire/draft18/objects.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::scenarios::gap_a {

namespace d18 = wire::draft18;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;

inline constexpr std::size_t kMaximumEvents = 4096;
inline constexpr std::size_t kMaximumBytes = 65546;

struct Fixture {
    Namespace track_namespace;
    Bytes track_name;
};

// Namespace and name are the only operator-controlled inputs.
bool fixture_valid(const Fixture& fixture);
Fixture canonical_fixture(Namespace track_namespace, Bytes track_name);

Bytes encode(const d18::Message& message);
Bytes setup_frame(const d18::KeyValuePairs& options = {});
bool setup_ready(std::span<const std::byte> bytes);
bool terminal(const transport::TransportEvent& event);

struct Frame {
    std::uint64_t type{0};
    std::size_t offset{0};
    std::size_t size{0};
};
struct SplitFrames {
    std::vector<Frame> frames;
    bool malformed{false};
    std::size_t trailing_bytes{0};
};
// Splits `type (vi64) | length (16) | payload` frames; a partial tail stays
// in trailing_bytes and is never reported as malformed.
SplitFrames split_frames(std::span<const std::byte> bytes);
std::optional<d18::Message> decode_frame(std::span<const std::byte> bytes,
                                         const Frame& frame);
// The wire decoder's rejection detail for a complete frame, if it rejects it.
std::optional<std::string> frame_decode_error(std::span<const std::byte> bytes, const Frame& frame);

struct StreamData {
    Bytes bytes;
    std::size_t first_event{0};
    bool fin{false};
    bool reset{false};
    std::optional<std::size_t> fin_event;
    std::optional<std::size_t> reset_event;
};
using Streams = std::map<transport::StreamId, StreamData>;
// Bounded, order-preserving per-stream view of the first `end` events.
std::optional<Streams> collect_streams(
    std::span<const transport::TransportEvent> events, std::size_t end);
std::optional<Streams> collect_streams(
    std::span<const transport::TransportEvent> events);

// The peer unidirectional stream whose bytes begin with a decodable SETUP.
struct PeerControl {
    transport::StreamId stream_id;
    d18::SetupMessage setup;
};
std::optional<PeerControl> peer_control(const Streams& streams);

// Decoded frames of one stream; `complete` messages only.
struct StreamMessages {
    std::vector<d18::Message> messages;
    std::vector<std::uint64_t> types;
    bool malformed{false};
    bool undecodable{false};
};
StreamMessages stream_messages(const StreamData& stream);

bool is_peer_bidi(transport::StreamId id);
bool is_peer_uni(transport::StreamId id);
bool is_local_bidi(transport::StreamId id);

// Largest location advertised in a LARGEST_OBJECT parameter, if present.
std::optional<d18::Location> largest_object(const d18::Parameters& parameters);
bool location_less(const d18::Location& left, const d18::Location& right);

d18::TrackNamespace track_namespace(const Namespace& fields);
Namespace namespace_fields(const d18::TrackNamespace& value);

// Decoded Subgroup stream for one subscription alias.
struct SubgroupStream {
    transport::StreamId stream_id;
    d18::SubgroupHeader header;
    std::vector<d18::ObjectEvent> objects;
    bool fin{false};
    bool reset{false};
    bool decode_error{false};
};
std::vector<SubgroupStream> subgroup_streams(const Streams& streams,
                                             std::uint64_t track_alias);

// Types of requests we open (our request IDs are odd: we play the server).
struct TypedRequest {
    std::uint64_t type;
    Bytes bytes;
};

// Response helpers for a request stream we opened.
const StreamData* local_stream(const Streams& streams,
                               const RawProbeAcceptedWrite& write);
std::optional<std::size_t> write_marker(const RawProbeAcceptedWrite& write);

// Standard request builders (SETUP-free frames).
Bytes subscribe_request(std::uint64_t request_id, const Fixture& fixture,
                        d18::Parameters parameters = {});
Bytes track_status_request(std::uint64_t request_id, const Fixture& fixture,
                           d18::Parameters parameters = {});
Bytes subscribe_namespace_request(std::uint64_t request_id,
                                  const Namespace& prefix,
                                  d18::Parameters parameters = {});
Bytes subscribe_tracks_request(std::uint64_t request_id,
                               const Namespace& prefix,
                               d18::Parameters parameters = {});
Bytes standalone_fetch_request(std::uint64_t request_id,
                               const Fixture& fixture, d18::Location start,
                               d18::Location end);
Bytes joining_fetch_request(std::uint64_t request_id,
                            std::uint64_t joining_request_id,
                            std::uint64_t joining_start, bool relative);
Bytes request_update(std::uint64_t request_id, d18::Parameters parameters);

d18::Parameter forward_parameter(std::uint8_t value);
d18::Parameter priority_parameter(std::uint8_t value);
d18::Parameter filter_parameter(d18::SubscriptionFilter filter);
d18::Parameter token_parameter(d18::Token token);

// Recover a Fixture from the first stimulus write of an actual transcript.
enum class FirstWrite { Subscribe, TrackStatus, Fetch, Prefix };
std::optional<Fixture> recover_fixture(FirstWrite kind,
                                       std::span<const std::byte> bytes);

// The Object 7/9 of a FETCH we sent, once FETCH_OK and the complete Object
// were both observed on that request's own response and data streams.
std::optional<d18::ObjectEvent> fetched_object(
    const Streams& streams, const RawProbeAcceptedWrite& fetch_write);
// Observed "Object has been published" precondition for the LARGEST_OBJECT
// probes.
bool fetch_object_observed(const RawProbeAcceptedWrite& fetch_write,
                           std::span<const transport::TransportEvent> events);

// Peer-initiated bidirectional streams, a PUBLISH_NAMESPACE or a PUBLISH
// message as the first frame.
std::optional<std::uint64_t> application_close_code(
    std::span<const transport::TransportEvent> events);
// First message of a local request stream written by `write`.
const d18::Message* first_response(const StreamMessages& messages);
std::optional<StreamMessages> messages_of(const Streams& streams,
                                          const RawProbeAcceptedWrite& write);
bool is_final_response(const d18::Message& message);

struct Observation {
    bool ready{false};
    std::optional<bool> result;
};

struct Entry {
    std::string requirement_id;
    std::string scenario_id;
    std::string evaluator_id;
    bool requires_track{true};
    bool native_only{false};
    // How the fixture is recovered from the transcript's first write; none
    // for probes that send no fixture-bearing request.
    std::optional<FirstWrite> recover;
    std::function<RawProbeDefinition(const Fixture&, std::chrono::milliseconds)> build;
    std::function<Observation(const RawProbeTranscript&, const Fixture&)> observe;
};
std::vector<Entry> setup_entries();
std::vector<Entry> discovery_entries();
std::vector<Entry> object_entries();
// Slice B (draft18_gap_b.cpp).
std::vector<Entry> gap_b_entries();

RawProbeWrite make_write(RawProbeChannel channel, Bytes bytes, bool fin = false);
RawProbeDefinition make_definition(std::string id, Bytes setup,
                                   std::chrono::milliseconds deadline);


}  // namespace moq::interop::scenarios::gap_a
