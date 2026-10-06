#include "moq/interop/scenarios/draft22_location_filter_probes.h"

#include "draft22_probe_support.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/raw_probe_liveness.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace moq::interop::scenarios {
namespace {

using namespace d22support;

// Section 9 message type and Section 9.20 parameter types.
constexpr std::uint64_t kSubscribe = 0x03;
constexpr std::uint64_t kLocationFilter = 0x21;
// Section 16.11.1: the session close code the row requires.
constexpr std::uint64_t kProtocolViolation = 0x3;

constexpr std::uint64_t kLargest = std::numeric_limits<std::uint64_t>::max();

// Section 9.20.9: Location Filter Type, then StartGroup, StartObject, EndGroupDelta (Type 0x03). StartGroup
// 2^64 - 1 plus EndGroupDelta 1 exceeds 2^64 - 1 by one, the smallest overflow. Written by hand:
// wire::draft22::encode_location_filter refuses it, and the decoder must reject exactly these bytes
// (tests/protocol/draft22_location_filter_probes_test.cpp).
Bytes bounded_overflow() {
    Bytes value;
    integer(value, 0x03);
    integer(value, kLargest);  // StartGroup
    integer(value, 0);         // StartObject
    integer(value, 1);         // EndGroupDelta
    return value;
}

// Section 9.6: SUBSCRIBE (Request ID 1) for the track, carrying `parameters` (already delta encoded).
Bytes subscribe(const Fixture& fixture, std::uint64_t count, const Bytes& parameters) {
    Bytes body;
    integer(body, request_id(0));
    track(body, fixture);
    integer(body, count);
    body.insert(body.end(), parameters.begin(), parameters.end());
    return frame(kSubscribe, body);
}

Bytes stimulus(std::string_view id, const Fixture& fixture) {
    Bytes parameters;
    integer(parameters, kLocationFilter);  // the first parameter: its type delta is the type
    const auto value = draft22_overflow_filter_value(id);
    parameters.insert(parameters.end(), value.begin(), value.end());
    return subscribe(fixture, 1, parameters);
}

RawProbeDefinition build(std::string_view id, std::chrono::milliseconds deadline, const Fixture& fixture) {
    require_draft22_wire("draft 22 location filter");
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid draft 22 location filter probe fixture or deadline");
    RawProbeDefinition definition;
    definition.id = std::string(id);
    definition.setup_bytes = setup_message();
    definition.peer_setup_ready = setup_ready;
    definition.deadline = deadline;
    definition.writes.push_back({RawProbeChannel::NewBidi, stimulus(id, fixture), false});
    // The liveness follow-up of the draft 21 counterparts (both listed in raw_probe_liveness.cpp): the row
    // is an unconditional MUST close, the stimulus is one complete message on a reliable stream, and a
    // SUBSCRIBE_OK on a fresh request after it shows the session kept serving. Draft 22 SUBSCRIBE and
    // SUBSCRIBE_OK keep the draft 21 encoding (Appendix A.1 changes only LOCATION_FILTER), so the answer is
    // recognised by the draft 21 rules. The run binds the request to the track fixture
    // (bind_liveness_track); the evaluator binds the recovered one.
    RawProbeLiveness liveness;
    liveness.draft = 21;
    definition.liveness = std::move(liveness);
    return definition;
}

Fixture fixture_of(std::vector<std::vector<std::byte>> track_namespace, std::vector<std::byte> track_name) {
    return {std::move(track_namespace), std::move(track_name)};
}

}  // namespace

std::vector<std::byte> draft22_overflow_filter_value(std::string_view scenario_id) {
    if (scenario_id == kDraft22LocationFilterOverflow) return bounded_overflow();
    return {};
}

RawProbeDefinition draft22_location_filter_overflow_probe(std::chrono::milliseconds deadline,
                                                          std::vector<std::vector<std::byte>> track_namespace,
                                                          std::vector<std::byte> track_name) {
    return build(kDraft22LocationFilterOverflow, deadline,
                 fixture_of(std::move(track_namespace), std::move(track_name)));
}

std::optional<bool> evaluate_draft22_location_filter_overflow(const RawProbeTranscript& t) {
    if (t.scenario_id != kDraft22LocationFilterOverflow) return std::nullopt;
    // The stimulus is rebuilt on the draft 22 wire; on any other wire nothing is judged.
    if (current_wire_draft() != 22 || t.writes.empty() || t.harness_failed) return std::nullopt;
    const auto fixture = recover_fixture(t.writes.front().write.bytes, kSubscribe, request_id(0));
    if (!fixture) return std::nullopt;
    auto expected = build(t.scenario_id, kRebuildDeadline, *fixture);
    bind_liveness_track(expected, fixture->ns, fixture->name);
    return evaluate_raw_probe_close(t, expected, kProtocolViolation);
}

}  // namespace moq::interop::scenarios
