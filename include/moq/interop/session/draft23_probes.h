#pragma once

#include "moq/interop/wire/draft21/key_values.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace moq::interop::session {

// Where a negative probe's malformed bytes go.
enum class Draft23ProbeChannel {
    Bidi,         // a new bidirectional stream the runner opens after the publisher's SETUP
    Uni,          // a new unidirectional stream the runner opens after the publisher's SETUP
    SetupOption,  // an option the runner puts in its own SETUP, which only a client may send
};

// A negative probe sends the publisher input a conformant endpoint must reject and expects the publisher to close the
// session with `expected_code`. Each probe ends the session, so each is its own scenario and its own connection.
struct Draft23Probe {
    std::string_view scenario_id;
    std::string_view requirement_id;
    Draft23ProbeChannel channel;
    std::uint64_t expected_code;
};

std::span<const Draft23Probe> draft23_probes();
const Draft23Probe* find_draft23_probe(std::string_view scenario_id);

// The bytes a Bidi or Uni probe writes. `peer_message_limit` is the publisher's MAX_CONTROL_MESSAGE_SIZE (65535 when
// it sent none); the oversize probe declares one byte more.
std::vector<std::byte> draft23_probe_bytes(const Draft23Probe& probe, std::uint64_t peer_message_limit);

// Appends the forbidden option a SetupOption probe adds to the runner's SETUP; other probes add nothing.
void draft23_probe_setup_options(const Draft23Probe& probe, wire::draft21::KeyValues& options);

}  // namespace moq::interop::session
