#include "moq/interop/scenarios/draft22_location_filter_probes.h"

#include "draft22_probe_support.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/raw_probe_liveness.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft21/objects.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace moq::interop::scenarios {
namespace {

using namespace d22support;

// Section 9 message type and Section 9.20 parameter types.
constexpr std::uint64_t kSubscribe = 0x03;
constexpr std::uint64_t kSubscribeOk = 0x04;
constexpr std::uint64_t kLocationFilter = 0x21;
constexpr std::uint64_t kFillParameters = 0x23;
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

// Type 0x04: the same fields, then EndObject.
Bytes range_overflow() {
    auto value = bounded_overflow();
    value.front() = std::byte{0x04};
    integer(value, 0);  // EndObject
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

// Section 9.20.9: "Any other Location Filter Type is a PROTOCOL_VIOLATION." Type 0x06 is the first value
// after the six defined ones (0x00-0x05); no field follows, since none is defined for it.
constexpr std::uint64_t kUndefinedType = 0x06;

bool overflow(std::string_view id) {
    return id == kDraft22LocationFilterOverflow || id == kDraft22FillLocationFilterOverflow;
}

// Absolute Start (Type 0x02) {StartGroup 0, StartObject 0}, built by the draft 22 encoder. No draft 21 field
// list can express it ({0, 0} there is Next Object), so filter_param_value cannot either.
Bytes absolute_origin() {
    wire::ByteWriter writer(16);
    const wire::draft22::LocationFilter filter{wire::draft22::LocationFilterType::Absolute, 0, 0, {}, {}};
    if (wire::draft22::encode_location_filter(filter, writer)) throw std::logic_error("absolute origin filter");
    return {writer.bytes().begin(), writer.bytes().end()};
}

// The LOCATION_FILTER value the scenario sends.
Bytes filter_value(std::string_view id) {
    if (id == kDraft22LocationFilterUnknownType) {
        Bytes value;
        integer(value, kUndefinedType);
        return value;
    }
    if (id == kDraft22LocationFilterAbsoluteOrigin) return absolute_origin();
    return draft22_overflow_filter_value(id);
}

// The LOCATION_FILTER parameter as a first parameter (its type delta is the type).
Bytes filter_parameter(std::string_view id) {
    Bytes parameter;
    integer(parameter, kLocationFilter);
    const auto value = filter_value(id);
    parameter.insert(parameter.end(), value.begin(), value.end());
    return parameter;
}

Bytes stimulus(std::string_view id, const Fixture& fixture) {
    if (id != kDraft22FillLocationFilterOverflow) return subscribe(fixture, 1, filter_parameter(id));
    // Section 9.20.15: FILL_PARAMETERS is length-prefixed and holds Parameters encoded as for a separate
    // message (Section 16.7: no count, the first type delta is the type).
    const auto nested = filter_parameter(id);
    Bytes parameters;
    integer(parameters, kFillParameters);
    integer(parameters, nested.size());
    parameters.insert(parameters.end(), nested.begin(), nested.end());
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
    // No follow-up for the undefined Type: Section 9.20.9 names the PROTOCOL_VIOLATION without a BCP 14
    // keyword, so the unconditional MUST close the follow-up's argument needs is not stated there (a
    // publisher that keeps serving is not shown to break a MUST). Only its close code is judged.
    if (!overflow(id)) return definition;
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
    if (scenario_id == kDraft22FillLocationFilterOverflow) return range_overflow();
    return {};
}

RawProbeDefinition draft22_location_filter_overflow_probe(std::chrono::milliseconds deadline,
                                                          std::vector<std::vector<std::byte>> track_namespace,
                                                          std::vector<std::byte> track_name) {
    return build(kDraft22LocationFilterOverflow, deadline,
                 fixture_of(std::move(track_namespace), std::move(track_name)));
}

RawProbeDefinition draft22_fill_location_filter_overflow_probe(std::chrono::milliseconds deadline,
                                                               std::vector<std::vector<std::byte>> track_namespace,
                                                               std::vector<std::byte> track_name) {
    return build(kDraft22FillLocationFilterOverflow, deadline,
                 fixture_of(std::move(track_namespace), std::move(track_name)));
}

RawProbeDefinition draft22_location_filter_unknown_type_probe(std::chrono::milliseconds deadline,
                                                              std::vector<std::vector<std::byte>> track_namespace,
                                                              std::vector<std::byte> track_name) {
    return build(kDraft22LocationFilterUnknownType, deadline,
                 fixture_of(std::move(track_namespace), std::move(track_name)));
}

namespace {

// The draft 21 close rule (evaluate_raw_probe_close) on the stimulus rebuilt for the transcript's fixture.
std::optional<bool> judge_close(const RawProbeTranscript& t) {
    // The stimulus is rebuilt on the draft 22 wire; on any other wire nothing is judged.
    if (current_wire_draft() != 22 || t.writes.empty() || t.harness_failed) return std::nullopt;
    const auto fixture = recover_fixture(t.writes.front().write.bytes, kSubscribe, request_id(0));
    if (!fixture) return std::nullopt;
    auto expected = build(t.scenario_id, kRebuildDeadline, *fixture);
    bind_liveness_track(expected, fixture->ns, fixture->name);
    return evaluate_raw_probe_close(t, expected, kProtocolViolation);
}

}  // namespace

std::optional<bool> evaluate_draft22_location_filter_overflow(const RawProbeTranscript& t) {
    if (!overflow(t.scenario_id)) return std::nullopt;
    return judge_close(t);
}

std::optional<bool> evaluate_draft22_location_filter_unknown_type(const RawProbeTranscript& t) {
    if (t.scenario_id != kDraft22LocationFilterUnknownType) return std::nullopt;
    return judge_close(t);
}

RawProbeDefinition draft22_location_filter_absolute_origin_probe(std::chrono::milliseconds deadline,
                                                                 std::vector<std::vector<std::byte>> track_namespace,
                                                                 std::vector<std::byte> track_name) {
    return build(kDraft22LocationFilterAbsoluteOrigin, deadline,
                 fixture_of(std::move(track_namespace), std::move(track_name)));
}

std::optional<bool> evaluate_draft22_location_filter_absolute_origin(const RawProbeTranscript& t) {
    if (t.scenario_id != kDraft22LocationFilterAbsoluteOrigin) return std::nullopt;
    if (current_wire_draft() != 22 || t.writes.empty() || t.harness_failed) return std::nullopt;
    const auto fixture = recover_fixture(t.writes.front().write.bytes, kSubscribe, request_id(0));
    if (!fixture) return std::nullopt;
    const auto expected = build(t.scenario_id, kRebuildDeadline, *fixture);
    // A session close: the publisher failed the probe only when it closed with PROTOCOL_VIOLATION in reaction
    // to the request, i.e. it read a valid filter as malformed (Section 3.3.1: "A Location Filter on a
    // subscription is always valid"). Any other close says nothing about the filter.
    if (std::any_of(t.events.begin(), t.events.end(), [](const auto& event) {
            return std::holds_alternative<transport::PeerCloseEvent>(event);
        })) {
        if (!raw_probe_stimulus_valid(t, expected)) return std::nullopt;
        const auto close = observe_raw_probe_close(t, expected);
        if (close && close->error_code == kProtocolViolation) return false;
        return std::nullopt;
    }
    // No close: the window must have ended on a proven stimulus.
    const auto proven = prove(t, expected, true);
    if (!proven || !proven->ended) return std::nullopt;
    const auto& prefix = proven->prefix;
    const auto collected = collect(prefix.events);
    if (!collected.bounded) return std::nullopt;
    // Accepted: the first answer is SUBSCRIBE_OK, whose first field is the Track Alias (Section 9.7).
    const auto response = response_of(prefix, collected, 0);
    if (!response.stream || response.messages.malformed || response.messages.complete.empty() ||
        response.messages.complete.front().type != kSubscribeOk)
        return std::nullopt;
    wire::Cursor body(response.messages.complete.front().body);
    const auto alias = number(body);
    if (!alias) return std::nullopt;
    // Delivered: a complete Object for that alias on a subgroup stream opened after the request (Section
    // 11.3; draft 22 keeps the draft 21 data stream format). Every Location is inside a range that starts at
    // {0, 0}, so no Object can be outside it.
    const auto marker = *t.writes.front().delivery_event_count;
    const auto control = control_stream(collected.streams);
    for (const auto& [id, stream] : collected.streams) {
        if ((id & 3u) != 2u || id == control || stream.first_event < marker) continue;
        wire::draft21::SubgroupDecoder decoder;
        const auto decoded = decoder.push(stream.bytes, false);
        if (decoded.header && decoded.header->track_alias == *alias && !decoded.objects.empty()) return true;
    }
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
