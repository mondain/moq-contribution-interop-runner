#pragma once

// The moq-lite-06 Fetch scenarios (L2a Task B2). The publisher under test is the CLIENT; the runner is the server
// and the subscriber. Verdict rules, allowances and NotRun conditions come from the catalog rows cited per evaluator
// (requirements/moq-lite-06.json).
//
// Both scenarios need the track fixture (plan decision (d)); the builders copy the broadcast path and track name
// into the definition (requires_track = true) and the transcript carries them, so the evaluators rebuild the exact
// stimulus bytes. Both begin like a subscriber must (draft 5.1.3, 7.16): the announce exchange, then a Track Stream
// lookup for the track's TRACK_INFO (lite06_track.h), before anything is fetched.
//
// Every probe observes its whole stated window through an UNGATED trailing Wait labelled "allowance"
// (lite06::allowance_step). Each builder throws std::invalid_argument when the deadline does not exceed the time the
// probe needs (listed per builder) or a fixture value is empty.
//
// l06-fetch-group learns a COMPLETE group first, through the engine's dynamic next_steps continuation: a default
// SUBSCRIBE (Max Age 0, so the publisher starts at its latest group) until one Group stream of it ended with FIN
// and held at least kL06FetchMinFrames frames. That group G with N frames is the reference: the publisher
// demonstrably held it. The continuation then opens three Fetch Streams for G (whole group, a leading range and a
// trailing range) and observes the `allowance`. No such group within `learn_allowance` (seen on the next recorder
// change), or the learning stream ended or reset: no fetch is sent and every evaluator is NotRun. As in the
// subscribe probes, a silent learning subscription leaves the continuation open until the deadline, which sets
// timed_out, so the evaluators are NotRun then too.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "moq/interop/scenarios/lite06_common.h"
#include "moq/interop/scenarios/lite06_timing.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/wire/moqlite06/fetch.h"

namespace moq::interop::scenarios {

inline constexpr std::string_view kL06FetchGroup = "l06-fetch-group";
inline constexpr std::string_view kL06FetchUnknownGroup = "l06-fetch-unknown-group";

// Step labels.
inline constexpr std::string_view kL06FetchLearnLabel = "fetch-learn";          // the learning SUBSCRIBE (id 0)
inline constexpr std::string_view kL06FetchWholeLabel = "fetch-whole";          // Frame Start 0, Frame End 0
inline constexpr std::string_view kL06FetchLeadingLabel = "fetch-leading";      // Frame Start 0, Frame End 2
inline constexpr std::string_view kL06FetchTrailingLabel = "fetch-trailing";    // Frame Start 1, Frame End 0
inline constexpr std::string_view kL06FetchUnknownLabel = "fetch-unknown";      // a group that never existed

// The learning SUBSCRIBE's Subscribe ID.
inline constexpr std::uint64_t kL06FetchLearnSubscribeId = 0;
// A reference group needs at least this many frames for the leading and trailing ranges to be proper sub-ranges.
inline constexpr std::uint64_t kL06FetchMinFrames = 3;
// The leading range returns frames 0 and 1: Frame End is the absolute frame index + 1.
inline constexpr std::uint64_t kL06FetchLeadingEnd = 2;
// The trailing range starts at frame 1 and runs to the end of the group.
inline constexpr std::uint64_t kL06FetchTrailingStart = 1;
// The group sequence l06-fetch-unknown-group asks for: far beyond anything a track produces in a test run.
inline constexpr std::uint64_t kL06FetchUnknownGroupSequence = 4'000'000'000ULL;
// The wait for a complete group to learn (a group lasts as long as the source's GOP).
inline constexpr std::chrono::milliseconds kL06FetchLearnAllowance{15000};

// A FETCH for the fixture at Subscriber Priority 0.
wire::moqlite06::FetchRequest l06_fetch_request(std::string_view broadcast_path, std::string_view track_name,
                                                std::uint64_t group_sequence, std::uint64_t frame_start,
                                                std::uint64_t frame_end);
// STREAM_TYPE 0x3 then the FETCH (encode_fetch_request; empty when it refuses the message).
std::vector<std::byte> l06_fetch_bytes(const wire::moqlite06::FetchRequest& request);
// The FETCH a recorded Fetch-stream stimulus carries (STREAM_TYPE 0x3 then a whole FETCH, nothing after); nullopt
// otherwise.
std::optional<wire::moqlite06::FetchRequest> l06_decode_fetch_stimulus(std::span<const std::byte> bytes);

// l06-fetch-group: announce exchange, Track Stream lookup (each gated, at most `answer_allowance`), the learning
// SUBSCRIBE, then the continuation described above, then the `allowance`. Needs
// deadline > 2 * answer_allowance + learn_allowance + allowance.
LiteProbeDefinition l06_fetch_group_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                          std::string_view track_name,
                                          std::chrono::milliseconds allowance = kLiteResponseAllowance,
                                          std::chrono::milliseconds learn_allowance = kL06FetchLearnAllowance,
                                          std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);
// l06-fetch-unknown-group: announce exchange, Track Stream lookup, one FETCH for kL06FetchUnknownGroupSequence
// (whole group), then the response `allowance`. Needs deadline > 2 * answer_allowance + allowance.
LiteProbeDefinition l06_fetch_unknown_group_probe(std::chrono::milliseconds deadline,
                                                  std::string_view broadcast_path, std::string_view track_name,
                                                  std::chrono::milliseconds allowance = kLiteResponseAllowance,
                                                  std::chrono::milliseconds answer_allowance = kLiteResponseAllowance);

// L06-7-16-MUST-177 (scenario l06-fetch-group).
std::optional<bool> evaluate_l06_fetch_short_run(const LiteTranscript& transcript);
// L06-5-1-3-MUST-066 (scenario l06-fetch-unknown-group).
std::optional<bool> evaluate_l06_fetch_unknown_group_reset(const LiteTranscript& transcript);

}  // namespace moq::interop::scenarios
