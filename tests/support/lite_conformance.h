#pragma once

// Every moq-lite-06 scenario run once against the conforming scripted publisher (tests/support/scripted_lite_peer.h)
// on a manual clock: the transcripts of the end-to-end conformance table (tests/protocol/lite_conformance_test.cpp)
// and of the staged-scoring tests (tests/unit/lite_evaluators_test.cpp).

#include <chrono>
#include <functional>
#include <utility>
#include <string>
#include <vector>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_datagram.h"
#include "moq/interop/scenarios/lite06_errors.h"
#include "moq/interop/scenarios/lite06_fetch.h"
#include "moq/interop/scenarios/lite06_goaway.h"
#include "moq/interop/scenarios/lite06_probe.h"
#include "moq/interop/scenarios/lite06_setup.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/scenarios/lite06_track.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "support/scripted_lite_peer.h"

namespace moq::interop::test::lite {

inline constexpr std::chrono::milliseconds kConformanceTick{10};
// Above every builder's stated sum (the largest: fetch-group 2 * 3 s + 15 s + 3 s; goaway-single 3 s + 10 s + 6 s).
inline constexpr std::chrono::milliseconds kConformanceDeadline{40000};
inline const std::string kConformanceBroadcast = "demo/live";
inline const std::string kConformanceTrack = "video";
inline const std::string kConformanceUrlPath = "/moq";
inline const std::string kConformanceUrlQuery = "token=l1d";
inline constexpr std::uint64_t kConformanceHopId = 7;

// The conforming publisher as a client of `binding`: the fixture broadcast and track, a SETUP with Hop and Cost
// parameters (plus Path from the session URL on native QUIC), Hop ID 7 in its ANNOUNCE_OKs.
inline ConformingLitePublisherConfig conformance_publisher_config(scenarios::LiteBinding binding) {
    ConformingLitePublisherConfig config;
    config.broadcast = kConformanceBroadcast;
    config.track = kConformanceTrack;
    config.hop_id = kConformanceHopId;
    // Two parameters on every binding, so row 111 (unique Parameter IDs) has something to judge without Path.
    config.setup_parameters = {{l06::kParamHop, Bytes{std::byte{kConformanceHopId}}},
                               {l06::kParamCost, Bytes{std::byte{0x0}}}};
    config.session_url_path = kConformanceUrlPath;
    config.session_url_query = kConformanceUrlQuery;
    config.binding = binding;
    // L2a. Every group has four frames, so the fetch probe learns a group with the three frames its ranges need and
    // every group the subscriptions deliver is one FETCH holds; the publisher advertises Probe level Report (so
    // row 072 is judged and row 075, which needs level None, is NotRun on the conforming table).
    config.frames_per_group = 4;
    config.fetch_frames_per_group = 4;
    config.fetch_last_group = 1000;
    config.setup_parameters.push_back(l06::SetupParameter{l06::kParamProbe, Bytes{std::byte{1}}});
    return config;
}

// The 27 scenario definitions with the default allowances and kConformanceDeadline, each carrying `binding`.
inline std::vector<scenarios::LiteProbeDefinition> conformance_probes(scenarios::LiteBinding binding) {
    namespace s = scenarios;
    const auto d = kConformanceDeadline;
    const auto& path = kConformanceBroadcast;
    const auto& track = kConformanceTrack;
    std::vector<s::LiteProbeDefinition> probes{
        s::l06_setup_stream_probe(d),
        s::l06_setup_unknown_parameter_probe(d),
        s::l06_setup_duplicate_parameter_probe(d),
        s::l06_setup_duplicate_stream_probe(d),
        s::l06_setup_server_path_probe(d),
        s::l06_setup_server_role_probe(d),
        s::l06_announce_prefix_probe(d, path),
        s::l06_announce_lifecycle_probe(d, path),
        s::l06_session_stream_close_probe(d, path, track),
        s::l06_subscribe_latest_probe(d, path, track),
        s::l06_subscribe_refused_probe(d, path, track),
        s::l06_subscribe_invalid_frame_bounds_probe(d, path, track),
        s::l06_subscribe_group_floor_probe(d, path, track),
        s::l06_subscribe_abutting_frame_start_probe(d, path, track),
        s::l06_errors_unknown_stream_type_probe(d),
        s::l06_errors_unknown_reset_code_probe(d, path, track),
        s::l06_errors_reserved_reset_code_probe(d, path, track),
        s::l06_errors_code_space_probe(d),
        s::l06_setup_client_path_probe(d, kConformanceUrlPath, kConformanceUrlQuery),
        s::l06_track_info_probe(d, path, track),
        s::l06_fetch_group_probe(d, path, track),
        s::l06_fetch_unknown_group_probe(d, path, track),
        s::l06_probe_report_probe(d),
        s::l06_datagram_size_probe(d, path, track),
        s::l06_goaway_single_probe(d, path, track),
        s::l06_goaway_duplicate_probe(d),
        s::l06_goaway_oversize_probe(d),
    };
    for (auto& probe : probes) probe.binding = binding;
    return probes;
}

// `tweak` edits the publisher configuration (tests that show what the table depends on).
inline scenarios::LiteTranscript run_conforming(
    scenarios::LiteProbeDefinition definition, scenarios::LiteBinding binding,
    const std::function<void(ConformingLitePublisherConfig&)>& tweak = {}) {
    auto config = conformance_publisher_config(binding);
    // Datagrams (draft 6.4) carry a single-frame group, which the four-frame groups of the other scenarios never are.
    if (definition.id == scenarios::kL06DatagramSize) {
        config.datagrams = true;
        config.frames_per_group = 1;
    }
    if (tweak) tweak(config);
    ConformingLitePublisher publisher(std::move(config));
    ScriptedLitePeer peer(publisher.reaction());
    scenarios::ManualLiteClock clock;
    return run_lite_probe(peer, std::move(definition), clock, kConformanceTick);
}

// One transcript per scenario (27), in conformance_probes order, all on `binding`.
inline std::vector<scenarios::LiteTranscript> conformance_transcripts(
    scenarios::LiteBinding binding = scenarios::LiteBinding::NativeQuic) {
    std::vector<scenarios::LiteTranscript> out;
    for (auto& probe : conformance_probes(binding)) out.push_back(run_conforming(std::move(probe), binding));
    return out;
}

}  // namespace moq::interop::test::lite
