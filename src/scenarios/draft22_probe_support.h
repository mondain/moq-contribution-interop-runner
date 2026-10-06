#pragma once

// Internal: plumbing shared by the draft 22 own probes and their evaluators (draft22_*.cpp): request
// framing (Section 9), Track Namespace encoding (Section 8.7), per-stream collection of what the
// publisher sent, request-stream message parsing, fixture recovery and the proof of a recorded stimulus.
// The request-stream framing and the data stream formats of draft 22 are those of draft 21, so the
// draft 21 SETUP decoder is used to recognise the publisher's control stream.

#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/cursor.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace moq::interop::scenarios::d22support {

using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;

// Evidence bounds of one evaluation (the draft 21 gap A bounds).
inline constexpr std::size_t kMaximumBytes = 65546;
inline constexpr std::size_t kMaximumStreams = 64;
// Deadline given to a probe rebuilt only to prove a recorded stimulus (the deadline is not part of it).
inline constexpr std::chrono::milliseconds kRebuildDeadline{1000};

// The runner's complete draft 22 SETUP (Section 9.1): Type 0x2F00, Length 0, no Setup Options.
Bytes setup_message();

struct Fixture {
    Namespace ns;
    Bytes name;
};

void integer(Bytes& output, std::uint64_t value);
// Section 9: Type (vi64), Length (16), body.
Bytes frame(std::uint64_t type, const Bytes& body);
// Section 8.7: Number of Track Namespace Fields, then each field length-prefixed.
void track_namespace(Bytes& body, const Namespace& ns);
// Track Namespace, then Track Name Length and Track Name.
void track(Bytes& body, const Fixture& fixture);
std::optional<std::uint64_t> number(wire::Cursor& cursor);
// Reads a Track Namespace (Section 8.7) that respects its limits: at most 32 fields, none empty, at most
// 4096 bytes in all. No value when it does not.
std::optional<Namespace> read_namespace(wire::Cursor& cursor);

// Whether `input` begins with a complete SETUP (the publisher's control stream, Section 6.3).
bool setup_ready(std::span<const std::byte> input);

// Request IDs of the runner's (server-parity) requests: 1, 3, 5, ... (Section 6.4.2.1).
inline std::uint64_t request_id(std::size_t index) { return 1 + 2 * static_cast<std::uint64_t>(index); }

struct Message {
    std::uint64_t type;
    Bytes frame;  // type, length and body, as received
    Bytes body;
};
struct Messages {
    std::vector<Message> complete;
    bool malformed{false};
};
// Request-stream messages are Type (vi64), Length (16), body (Section 9). A trailing partial message is
// left out.
Messages parse_messages(std::span<const std::byte> input);

struct StreamData {
    Bytes bytes;
    std::size_t first_event{0};  // first event of any kind (data, reset, STOP_SENDING)
    std::optional<std::size_t> first_data_event;  // first event that carried bytes or FIN
    bool fin{false};
    bool reset{false};
    std::size_t reset_event{0};
    bool stop_sending{false};  // the publisher sent STOP_SENDING for the runner's direction
    std::size_t stop_event{0};
    bool overrun{false};  // data arrived after FIN or reset
};
using Streams = std::map<transport::StreamId, StreamData>;

bool terminal(const transport::TransportEvent& event);

struct Collected {
    Streams streams;
    bool bounded{true};
};
struct CollectLimits {
    std::size_t maximum_bytes{kMaximumBytes};
    // Count and keep only bidirectional (request) streams, so a publisher's media cannot exhaust the bound.
    bool request_streams_only{false};
};
// Every stream the publisher wrote, reset or stopped before the session ended, within the bounds.
Collected collect(std::span<const transport::TransportEvent> events, CollectLimits limits = {});

// The publisher's control stream: the first peer unidirectional stream whose bytes decode as SETUP.
std::optional<transport::StreamId> control_stream(const Streams& streams);

struct Response {
    const StreamData* stream{nullptr};
    Messages messages;
};
// The response direction of write `index`, when it began after the request was fully accepted.
Response response_of(const RawProbeTranscript& t, const Collected& collected, std::size_t index);

// Throws std::logic_error unless scenarios::current_wire_draft() is 22.
void require_draft22_wire(const char* what);

// The track identity of a recorded first request: a single frame of `type` whose Request ID is
// `expected_request` and whose next field is the Track Namespace, then the Track Name, which together must
// pass fetch_first_object_fixture_valid. Only the track identity is configurable; every other byte is
// regenerated and compared by the proof.
std::optional<Fixture> recover_fixture(std::span<const std::byte> input, std::uint64_t type,
                                       std::uint64_t expected_request);

// A transcript cut at its first session terminal, once proven to carry exactly `expected`'s stimulus.
struct Proven {
    RawProbeTranscript prefix;
    bool ended{false};  // the window ended: a timeout, or a session terminal
};
// No value for another scenario, an incomplete context that did not time out, or a stimulus that does
// not match `expected`. `window` says whether a timed-out context still carries a full observation.
std::optional<Proven> prove(const RawProbeTranscript& t, const RawProbeDefinition& expected, bool window);

// For a context that timed out before its whole stimulus went out (a later write waited on a gate that
// never opened): the transcript cut at its first session terminal and restricted to the leading writes
// that were delivered, once proven to carry exactly those writes of `expected`. The publisher saw only
// those writes, so what follows them is evidence about them alone. No value when no write was delivered,
// the context did not time out, or its whole stimulus was delivered (use prove()).
std::optional<RawProbeTranscript> prove_delivered(const RawProbeTranscript& t, RawProbeDefinition expected);

}  // namespace moq::interop::scenarios::d22support
