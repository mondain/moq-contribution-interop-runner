#pragma once

#include "moq/interop/wire/draft21/key_values.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::session {

// What the runner does to the publisher.
enum class Draft23ProbeChannel {
    Bidi,           // each payload is the first bytes of a new request stream the runner opens after the publisher's SETUP
    Uni,            // a new unidirectional stream
    SetupOption,    // options added to the runner's own SETUP (client-only options, unknown options)
    SetupBytes,     // the runner's whole SETUP is replaced by the payload
    ControlFin,     // the runner finishes its control stream after the publisher's SETUP
    Datagram,       // one datagram
    AnnounceReply,  // the payload answers the publisher's PUBLISH_NAMESPACE in place of REQUEST_OK
    PublishReply,   // the payload answers the publisher's PUBLISH for a track the harness declines, in place of its REQUEST_ERROR
    AnnounceExtra,  // the payload follows the REQUEST_OK on the publisher's announcement stream
    PublishExtra,   // after the harness accepts the publisher's PUBLISH for its own track, the payload follows the REQUEST_OK
    ControlBytes,   // the payload follows the runner's SETUP on its control stream
    SubscribeExtra, // after SUBSCRIBE_OK, the payload goes on the runner's SUBSCRIBE stream (needs a direct subscription)
};

// What the publisher must do in answer.
enum class Draft23ProbeExpect {
    Close,         // close the session with `expected_code`
    RequestError,  // reject the request with a REQUEST_ERROR carrying `expected_code`, leaving the session open
    Tolerate,      // ignore the input and keep the session open
};

// A negative probe sends the publisher input a conformant endpoint must reject or tolerate. Most end the session, so
// each probe is its own scenario and its own connection.
struct Draft23Probe {
    std::string_view scenario_id;
    std::string_view requirement_id;                      // the primary row the probe judges
    std::array<std::string_view, 3> more_requirement_ids; // further rows the same probe judges; unused entries are empty
    std::string_view evaluator_id;
    Draft23ProbeChannel channel;
    Draft23ProbeExpect expect;
    std::uint64_t expected_code;  // close code, REQUEST_ERROR code, or 0 for Tolerate
};

// What the publisher told the runner about its limits in SETUP, which some probes size their input from.
struct Draft23PeerLimits {
    std::uint64_t control_message_size{65535};  // MAX_CONTROL_MESSAGE_SIZE
    std::uint64_t filter_ranges{0};             // MAX_FILTER_RANGES
    // The track the publisher announced. Requests that should be valid but for one defect name it, so the defect is the
    // only reason to reject them; both default to a placeholder track when empty.
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name;
};

std::span<const Draft23Probe> draft23_probes();
const Draft23Probe* find_draft23_probe(std::string_view scenario_id);

// Every requirement id the probe judges, primary first.
std::vector<std::string_view> draft23_probe_rows(const Draft23Probe& probe);

// The bytes the probe sends, one entry per stream, datagram or write. `peer_message_limit` is the publisher's
// MAX_CONTROL_MESSAGE_SIZE (65535 when it sent none); the oversize probe declares one byte more.
std::vector<std::vector<std::byte>> draft23_probe_payloads(const Draft23Probe& probe, std::uint64_t peer_message_limit);
std::vector<std::vector<std::byte>> draft23_probe_payloads(const Draft23Probe& probe, const Draft23PeerLimits& limits);

// The first payload, for probes that send one.
std::vector<std::byte> draft23_probe_bytes(const Draft23Probe& probe, std::uint64_t peer_message_limit);

// Appends the options a SetupOption probe adds to the runner's SETUP; other probes add nothing.
void draft23_probe_setup_options(const Draft23Probe& probe, wire::draft21::KeyValues& options);

// A SUBSCRIBE built by hand, which the probes use to make deliberately malformed variants. Exposed so a test can pin
// that it matches the encoder for a valid message.
std::vector<std::byte> draft23_hand_built_subscribe(std::uint64_t request_id, const std::vector<std::string>& namespace_fields,
                                                    const std::string& track_name);

}  // namespace moq::interop::session
