#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace moq::interop::scenarios {

// The track a draft 21 family probe names in its SUBSCRIBE or TRACK_STATUS. Draft 21 probes name their
// own track, namespace () and track "x"; on the draft 22 wire the probes that draft 22 runs share name the
// run's track fixture instead, because a publisher may refuse an empty namespace before it reads the
// parameter a probe is about (imquic closes 0x3 "Invalid number of namespaces").
struct ProbeTrackNames {
    std::vector<std::vector<std::byte>> track_namespace;
    std::vector<std::byte> track_name{std::byte{'x'}};
};

// The names a probe built now uses: the run's names on wire draft 22 (current_wire_draft()) when a track
// name is given, otherwise the probe's own (), "x". Draft 21 is frozen, so wire 21 always gets the own names.
ProbeTrackNames probe_track_names(std::vector<std::vector<std::byte>> track_namespace,
                                  std::vector<std::byte> track_name);

// Track Namespace (field count and length-prefixed fields) followed by the length-prefixed Track Name, as
// SUBSCRIBE and TRACK_STATUS carry them after their Request ID (draft 21 and 22 alike). For (), "x" this is
// 00 01 78. Throws std::invalid_argument when the names cannot fit one probe frame.
std::vector<std::byte> encode_probe_track_names(const ProbeTrackNames& names);

// The names in the first message of `input`: a request frame (type, 16-bit length) whose body starts with a
// Request ID, a Track Namespace and a Track Name. The rest of the message is not read and may be truncated
// or malformed (it is the probe's stimulus). A caller accepts the result only when rebuilding its probe
// from it reproduces `input` exactly.
std::optional<ProbeTrackNames> recover_probe_track_names(std::span<const std::byte> input);

}  // namespace moq::interop::scenarios
